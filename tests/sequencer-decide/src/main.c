/*
 * The open radio sequencer's staged-reply decision (efr32_seq_conn_decide(), hal/efr32/seq_abi.h,
 * docs/radio-tables.md 7.82, 7.97) on crafted stages: the installed image's RDMAILBOX1 vector is
 * pointed at the harness entry (tramp.c), which runs the image's own decision once per mailbox
 * flag. Each case writes a stage and a received frame, runs it, prints what the decision left
 * (return, OUTCOME, the sequencer's shared words and its SysTick cycles) and checks whether it
 * answered. run-qemu.sh runs the C and the assembly image and compares every case, the M0+'s
 * register writes (the reply into the BUFC TX FIFO among them) included.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <soc.h>
#include "hal/efr32/seq_abi.h"
#include "harness.h"
#include "seq-syms.h"

extern const uint8_t efr32_radio_seq_image[];
extern const size_t efr32_radio_seq_image_size;
extern const uint8_t tramp_image[];
extern const size_t tramp_image_size;

/* Register map and install sequence of hal/radio.c, as tests/sequencer-timed-wrap uses them. */
#define RAC_BASE 0xB8020000UL
#define RAC_CTRL_OFFSET 0x014U
#define RAC_SEQCTRL_OFFSET 0x038U
#define RAC_RXENSRCEN_OFFSET 0x008U
#define RAC_RXENSRCEN_SWRXEN BIT(1)
#define RAC_CTRL_FORCEDISABLE BIT(0)
#define RAC_CTRL_ACTIVE_HIGH BIT(7)
#define RAC_CTRL_PA_ENABLE_HIGH BIT(8)
#define RAC_CTRL_LNA_ENABLE_HIGH BIT(9)
#define RAC_CTRL_SEQRESET BIT(24)
#define RAC_CTRL_CPUWAITDIS BIT(26)
#define RAC_SEQCTRL_STAGE2_STARTUP 0x00000011U
#define REGISTER_SET_OFFSET 0x1000U
#define REGISTER_CLR_OFFSET 0x2000U
#define SEQ_RAM_BASE 0xB0000000UL
#define SEQ_IDENTITY_ADDRESS 0xB0004FE0UL
#define SEQ_IDENTITY_OPEN_V1 0x4F535131U
#define SEQ_RX2TX_DECISION_ADDRESS 0xB0004FA4UL
#define SEQ_M33_RADIO_ALIAS 0x10000000UL
#define SEQ_MAILBOX_FLAGS_SET \
	(EFR32_SEQ_RDMAILBOX1_BASE + SEQ_M33_RADIO_ALIAS + REGISTER_SET_OFFSET + \
	 EFR32_SEQ_RDMAILBOX_FLAGS)
#define SEQ_MAILBOX_IEN (EFR32_SEQ_RDMAILBOX1_BASE + SEQ_M33_RADIO_ALIAS + EFR32_SEQ_RDMAILBOX_IEN)
#define SEQ_READY_TIMEOUT_US 10000U
/* The sequencer's RDMAILBOX1 vector: exception 16 + IRQ. */
#define SEQ_MAILBOX_VECTOR (SEQ_RAM_BASE + 4U * (16U + EFR32_SEQ_RDMAILBOX_IRQ))
/* The shared words the decision may write (traces, RX2TX_DECISION, identity). */
#define SEQ_SHARED_FROM 0x4f00U
#define SEQ_SHARED_TO 0x5000U
#define POLL_US 10U
#define CASE_TIMEOUT_US 10000U

static volatile uint32_t *reg(uintptr_t address)
{
	return (volatile uint32_t *)address;
}

static volatile uint32_t *seq32(uint32_t seq_address)
{
	return reg(SEQ_RAM_BASE + seq_address);
}

static volatile uint8_t *seq8(uint32_t seq_address)
{
	return (volatile uint8_t *)(SEQ_RAM_BASE + seq_address);
}

static volatile uint32_t *conn(uint32_t offset)
{
	return seq32(EFR32_SEQ_CONN_BLOCK + offset);
}

static volatile uint32_t *harness(uint32_t offset)
{
	return seq32(HARNESS_BLOCK + offset);
}

/* Addresses: the advertiser's (static random), the initiator's own (public), another device's. */
static const uint8_t adva[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0xc6};
static const uint8_t inita[6] = {0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0x06};
static const uint8_t other[6] = {0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xc7};
/* A resolvable private address (octet 5 bits 7:6 = 01) and a static one, neither ours. */
static const uint8_t rpa[6] = {0x31, 0x32, 0x33, 0x34, 0x35, 0x46};
static const uint8_t scana[6] = {0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6};

enum role { CONN, ADV, SCAN, INIT };

/* A received frame: header, length, payload (the trailer follows from crc_ok). */
struct frame {
	uint8_t octets[64];
	uint32_t n; /* header, length and payload octets written */
	bool crc_ok;
};

struct stage {
	enum role role;
	uint32_t flags;
	uint8_t rx_max;
	uint8_t rx_offset;
	/* Slot check (EFR32_SEQ_CONN_INIT_TIMED): TIMER0's count and the words. */
	uint32_t t0;
	uint32_t start, latest, period, q8, eof;
	/* 0: staged before the mailbox; else STAGED is written this long after it, or never. */
	uint32_t stage_after_us;
	bool never_staged;
};

static void frame_init(struct frame *f, uint8_t header, uint8_t len)
{
	memset(f, 0, sizeof(*f));
	f->octets[0] = header;
	f->octets[1] = len;
	f->n = 2U + len;
	f->crc_ok = true;
}

static void frame_put(struct frame *f, uint32_t at, const uint8_t *p, uint32_t n)
{
	memcpy(&f->octets[2U + at], p, n);
}

/* An advertising-channel PDU of @p type with AdvA first, then @p tail. */
static void adv_pdu(struct frame *f, uint8_t type, bool tx_addr, bool rx_addr, uint8_t len,
		    const uint8_t *a, const uint8_t *tail, uint32_t tail_n)
{
	frame_init(f, type | (tx_addr ? BIT(EFR32_SEQ_ADV_TXADD_BIT) : 0U) |
			      (rx_addr ? BIT(EFR32_SEQ_ADV_RXADD_BIT) : 0U),
		   len);
	frame_put(f, 0U, a, 6U);
	if (tail != NULL) {
		frame_put(f, 6U, tail, tail_n);
	}
}

static const uint8_t adv_data[3] = {0x02, 0x01, 0x06};

static void adv_ind(struct frame *f, const uint8_t *a)
{
	adv_pdu(f, EFR32_SEQ_ADV_IND_TYPE, true, false, 9U, a, adv_data, sizeof(adv_data));
}

static void direct_ind(struct frame *f, bool tx_addr, bool rx_addr, const uint8_t *a,
		       const uint8_t *target)
{
	adv_pdu(f, EFR32_SEQ_ADV_DIRECT_IND_TYPE, tx_addr, rx_addr, EFR32_SEQ_ADV_DIRECT_IND_LEN, a,
		target, 6U);
}

/* A data-channel PDU: LLID 1 or 2, NESN, SN, MD, @p len payload octets. */
static void data_pdu(struct frame *f, uint8_t llid, bool nesn, bool sn, bool md, uint8_t len)
{
	frame_init(f, llid | (nesn ? BIT(EFR32_SEQ_PDU_NESN_BIT) : 0U) |
			      (sn ? BIT(EFR32_SEQ_PDU_SN_BIT) : 0U) |
			      (md ? BIT(EFR32_SEQ_PDU_MD_BIT) : 0U),
		   len);
	for (uint32_t i = 0U; i < len && i < sizeof(f->octets) - 2U; i++) {
		f->octets[2U + i] = (uint8_t)(0x40U + i);
	}
}

/* The staged PDUs each role sends: an empty PDU then data, a SCAN_RSP, a SCAN_REQ, a CONNECT_IND. */
static void write_pdus(enum role role, uint32_t flags)
{
	volatile uint8_t *cur = seq8(HARNESS_CUR);
	volatile uint8_t *next = seq8(HARNESS_NEXT);
	uint32_t cur_n;
	uint32_t next_n;

	for (uint32_t i = 0U; i < 0x80U; i++) {
		cur[i] = 0U;
		next[i] = 0U;
	}
	switch (role) {
	case CONN:
		cur[0] = 0x01; /* LLID 1, empty */
		cur[1] = 0x00;
		cur_n = 2U;
		next[0] = 0x02; /* LLID 2, 3 octets */
		next[1] = 0x03;
		next[2] = 0xaa;
		next[3] = 0xbb;
		next[4] = 0xcc;
		next_n = 5U;
		break;
	case ADV:
		cur[0] = 0x04 | ((flags & EFR32_SEQ_CONN_ADV_TXADD) ? BIT(EFR32_SEQ_ADV_TXADD_BIT) : 0U);
		cur[1] = 9U;
		for (uint32_t i = 0U; i < 6U; i++) {
			cur[2U + i] = adva[i];
		}
		cur[8] = 0x02;
		cur[9] = 0x0a;
		cur[10] = 0x00;
		cur_n = 11U;
		next_n = 0U;
		break;
	case SCAN:
		cur[0] = EFR32_SEQ_ADV_SCAN_REQ_TYPE | BIT(EFR32_SEQ_ADV_TXADD_BIT);
		cur[1] = EFR32_SEQ_ADV_SCAN_REQ_LEN;
		for (uint32_t i = 0U; i < 6U; i++) {
			cur[2U + i] = scana[i];
			cur[8U + i] = 0xee; /* AdvA: replaced by the frame's */
		}
		cur_n = 14U;
		next_n = 0U;
		break;
	case INIT:
	default:
		cur[0] = 0x05 | ((flags & EFR32_SEQ_CONN_INIT_RXADD) ? BIT(EFR32_SEQ_ADV_TXADD_BIT) : 0U) |
			 ((flags & EFR32_SEQ_CONN_ADV_TXADD) ? BIT(EFR32_SEQ_ADV_RXADD_BIT) : 0U);
		cur[1] = 34U;
		for (uint32_t i = 0U; i < 6U; i++) {
			cur[2U + i] = inita[i];
			cur[8U + i] = adva[i];
		}
		for (uint32_t i = 14U; i < 36U; i++) {
			cur[i] = (uint8_t)i;
		}
		cur_n = 36U;
		next_n = 0U;
		break;
	}
	*conn(EFR32_SEQ_CONN_CUR) = HARNESS_CUR;
	*conn(EFR32_SEQ_CONN_CUR_OCTETS) = cur_n;
	*conn(EFR32_SEQ_CONN_NEXT) = next_n ? HARNESS_NEXT : HARNESS_CUR;
	*conn(EFR32_SEQ_CONN_NEXT_OCTETS) = next_n ? next_n : cur_n;
}

static uint32_t case_index;
static uint32_t shared_before[(SEQ_SHARED_TO - SEQ_SHARED_FROM) / 4U];

/* Writes the stage and the frame, runs the decision once and prints what it left. */
static uint32_t run(const char *name, const struct stage *s, const struct frame *f)
{
	uint32_t generation = ++case_index;
	volatile uint8_t *fifo = seq8(HARNESS_RX_FIFO);

	/* Marker for run-qemu.sh: the case's register writes follow this one. */
	TIMER0->CC[0].OC = generation;
	*conn(EFR32_SEQ_CONN_STATE) = 0U;
	*reg(SEQ_RX2TX_DECISION_ADDRESS) = 0U;
	*reg(RAC_BASE + RAC_RXENSRCEN_OFFSET + REGISTER_CLR_OFFSET) = RAC_RXENSRCEN_SWRXEN;
	for (uint32_t i = 0U; i <= HARNESS_RX_MASK; i++) {
		fifo[i] = 0x5a;
	}
	/* Payload beyond the buffer is the FIFO's fill. */
	for (uint32_t i = 0U; i < MIN(f->n, sizeof(f->octets)); i++) {
		fifo[(s->rx_offset + i) & HARNESS_RX_MASK] = f->octets[i];
	}
	/* FRC's trailer octet after the payload: CRCOK in bit 7. */
	fifo[(s->rx_offset + 2U + f->octets[1]) & HARNESS_RX_MASK] =
		f->crc_ok ? EFR32_SEQ_FRC_TRAILER_CRCOK : 0x00;
	write_pdus(s->role, s->flags);
	*conn(EFR32_SEQ_CONN_FLAGS) = s->flags;
	*conn(EFR32_SEQ_CONN_RX_BASE) = HARNESS_RX_FIFO;
	*conn(EFR32_SEQ_CONN_RX_OFFSET) = s->rx_offset;
	*conn(EFR32_SEQ_CONN_RX_MASK) = HARNESS_RX_MASK;
	*conn(EFR32_SEQ_CONN_RX_MAX) = s->rx_max;
	*conn(EFR32_SEQ_CONN_OUTCOME) = 0xdeadbeefU;
	*conn(EFR32_SEQ_CONN_TAKEN_AT) = 0U;
	for (uint32_t i = 0U; i < 6U; i++) {
		seq8(EFR32_SEQ_CONN_BLOCK + EFR32_SEQ_CONN_ADVA)[i] = adva[i];
		seq8(EFR32_SEQ_CONN_BLOCK + EFR32_SEQ_CONN_INITA)[i] = inita[i];
	}
	*conn(EFR32_SEQ_SCAN_START) = s->start;
	*conn(EFR32_SEQ_SCAN_LATEST) = s->latest;
	*conn(EFR32_SEQ_SCAN_PERIOD) = s->period;
	*conn(EFR32_SEQ_SCAN_T0_PER_STIMER_Q8) = s->q8;
	*conn(EFR32_SEQ_SCAN_EOF_TO_EXIT) = s->eof;
	TIMER0->CMD_SET = TIMER_CMD_STOP;
	TIMER0->CNT = s->t0;
	for (uint32_t i = 0U; i < ARRAY_SIZE(shared_before); i++) {
		shared_before[i] = *seq32(SEQ_SHARED_FROM + 4U * i);
	}

	uint32_t staged = EFR32_SEQ_CONN_STAGED | (generation << EFR32_SEQ_CONN_GENERATION_SHIFT);
	uint32_t done = *harness(HARNESS_DONE);

	if (s->stage_after_us == 0U && !s->never_staged) {
		*conn(EFR32_SEQ_CONN_STATE) = staged;
	}
	__DSB();
	*reg(SEQ_MAILBOX_FLAGS_SET) = EFR32_SEQ_RDMAILBOX_TIMED_FLAG;
	if (s->stage_after_us != 0U) {
		k_busy_wait(s->stage_after_us);
		*conn(EFR32_SEQ_CONN_STATE) = staged;
	}
	for (uint32_t us = 0U; us < CASE_TIMEOUT_US && *harness(HARNESS_DONE) == done;
	     us += POLL_US) {
		k_busy_wait(POLL_US);
	}
	zassert_not_equal(*harness(HARNESS_DONE), done, "%s: the decision did not return", name);

	uint32_t ret = *harness(HARNESS_RET);
	uint32_t before = *harness(HARNESS_BEFORE);
	uint32_t after = *harness(HARNESS_AFTER);
	uint32_t cycles = before >= after ? before - after
					  : before + *harness(HARNESS_RELOAD) + 1U - after;

	printk("CASE %u %s ret=%u outcome=%08x state=%08x cycles=%u taken_at=%04x", generation,
	       name, ret, *conn(EFR32_SEQ_CONN_OUTCOME), *conn(EFR32_SEQ_CONN_STATE), cycles,
	       *conn(EFR32_SEQ_CONN_TAKEN_AT) & 0xffffU);
	for (uint32_t i = 0U; i < ARRAY_SIZE(shared_before); i++) {
		uint32_t now = *seq32(SEQ_SHARED_FROM + 4U * i);

		if (now != shared_before[i]) {
			printk(" [%04x]=%08x", SEQ_SHARED_FROM + 4U * i, now);
		}
	}
	printk("\n");
	return ret;
}

static void expect(const char *name, const struct stage *s, const struct frame *f, uint32_t answer)
{
	uint32_t ret = run(name, s, f);

	zassert_equal(ret, answer, "%s: answered %u, expected %u", name, ret, answer);
}

static const struct stage conn_stage = {
	.role = CONN, .flags = EFR32_SEQ_CONN_RX_FREE, .rx_max = 27U, .rx_offset = 0x10U,
};

/* Baselines: paths the e2e suite reaches (§7.97), for the cycle comparison with it. */
ZTEST(sequencer_decide, test_0_baselines)
{
	struct frame f;
	struct stage s = conn_stage;

	data_pdu(&f, 1U, true, false, false, 0U);
	expect("conn_periph_ack_accept_reply", &s, &f, 1U);
	s.flags |= EFR32_SEQ_CONN_CENTRAL;
	data_pdu(&f, 1U, false, true, false, 0U);
	expect("conn_central_done_silent", &s, &f, 0U);

	s = (struct stage){.role = SCAN, .flags = EFR32_SEQ_CONN_SCAN | EFR32_SEQ_CONN_RX_FREE,
			   .rx_max = 37U, .rx_offset = 0x10U};
	adv_ind(&f, other);
	expect("scan_adv_not_answered", &s, &f, 0U);
	s.flags |= EFR32_SEQ_CONN_SCAN_REQ;
	expect("scan_req_sent", &s, &f, 1U);

	s = (struct stage){.role = ADV, .flags = EFR32_SEQ_CONN_ADV | EFR32_SEQ_CONN_ADV_TXADD,
			   .rx_max = 37U, .rx_offset = 0x10U};
	adv_pdu(&f, EFR32_SEQ_ADV_SCAN_REQ_TYPE, false, true, EFR32_SEQ_ADV_SCAN_REQ_LEN, scana,
		adva, 6U);
	expect("adv_scan_req_matched", &s, &f, 1U);
}

#define INIT_FLAGS (EFR32_SEQ_CONN_SCAN | EFR32_SEQ_CONN_INIT | EFR32_SEQ_CONN_RX_FREE | \
		    EFR32_SEQ_CONN_ADV_TXADD)

/* 1: the initiator's ADV_DIRECT_IND, TargetA ours or (extended filter policy) any RPA. */
ZTEST(sequencer_decide, test_1_direct_ind)
{
	struct frame f;
	struct stage s = {.role = INIT, .flags = INIT_FLAGS, .rx_max = 37U, .rx_offset = 0x10U};
	uint8_t near[6];

	direct_ind(&f, true, false, adva, inita);
	expect("direct_to_us", &s, &f, 1U);
	memcpy(near, inita, 6U);
	near[0] ^= 1U;
	direct_ind(&f, true, false, adva, near);
	expect("direct_other_target_first_octet", &s, &f, 0U);
	memcpy(near, inita, 6U);
	near[5] ^= 0x80U;
	direct_ind(&f, true, false, adva, near);
	expect("direct_other_target_last_octet", &s, &f, 0U);
	direct_ind(&f, true, true, adva, inita);
	expect("direct_rxadd_mismatch", &s, &f, 0U);
	direct_ind(&f, false, false, adva, inita);
	expect("direct_txadd_mismatch", &s, &f, 0U);
	direct_ind(&f, true, false, other, inita);
	expect("direct_adva_mismatch", &s, &f, 0U);
	direct_ind(&f, true, false, adva, inita);
	f.octets[1] = EFR32_SEQ_ADV_DIRECT_IND_LEN + 1U;
	f.n++;
	expect("direct_len_13", &s, &f, 0U);
	direct_ind(&f, true, false, adva, inita);
	f.crc_ok = false;
	expect("direct_crc_fail", &s, &f, 0U);
	s.rx_offset = 0xfcU;
	direct_ind(&f, true, false, adva, inita);
	expect("direct_to_us_fifo_wraps", &s, &f, 1U);
	s.rx_offset = 0x10U;

	s.flags = INIT_FLAGS | EFR32_SEQ_CONN_INIT_RXADD;
	direct_ind(&f, true, true, adva, inita);
	expect("direct_to_us_random_inita", &s, &f, 1U);
	direct_ind(&f, true, false, adva, inita);
	expect("direct_random_inita_rxadd_0", &s, &f, 0U);

	s.flags = INIT_FLAGS | EFR32_SEQ_CONN_INIT_TGTA_RPA;
	direct_ind(&f, true, true, adva, rpa);
	expect("rpa_policy_rpa_target", &s, &f, 1U);
	direct_ind(&f, true, true, adva, other);
	expect("rpa_policy_static_target", &s, &f, 0U);
	direct_ind(&f, true, false, adva, rpa);
	expect("rpa_policy_rpa_bits_rxadd_0", &s, &f, 0U);
	direct_ind(&f, true, false, adva, inita);
	expect("rpa_policy_ours_rxadd_0", &s, &f, 1U);
	s.rx_offset = 0xf8U;
	direct_ind(&f, true, true, adva, rpa);
	expect("rpa_policy_rpa_target_fifo_wraps", &s, &f, 1U);
	s.rx_offset = 0x10U;
	s.flags = INIT_FLAGS | EFR32_SEQ_CONN_INIT_TGTA_RPA | EFR32_SEQ_CONN_INIT_RXADD;
	direct_ind(&f, true, true, adva, rpa);
	expect("rpa_policy_rpa_target_random_inita", &s, &f, 1U);
	direct_ind(&f, true, true, adva, inita);
	expect("rpa_policy_ours_random_inita", &s, &f, 1U);

	s.flags = INIT_FLAGS;
	adv_ind(&f, adva);
	expect("init_adv_ind", &s, &f, 1U);
	adv_pdu(&f, EFR32_SEQ_ADV_SCAN_IND_TYPE, true, false, 9U, adva, adv_data, 3U);
	expect("init_adv_scan_ind", &s, &f, 0U);
}

/* Answered when (t0 - stimer*q8>>8 - eof - start) mod period < latest, period 0 meaning none. */
static void slot(const char *name, uint32_t t0, uint32_t start, uint32_t eof, uint32_t latest,
		 uint32_t period, uint32_t answer)
{
	struct frame f;
	struct stage s = {.role = INIT, .flags = INIT_FLAGS | EFR32_SEQ_CONN_INIT_TIMED,
			  .rx_max = 37U, .rx_offset = 0x10U, .t0 = t0, .start = start,
			  .latest = latest, .period = period, .eof = eof};

	adv_ind(&f, adva);
	expect(name, &s, &f, answer);
}

/* The remainder pinned: LATEST = r refuses, r + 1 answers. */
static void slot_mod(const char *name, uint32_t n, uint32_t d)
{
	char a[48];
	char b[48];
	uint32_t r = n % d;

	snprintk(a, sizeof(a), "%s_latest_r", name);
	snprintk(b, sizeof(b), "%s_latest_r+1", name);
	slot(a, n, 0U, 0U, r, d, 0U);
	slot(b, n, 0U, 0U, r + 1U, d, 1U);
}

/* 2: the initiator's slot check (EFR32_SEQ_CONN_INIT_TIMED) and its modulo. */
ZTEST(sequencer_decide, test_2_slot_check)
{
	slot("slot_no_period_in", 999U, 0U, 0U, 1000U, 0U, 1U);
	slot("slot_no_period_edge", 1000U, 0U, 0U, 1000U, 0U, 0U);
	slot("slot_start_eof_in", 1599U, 500U, 100U, 1000U, 0U, 1U);
	slot("slot_start_eof_edge", 1600U, 500U, 100U, 1000U, 0U, 0U);
	slot("slot_before_start_no_period", 0x80U, 0x100U, 0U, 1000U, 0U, 0U);
	slot("slot_latest_0", 0U, 0U, 0U, 0U, 1000U, 0U);
	slot_mod("mod_12345_1000", 12345U, 1000U);
	slot_mod("mod_999_1000", 999U, 1000U);
	slot_mod("mod_1000_1000", 1000U, 1000U);
	slot_mod("mod_5_7", 5U, 7U);
	slot_mod("mod_ffffffff_1", 0xffffffffU, 1U);
	slot_mod("mod_ffffffff_80000001", 0xffffffffU, 0x80000001U);
	slot_mod("mod_80000000_80000000", 0x80000000U, 0x80000000U);
	slot_mod("mod_7fffffff_80000000", 0x7fffffffU, 0x80000000U);
	slot_mod("mod_fffffffe_3", 0xfffffffeU, 3U);
	slot_mod("mod_12345678_ffff", 0x12345678U, 0xffffU);
	/* A 100 ms scan interval at 38.4 MHz, seven intervals in. */
	slot_mod("mod_interval_7", 7U * 3840000U + 123U, 3840000U);
	/* Before START, wrapping: (0x80 - 0x100) mod 1000. */
	slot("slot_before_start_mod_r", 0x80U, 0x100U, 0U, (0x80U - 0x100U) % 1000U, 1000U, 0U);
	slot("slot_before_start_mod_r+1", 0x80U, 0x100U, 0U, (0x80U - 0x100U) % 1000U + 1U, 1000U,
	     1U);

	/* RAC.STIMER's term: with q8 2048 (TIMER0 at 8 per STIMER tick) it is at most 0xffff * 8,
	 * whatever STIMER reads here; answers that hold for any of them. */
	struct frame f;
	struct stage s = {.role = INIT, .flags = INIT_FLAGS | EFR32_SEQ_CONN_INIT_TIMED,
			  .rx_max = 37U, .rx_offset = 0x10U, .q8 = 2048U, .eof = 100U,
			  .latest = 2000000U, .period = 0x7fffffffU};

	adv_ind(&f, adva);
	s.t0 = 1000000U;
	expect("slot_stimer_term_in", &s, &f, 1U);
	s.t0 = 3000000U;
	expect("slot_stimer_term_late", &s, &f, 0U);
}

/* 3: a frame longer than RX_MAX counts as a CRC failure. */
ZTEST(sequencer_decide, test_3_longer_than_rx_max)
{
	struct frame f;
	struct stage s = conn_stage;

	data_pdu(&f, 2U, true, false, false, 27U);
	expect("conn_len_eq_rx_max", &s, &f, 1U);
	data_pdu(&f, 2U, true, false, false, 28U);
	expect("conn_len_gt_rx_max", &s, &f, 1U);
	s.flags |= EFR32_SEQ_CONN_CRC_CLOSES;
	expect("conn_len_gt_rx_max_closes", &s, &f, 1U);
	s.flags |= EFR32_SEQ_CONN_CENTRAL;
	expect("conn_len_gt_rx_max_closes_central", &s, &f, 0U);
	s = conn_stage;
	s.rx_max = 251U;
	data_pdu(&f, 2U, true, false, false, 251U);
	expect("conn_len_251_eq_rx_max", &s, &f, 1U);
	s.rx_max = 27U;
	data_pdu(&f, 2U, true, false, false, 200U);
	expect("conn_len_200_gt_rx_max", &s, &f, 1U);
	/* The trailer past the FIFO's end. */
	s.rx_offset = 0xf0U;
	data_pdu(&f, 2U, true, false, false, 28U);
	expect("conn_len_gt_rx_max_trailer_wraps", &s, &f, 1U);
	data_pdu(&f, 2U, true, false, false, 27U);
	expect("conn_len_eq_rx_max_trailer_wraps", &s, &f, 1U);

	s = (struct stage){.role = SCAN, .flags = EFR32_SEQ_CONN_SCAN | EFR32_SEQ_CONN_RX_FREE |
						  EFR32_SEQ_CONN_SCAN_REQ,
			   .rx_max = 8U, .rx_offset = 0x10U};
	adv_ind(&f, other);
	expect("scan_len_gt_rx_max", &s, &f, 0U);
	s = (struct stage){.role = ADV, .flags = EFR32_SEQ_CONN_ADV | EFR32_SEQ_CONN_ADV_TXADD,
			   .rx_max = 11U, .rx_offset = 0x10U};
	adv_pdu(&f, EFR32_SEQ_ADV_SCAN_REQ_TYPE, false, true, EFR32_SEQ_ADV_SCAN_REQ_LEN, scana,
		adva, 6U);
	expect("adv_len_gt_rx_max", &s, &f, 0U);
	s = (struct stage){.role = INIT, .flags = INIT_FLAGS, .rx_max = 8U, .rx_offset = 0x10U};
	adv_ind(&f, adva);
	expect("init_len_gt_rx_max", &s, &f, 0U);
}

/* 4: CRC failures; the second in a row (CRC_CLOSES) closes the event. */
ZTEST(sequencer_decide, test_4_crc_failure)
{
	struct frame f;
	struct stage s = conn_stage;

	data_pdu(&f, 2U, true, false, true, 3U);
	f.crc_ok = false;
	expect("periph_crc_fail_first", &s, &f, 1U);
	s.flags |= EFR32_SEQ_CONN_CRC_CLOSES;
	expect("periph_crc_fail_second", &s, &f, 1U);
	s.flags |= EFR32_SEQ_CONN_CENTRAL;
	expect("central_crc_fail_second", &s, &f, 0U);
	s.flags &= ~EFR32_SEQ_CONN_CRC_CLOSES;
	expect("central_crc_fail_first", &s, &f, 1U);
	s = conn_stage;
	s.flags |= EFR32_SEQ_CONN_SN | EFR32_SEQ_CONN_NESN | EFR32_SEQ_CONN_CRC_CLOSES;
	expect("periph_crc_fail_second_sn_nesn_1", &s, &f, 1U);
}

/* 5: no RX buffer free. */
ZTEST(sequencer_decide, test_5_no_rx_free)
{
	struct frame f;
	struct stage s = conn_stage;

	s.flags &= ~EFR32_SEQ_CONN_RX_FREE;
	data_pdu(&f, 2U, true, false, false, 3U);
	expect("conn_new_data_no_rx_free", &s, &f, 1U);
	s.flags |= EFR32_SEQ_CONN_NESN;
	data_pdu(&f, 2U, false, true, false, 3U);
	expect("conn_new_data_no_rx_free_nesn_1", &s, &f, 1U);

	s = (struct stage){.role = SCAN, .flags = EFR32_SEQ_CONN_SCAN | EFR32_SEQ_CONN_SCAN_REQ,
			   .rx_max = 37U, .rx_offset = 0x10U};
	adv_ind(&f, other);
	expect("scan_no_rx_free", &s, &f, 0U);
	s = (struct stage){.role = INIT, .flags = INIT_FLAGS & ~EFR32_SEQ_CONN_RX_FREE,
			   .rx_max = 37U, .rx_offset = 0x10U};
	direct_ind(&f, true, false, adva, inita);
	expect("init_no_rx_free", &s, &f, 0U);
	s = (struct stage){.role = ADV, .flags = EFR32_SEQ_CONN_ADV | EFR32_SEQ_CONN_ADV_TXADD,
			   .rx_max = 37U, .rx_offset = 0x10U};
	adv_pdu(&f, EFR32_SEQ_ADV_SCAN_REQ_TYPE, false, true, EFR32_SEQ_ADV_SCAN_REQ_LEN, scana,
		adva, 6U);
	expect("adv_no_rx_free_flag", &s, &f, 1U);
}

/* 6: the stage taken within EFR32_SEQ_RX2TX_DECISION_WAIT_US, or not at all. */
ZTEST(sequencer_decide, test_6_stage_timeout)
{
	struct frame f;
	struct stage s = conn_stage;

	data_pdu(&f, 1U, true, false, false, 0U);
	s.never_staged = true;
	expect("never_staged", &s, &f, 0U);
	s.never_staged = false;
	s.stage_after_us = 40U;
	expect("staged_40us_late", &s, &f, 1U);
	/* Either side of the bound: 120 passes of efr32_seq_delay(1), 12 cycles each besides the
	 * delay, end about 135 us after the mailbox in QEMU (7.97). */
	s.stage_after_us = 128U;
	expect("staged_128us_late", &s, &f, 1U);
	s.stage_after_us = 142U;
	expect("staged_142us_late", &s, &f, 0U);
	s.stage_after_us = 250U;
	expect("staged_250us_late", &s, &f, 0U);
}

static void *setup(void)
{
	volatile uint8_t *destination = (volatile uint8_t *)SEQ_RAM_BASE;
	uint32_t identity = 0U;

	CMU->RADIOCLKCTRL_SET = CMU_RADIOCLKCTRL_EN;
	CMU->CLKEN1_SET = CMU_CLKEN1_AGC | CMU_CLKEN1_MODEM | CMU_CLKEN1_RFCRC |
			  CMU_CLKEN1_FRC | CMU_CLKEN1_PROTIMER | CMU_CLKEN1_RAC |
			  CMU_CLKEN1_SYNTH | CMU_CLKEN1_BUFC | CMU_CLKEN1_RDMAILBOX1;
	CMU->CLKEN0_SET = CMU_CLKEN0_TIMER0;
	TIMER0->EN_SET = TIMER_EN_EN;
	TIMER0->TOP = UINT32_MAX;

	*reg(RAC_BASE + RAC_CTRL_OFFSET) = RAC_CTRL_ACTIVE_HIGH | RAC_CTRL_PA_ENABLE_HIGH |
					   RAC_CTRL_LNA_ENABLE_HIGH | RAC_CTRL_SEQRESET |
					   RAC_CTRL_FORCEDISABLE;
	__DSB();
	for (size_t i = 0U; i < efr32_radio_seq_image_size; i++) {
		destination[i] = efr32_radio_seq_image[i];
	}
	for (size_t i = 0U; i < tramp_image_size; i++) {
		destination[HARNESS_ENTRY + i] = tramp_image[i];
	}
	*harness(HARNESS_DECIDE) = SEQ_DECIDE_ADDR | 1U;
	*harness(HARNESS_DONE) = 0U;
	*reg(SEQ_IDENTITY_ADDRESS) = 0U;
	*reg(SEQ_RX2TX_DECISION_ADDRESS) = 0U;
	__DSB();
	*reg(RAC_BASE + RAC_SEQCTRL_OFFSET) = RAC_SEQCTRL_STAGE2_STARTUP;
	*reg(RAC_BASE + RAC_CTRL_OFFSET) = RAC_CTRL_ACTIVE_HIGH | RAC_CTRL_PA_ENABLE_HIGH |
					   RAC_CTRL_LNA_ENABLE_HIGH | RAC_CTRL_FORCEDISABLE |
					   RAC_CTRL_CPUWAITDIS;
	__DSB();
	for (uint32_t us = 0U; us < SEQ_READY_TIMEOUT_US && identity != SEQ_IDENTITY_OPEN_V1;
	     us += POLL_US) {
		k_busy_wait(POLL_US);
		identity = *reg(SEQ_IDENTITY_ADDRESS);
	}
	zassert_equal(identity, SEQ_IDENTITY_OPEN_V1, "the open sequencer did not start");
	for (uint32_t us = 0U; us < SEQ_READY_TIMEOUT_US &&
	     (*reg(SEQ_MAILBOX_IEN) & EFR32_SEQ_RDMAILBOX_TIMED_FLAG) == 0U; us += POLL_US) {
		k_busy_wait(POLL_US);
	}
	zassert_not_equal(*reg(SEQ_MAILBOX_IEN) & EFR32_SEQ_RDMAILBOX_TIMED_FLAG, 0U,
			  "the sequencer did not open its mailbox");
	/* The image is running its idle loop: its mailbox now runs the decision. */
	*reg(SEQ_MAILBOX_VECTOR) = HARNESS_ENTRY | 1U;
	__DSB();
	printk("DECIDE 0x%08x\n", SEQ_DECIDE_ADDR);
	return NULL;
}

ZTEST_SUITE(sequencer_decide, NULL, setup, NULL, NULL, NULL);
