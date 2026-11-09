/*
 * Scanner and initiator replies staged before the frame ends (docs/radio-tables.md 7.87).
 * Included at the end of the derived lll_scan.c (CMakeLists.txt), so it reads lll_scan.c's state;
 * the patches there call into it. SPDX-License-Identifier: Apache-2.0
 *
 * Before each receive that may be answered (prepare_cb(), isr_common_done(), and after a SCAN_REQ
 * for the SCAN_RSP receive), efr32_scan_stage() builds what isr_rx_pdu() would send, as it builds
 * it: an active scanner's SCAN_REQ, whose RxAdd and AdvA come from the frame, or an initiator's
 * CONNECT_IND, which does not depend on the frame. The radio HAL hands it to the sequencer, which
 * decides at the frame's end by isr_rx_pdu()'s rules (hal/efr32/seq_abi.h EFR32_SEQ_CONN_SCAN)
 * and turns around at T_IFS on its own. isr_rx() then runs as before and commits the same
 * answer; the HAL checks the PDU it commits against the one sent. What moves where:
 * - on the sequencer: the CRC result, the PDU type and length (AdvA present), the advertiser's
 *   address and its type (initiator), the directed TargetA (ours, or any resolvable private one
 *   with the extended filter policy), and the initiator's check that the CONNECT_IND fits the
 *   scan slot, counted on TIMER0 from the event timer's start;
 * - staged from here, fixed for the receive: whether an RX buffer is free (only the LLL takes
 *   them, so one free when staged is free when committed), the scanner's and initiator's own
 *   addresses, the filter policy, and the resolving-list facts below. The HCI forbids changing
 *   the filter accept list, the resolving list or address resolution while scanning or
 *   initiating uses them (Core v6.1 Vol 4 Part E 7.8.16, 7.8.38, 7.8.44);
 * - isr_rx_pdu() follows the sequencer's decision where its own inputs could differ from the
 *   staged ones (efr32_scan_declined()): an RX buffer freed since staging, and the slot check,
 *   whose frame-end estimates differ by the two cores' timer reads.
 * - Left to the M33: address resolution. With it enabled (ull_filter_lll_rl_enabled()) a frame
 *   is matched by AES over every peer IRK, which the sequencer cannot do within T_IFS; the
 *   receive is then not staged and the M33 decides at the frame's end as before (infeasible on
 *   silicon by analysis, 7.82). This HAL has no filter accept list matcher
 *   (radio_filter_has_match() is 0), so with the accept list in the filter policy no frame
 *   passes isr_rx_scan_check(): nothing is armed for a response.
 * The initiator's WinOffset is EFR32_DL_CONNECT_IND_WIN_OFFSET (hal/efr32/ll_deadlines.h): the
 * transmit window counts from the CONNECT_IND's own end (Vol 6 Part B 4.5.3), so it does not
 * depend on the advertisement's timing; isr_rx_pdu() puts its first anchor there.
 */

#if defined(CONFIG_EFR32_RADIO_CONN_STAGED)
/* lll->conn_win_offset_us would make the WinOffset depend on the frame's end. */
BUILD_ASSERT(!IS_ENABLED(CONFIG_BT_CTLR_SCHED_ADVANCED),
	     "staged CONNECT_IND: BT_CTLR_SCHED_ADVANCED is not supported");

static struct pdu_adv efr32_scan_reply;
/* The sequencer sent the SCAN_REQ isr_rx_pdu() is committing. */
static bool efr32_scan_replied;

/*
 * Stage the answer to the receive being set up: 1 staged, 0 left to the M33, -1 nothing can be
 * answered.
 */
static int efr32_scan_stage(struct lll_scan *lll)
{
	struct pdu_adv *pdu = &efr32_scan_reply;
	struct efr32_radio_scan_stage s = {
		.reply = (const uint8_t *)&efr32_scan_reply,
	};
	bool connect = false;

#if defined(CONFIG_BT_CENTRAL)
	connect = (lll->conn != NULL);
#endif
#if defined(CONFIG_BT_CTLR_PRIVACY)
	if (ull_filter_lll_rl_enabled()) {
		return 0;
	}
#endif
	if ((lll->filter_policy & SCAN_FP_FILTER) != 0U) {
		return -1;
	}
	/* isr_rx_pdu() writes its PDU's fields over the scratch PDU's header. */
	memcpy(pdu, radio_pkt_scratch_get(), PDU_AC_LL_HEADER_SIZE);
	if (!connect) {
		pdu->type = PDU_ADV_TYPE_SCAN_REQ;
		pdu->rx_addr = 0U;
		pdu->len = sizeof(struct pdu_adv_scan_req);
		pdu->tx_addr = lll->init_addr_type;
		memcpy(&pdu->scan_req.scan_addr[0], &lll->init_addr[0], BDADDR_SIZE);
		(void)memset(&pdu->scan_req.adv_addr[0], 0, BDADDR_SIZE);
		s.rx_free = (ull_pdu_rx_alloc_peek(3) != NULL);
		efr32_radio_scan_stage(&s);
		return 1;
	}
#if defined(CONFIG_BT_CENTRAL)
	struct lll_conn *lll_conn = lll->conn;
	struct ull_hdr *ull = HDR_LLL2ULL(lll);
	uint32_t latest_us;
	uint32_t period_us;
#if defined(CONFIG_BT_CTLR_PRIVACY)
	uint8_t rl_idx = FILTER_IDX_NONE;

	/* isr_scan_init_adva_check(): an advertiser the resolving list requires to resolve. */
	if (!ull_filter_lll_rl_addr_allowed(lll->adv_addr_type, lll->adv_addr, &rl_idx)) {
		return -1;
	}
#endif
	pdu->type = PDU_ADV_TYPE_CONNECT_IND;
	pdu->chan_sel = IS_ENABLED(CONFIG_BT_CTLR_CHAN_SEL_2) ? 1U : 0U;
	pdu->rx_addr = lll->adv_addr_type;
	pdu->len = sizeof(struct pdu_adv_connect_ind);
	pdu->tx_addr = lll->init_addr_type;
	memcpy(&pdu->connect_ind.init_addr[0], &lll->init_addr[0], BDADDR_SIZE);
	memcpy(&pdu->connect_ind.adv_addr[0], &lll->adv_addr[0], BDADDR_SIZE);
	memcpy(&pdu->connect_ind.access_addr[0], &lll_conn->access_addr[0], 4);
	memcpy(&pdu->connect_ind.crc_init[0], &lll_conn->crc_init[0], 3);
	pdu->connect_ind.win_size = 1U;
	pdu->connect_ind.win_offset = sys_cpu_to_le16(EFR32_DL_CONNECT_IND_WIN_OFFSET);
	pdu->connect_ind.interval = sys_cpu_to_le16(lll_conn->interval);
	pdu->connect_ind.latency = sys_cpu_to_le16(lll_conn->latency);
	pdu->connect_ind.timeout = sys_cpu_to_le16(lll->conn_timeout);
	memcpy(&pdu->connect_ind.chan_map[0], &lll_conn->data_chan_map[0],
	       sizeof(pdu->connect_ind.chan_map));
	pdu->connect_ind.hop = lll_conn->data_chan_hop;
	pdu->connect_ind.sca = lll_clock_sca_local_get();
	/* transmitWindowOffset is at most connInterval (Vol 6 Part B 2.3.3.1). */
	LL_ASSERT(EFR32_DL_CONNECT_IND_WIN_OFFSET * BT_TRANSMIT_WINDOW_UNIT_US <=
		  (uint32_t)lll_conn->interval * CONN_INT_UNIT_US);

	s.connect = true;
	s.rx_free = (ull_pdu_rx_alloc_peek(IS_ENABLED(CONFIG_BT_CTLR_CHAN_SEL_2) ? 4 : 3) != NULL);
	s.adv_addr = lll->adv_addr;
	s.adv_addr_type = lll->adv_addr_type;
	s.init_addr = lll->init_addr;
	s.init_addr_type = lll->init_addr_type;
	s.tgta_rpa = (lll->filter_policy & SCAN_FP_EXT) != 0U;
	/* isr_rx_pdu()'s slot check: no CONNECT_IND for a frame ending after latest_us. */
	latest_us = HAL_TICKER_TICKS_TO_US(ull->ticks_slot) - EVENT_IFS_US -
		    BT_LE_1M_CONNECT_IND_US - EVENT_OVERHEAD_START_US - EVENT_TICKER_RES_MARGIN_US;
	period_us = (lll->ticks_window != 0U) ? 0U : (uint32_t)lll->interval * SCAN_INT_UNIT_US;
	s.timed = latest_us < ((period_us != 0U) ? period_us - 1U : UINT32_MAX);
	s.latest_us = latest_us + 1U;
	s.period_us = period_us;
	efr32_radio_scan_stage(&s);
#endif /* CONFIG_BT_CENTRAL */
	return 1;
}

/*
 * The sequencer's decision on a staged receive, or the link layer's own (@p declines) when it
 * was not staged: true when isr_rx_pdu() must answer nothing.
 */
static bool efr32_scan_declined(bool declines)
{
	uint32_t outcome;
	int staged = efr32_radio_conn_outcome(&outcome);

	if (staged == 0) {
		return declines;
	}
	efr32_scan_replied = staged > 0 && (outcome & EFR32_SEQ_CONN_OUT_REPLY) != 0U;
	return !efr32_scan_replied;
}

/* After the SCAN_REQ the sequencer sent: the SCAN_RSP receive it armed. */
static void efr32_scan_stage_next(struct lll_scan *lll)
{
	if (efr32_scan_replied) {
		efr32_scan_replied = false;
		(void)efr32_scan_stage(lll);
	}
}
#else
static int efr32_scan_stage(struct lll_scan *lll)
{
	ARG_UNUSED(lll);
	return 0;
}

static bool efr32_scan_declined(bool declines)
{
	return declines;
}

static void efr32_scan_stage_next(struct lll_scan *lll)
{
	ARG_UNUSED(lll);
}
#endif /* CONFIG_EFR32_RADIO_CONN_STAGED */

#if defined(CONFIG_BT_CENTRAL)
/* isr_rx_pdu()'s CONNECT_IND slot check, then the sequencer's decision on a staged receive. */
static bool efr32_init_declined(uint32_t pdu_end_us, uint32_t latest_us)
{
	bool declines = efr32_scan_declined(pdu_end_us > latest_us);

#if defined(CONFIG_EFR32_RADIO_TIMING_PROBE)
	efr32_radio_probe_slot(!declines, pdu_end_us > latest_us,
			       (int32_t)(latest_us - pdu_end_us));
#endif
	return declines;
}
#endif

/*
 * OpenISA's scanner arms an RX->TX turnaround for every receive window, a passive scanner's
 * included. On the EFR32 an armed window holds the RAC in RX2TX at each frame's end and gives
 * the frame's completion the T_IFS response deadline (docs/radio-tables.md 7.77), which a
 * passive scanner, which never responds, has no reason to meet: on silicon admission found it
 * unsat at the first received advertisement (7.80). Arm the turnaround only where a response
 * can follow: an active scanner's SCAN_REQ or an initiator's CONNECT_IND, and stage it.
 */
static void efr32_scan_switch(struct lll_scan *lll)
{
	bool respond = (lll->type != 0U);

#if defined(CONFIG_BT_CENTRAL)
	respond = respond || (lll->conn != NULL);
#endif /* CONFIG_BT_CENTRAL */
	if (respond && efr32_scan_stage(lll) < 0) {
		respond = false;
	}
	if (respond) {
		radio_switch_complete_and_tx(PHY_LEGACY, PHY_FLAGS_UNUSED, PHY_LEGACY,
					     PHY_FLAGS_UNUSED);
	} else {
		radio_switch_complete_and_disable();
	}
}
