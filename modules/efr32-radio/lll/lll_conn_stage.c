/*
 * Connection replies staged before the frame ends (docs/radio-tables.md 7.82). Included at the end
 * of the derived lll_conn.c (CMakeLists.txt), so it reads lll_conn.c's own state (crc_expire) and
 * calls its functions; the patches there call into it. SPDX-License-Identifier: Apache-2.0
 *
 * Before a receive in a connection event, efr32_conn_stage() works out both PDUs isr_rx() can
 * answer it with, without changing the link layer's state: CUR, sent when the received frame does
 * not acknowledge our last PDU (or fails its CRC), and NEXT, sent when it does. The radio HAL
 * builds both images (encrypted when the link is) and hands them to the sequencer, which decides
 * at the frame's end by isr_rx()'s rules and turns around at T_IFS on its own
 * (hal/efr32/seq_abi.h). isr_rx() then runs as before and commits the same outcome:
 * - the received frame's acknowledgement and new-data decision come from the same header, CRC
 *   result and SN/NESN, so they agree; the one input that can change in between, whether an RX
 *   buffer is free, is the staged one (efr32_conn_rx_free());
 * - lll_conn_pdu_tx_prep() is replaced by efr32_conn_tx_prep(), which puts the staged choice in
 *   place exactly as lll_conn_pdu_tx_prep() would have (lll->empty, the head fragment's length,
 *   MD and LLID), because the TX queue may have grown since it was staged;
 * - the HAL checks that the SN and NESN isr_rx() then writes are those the sequencer sent.
 * Everything isr_rx() sends upwards (acknowledged TX nodes, received PDUs, MIC result) is unchanged.
 */
#include "hal/efr32/radio_conn.h"

#if defined(CONFIG_EFR32_RADIO_CONN_STAGED)

/* What lll_conn_pdu_tx_prep() would give in a given TX queue state. */
struct efr32_conn_cand {
	memq_link_t *link; /* NULL: the empty PDU */
	uint8_t offset;
	uint8_t head_len;
	uint8_t len;
	uint8_t md;
	uint8_t ll_id;
};

/* [0] CUR, [1] NEXT: staged for the receive in progress, committed by isr_rx(). */
static struct efr32_conn_cand efr32_conn_cand[2];
static bool efr32_conn_rx_free_staged;
static int efr32_conn_committing; /* -1: not staged; 0/1: the candidate sent */
static uint8_t efr32_conn_sn_before;
static uint8_t efr32_conn_nesn_before;

/* lll_conn_pdu_tx_prep() without side effects, for queue head @p link at @p offset. */
static void efr32_conn_cand_get(struct lll_conn *lll, bool empty, memq_link_t *link,
				uint8_t offset, uint8_t head_len, struct efr32_conn_cand *c)
{
	struct node_tx *tx;
	struct pdu_data *p;
	uint16_t max_tx_octets;

	c->offset = offset;
	c->head_len = head_len;
	if (empty || link == NULL) {
		c->link = NULL;
		c->len = 0U;
		c->ll_id = PDU_DATA_LLID_DATA_CONTINUE;
		/* empty_tx_enqueue(): MD while anything is queued. */
		c->md = (link != NULL) ? 1U : 0U;
		return;
	}
	tx = link->mem;
	p = (void *)(tx->pdu + offset);
	c->link = link;
	if (c->head_len == 0U) {
		c->head_len = p->len;
	}
	c->ll_id = (offset != 0U) ? PDU_DATA_LLID_DATA_CONTINUE : p->ll_id;
	c->len = c->head_len - offset;
	c->md = 0U;
	max_tx_octets = ull_conn_lll_max_tx_octets_get(lll);
	if (c->len > max_tx_octets) {
		c->len = max_tx_octets;
		c->md = 1U;
	}
	if (link->next != lll->memq_tx.tail) {
		c->md = 1U;
	}
}

static void efr32_conn_reply_fill(struct lll_conn *lll, const struct efr32_conn_cand *c,
				  uint64_t counter, struct efr32_radio_conn_reply *r)
{
	r->header = c->ll_id | (c->md << EFR32_SEQ_PDU_MD_BIT);
	r->len = c->len;
	r->payload = (c->link != NULL) ?
		((struct pdu_data *)(((struct node_tx *)c->link->mem)->pdu + c->offset))->lldata :
		NULL;
#if defined(CONFIG_BT_CTLR_LE_ENC)
	r->ccm = lll->enc_tx ? &lll->ccm_tx : NULL;
#else
	ARG_UNUSED(lll);
	r->ccm = NULL;
#endif
	r->counter = counter;
}

/*
 * Stage both replies to the next receive. The link layer state is the one isr_rx() will start
 * from: our last PDU is lll->empty or the head fragment at packet_tx_head_offset.
 */
static void efr32_conn_stage(struct lll_conn *lll)
{
	struct efr32_radio_conn_reply r[2];
	memq_link_t *link;
	bool empty = lll->empty != 0U;
	uint8_t offset = lll->packet_tx_head_offset;
	uint8_t head_len = lll->packet_tx_head_len;
	uint64_t counter = 0U;
	uint16_t rx_max;
	bool central = false;

#if defined(CONFIG_BT_CTLR_LE_ENC)
	counter = lll->ccm_tx.counter;
#endif
#if defined(CONFIG_BT_CENTRAL)
	central = lll->role == 0U;
#endif
	link = memq_peek(lll->memq_tx.head, lll->memq_tx.tail, NULL);
	/* No acknowledgement: the last PDU again (lll_conn_pdu_tx_prep() as it stands). */
	efr32_conn_cand_get(lll, empty, link, offset, head_len, &efr32_conn_cand[0]);
	efr32_conn_reply_fill(lll, &efr32_conn_cand[0], counter, &r[0]);
	/* Acknowledged: isr_rx_pdu()'s advance, then lll_conn_pdu_tx_prep(). */
	if (!empty && link != NULL) {
		struct pdu_data *last = (void *)(((struct node_tx *)link->mem)->pdu + offset);
		uint8_t next = offset + last->len;

#if defined(CONFIG_BT_CTLR_LE_ENC)
		if (last->len != 0U && lll->enc_tx) {
			counter++;
		}
#endif
		if (next < head_len) {
			offset = next;
		} else {
			link = (link->next != lll->memq_tx.tail) ? link->next : NULL;
			offset = 0U;
			head_len = 0U;
		}
	}
	efr32_conn_cand_get(lll, false, link, offset, head_len, &efr32_conn_cand[1]);
	efr32_conn_reply_fill(lll, &efr32_conn_cand[1], counter, &r[1]);
	efr32_conn_rx_free_staged = ull_pdu_rx_alloc_peek(3) != NULL;
	/* The receive's length limit, as lll_conn_rx_pkt_set() configures it. */
#if defined(CONFIG_BT_CTLR_DATA_LENGTH)
	rx_max = lll->dle.eff.max_rx_octets;
#else
	rx_max = PDU_DC_PAYLOAD_SIZE_MIN;
#endif
#if defined(CONFIG_BT_CTLR_LE_ENC)
	if (lll->enc_rx) {
		rx_max += PDU_MIC_SIZE;
	}
#endif
	/* isr_rx(): a second CRC error in a row closes the event. */
	efr32_radio_conn_stage(central, efr32_conn_rx_free_staged, crc_expire == 1U,
			       lll->sn, lll->nesn, (uint8_t)rx_max, &r[0], &r[1]);
}

/* From the Peripheral's and the Central's prepare, after the radio is set up. */
void efr32_conn_stage_prepare(struct lll_conn *lll)
{
	efr32_conn_committing = -1;
	efr32_conn_stage(lll);
}

/*
 * At isr_rx()'s start: 0 to run it (staged or not), -1 when the reply was staged but the
 * sequencer took no decision (it sent nothing): the event then ends.
 */
static int efr32_conn_commit_begin(struct lll_conn *lll)
{
	uint32_t outcome;
	int staged = efr32_radio_conn_outcome(&outcome);

	efr32_conn_committing = -1;
	if (staged == 0) {
		return 0;
	}
	if (staged < 0) {
		return -1;
	}
	efr32_conn_committing = (outcome & EFR32_SEQ_CONN_OUT_ACK) != 0U ? 1 : 0;
	efr32_conn_sn_before = lll->sn;
	efr32_conn_nesn_before = lll->nesn;
	return 0;
}

/* isr_rx_pdu(): whether an RX buffer is free, as staged when the reply was. */
static bool efr32_conn_rx_free(void)
{
	if (efr32_conn_committing >= 0) {
		return efr32_conn_rx_free_staged;
	}
	return ull_pdu_rx_alloc_peek(3) != NULL;
}

/* isr_rx(): lll_conn_pdu_tx_prep(), the staged choice when one was sent. */
static void efr32_conn_tx_prep(struct lll_conn *lll, struct pdu_data **pdu_data_tx)
{
	const struct efr32_conn_cand *c;
	struct pdu_data *p;

	if (efr32_conn_committing < 0) {
		lll_conn_pdu_tx_prep(lll, pdu_data_tx);
		return;
	}
	c = &efr32_conn_cand[efr32_conn_committing];
	/* isr_rx_pdu() advanced the queue as the sequencer assumed. */
	LL_ASSERT(((lll->sn ^ efr32_conn_sn_before) & 1U) == (uint8_t)efr32_conn_committing);
	if (c->link == NULL) {
		lll->empty = 1U;
		p = (void *)radio_pkt_empty_get();
		p->ll_id = PDU_DATA_LLID_DATA_CONTINUE;
		p->len = 0U;
		p->md = c->md;
	} else {
		LL_ASSERT(lll->empty == 0U);
		LL_ASSERT(lll->memq_tx.head == c->link);
		LL_ASSERT(lll->packet_tx_head_offset == c->offset);
		lll->packet_tx_head_len = c->head_len;
		p = (void *)(((struct node_tx *)c->link->mem)->pdu + c->offset);
		p->ll_id = c->ll_id;
		p->len = c->len;
		p->md = c->md;
	}
	p->rfu = 0U;
	*pdu_data_tx = p;
}

/*
 * isr_rx_pdu() failed (MIC failure): the link layer reports it and the connection ends. A staged
 * reply has already gone out, so the event ends here rather than waiting for its completion.
 */
static void efr32_conn_reply_abort(void *param)
{
	if (efr32_conn_committing >= 0) {
		efr32_conn_committing = -1;
		radio_isr_set(isr_done, param);
		radio_disable();
	}
}

/*
 * At isr_rx()'s end: when its reply went out and the event continues, stage the replies to the
 * next receive; the queue and SN/NESN are now those that receive will start from.
 */
static void efr32_conn_stage_next(struct lll_conn *lll)
{
	int committed = efr32_conn_committing;

	efr32_conn_committing = -1;
	if (committed >= 0 && efr32_radio_conn_continues()) {
		efr32_conn_stage(lll);
	}
}

#else /* !CONFIG_EFR32_RADIO_CONN_STAGED */

void efr32_conn_stage_prepare(struct lll_conn *lll)
{
	ARG_UNUSED(lll);
}

static int efr32_conn_commit_begin(struct lll_conn *lll)
{
	ARG_UNUSED(lll);
	return 0;
}

static bool efr32_conn_rx_free(void)
{
	return ull_pdu_rx_alloc_peek(3) != NULL;
}

static void efr32_conn_tx_prep(struct lll_conn *lll, struct pdu_data **pdu_data_tx)
{
	lll_conn_pdu_tx_prep(lll, pdu_data_tx);
}

static void efr32_conn_reply_abort(void *param)
{
	ARG_UNUSED(param);
}

static void efr32_conn_stage_next(struct lll_conn *lll)
{
	ARG_UNUSED(lll);
}

#endif /* CONFIG_EFR32_RADIO_CONN_STAGED */
