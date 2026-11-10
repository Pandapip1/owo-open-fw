/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Open firmware for the EFR32BG22 private radio Cortex-M0+.
 *
 * This core implements only bounded peripheral mechanics.  It does not choose jobs, priorities,
 * channels, deadlines, or follow-up operations; those remain owned by Zephyr's M33 scheduler.
 * Additional handlers are added only after their register-level contracts are explained in
 * docs/radio-tables.md and covered by simulator tests.
 *
 * This file holds the bounded logic: reset, and the staged reply's decision, which runs inside the
 * RX->TX handler before the hold that sets T_IFS (SS7.82; 12.7-25.6 us on silicon against the
 * 135.4 us hold, SS7.91).  The state-event handlers, the timed command and the calibrated busy
 * wait are in radio_seq_core.S: their instruction sequences are what the timing rests on.
 */

#include <stdbool.h>
#include <stdint.h>

#include "seq_regs.h"

#define REG32(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define REG8(a)  (*(volatile uint8_t *)(uintptr_t)(a))
/* A word of the stage block, through `blk` (see stage_block()). */
#define CONN(off) REG32(blk + (off))
/* The scanner's and advertiser's paths stay out of line, so their registers are not taken from
 * the connection path, the one that runs every connection event. */
#define NOINLINE __attribute__((noinline))

/* radio_seq_core.S; both clobber r0-r1 only. */
void efr32_seq_delay(uint32_t units);
void efr32_seq_trace_count(uint32_t slot);

void efr32_radio_seq_reset(void);
uint32_t efr32_seq_conn_decide(void);

void efr32_radio_seq_reset(void)
{
	__asm__ volatile("cpsid i" ::: "memory");

	/* Publish image identity before accepting any peripheral interrupt. */
	REG32(EFR32_SEQ_IDENTITY_WORD) = EFR32_SEQ_IDENTITY_OPEN_V1;

	/* Discard retained state-transition indications, then expose only that bounded class to
	 * this core.  No operation is scheduled here. */
	REG32(EFR32_RAC_SEQIF_CLEAR) = EFR32_RAC_SEQIF_STATE_EVENTS;
	REG32(EFR32_RAC_SEQIEN_SET) = EFR32_RAC_SEQIF_STATE_EVENTS;

	/* Stock sets NVIC priority 1 for IRQ8/IRQ9 before enabling them (docs/radio-tables.md
	 * SS7.9); single-byte stores leave the neighbouring IRQs' priority bytes untouched. */
	REG8(EFR32_NVIC_IPR_IRQ8) = EFR32_NVIC_PRIORITY_TXWARM_TX;
	REG8(EFR32_NVIC_IPR_IRQ9) = EFR32_NVIC_PRIORITY_TXWARM_TX;
	REG8(EFR32_NVIC_IPR_IRQ10) = EFR32_NVIC_PRIORITY_TXWARM_TX;

	/* Stock's own RAC.STCTRL write and private-SysTick setup (SS7.9), without the SR3 bit 17
	 * wait stock gates it behind (see EFR32_RAC_STCTRL). */
	REG32(EFR32_RAC_STCTRL) = EFR32_RAC_STCTRL_VALUE;
	REG32(EFR32_SYSTICK_RELOAD) = (REG32(EFR32_SYSTICK_CALIB) & EFR32_SYSTICK_CALIB_TENMS_MASK) *
				      EFR32_SYSTICK_PERIOD_MULTIPLIER;
	REG32(EFR32_SYSTICK_CSR) = EFR32_SYSTICK_CSR_ENABLE_AND_CLKSOURCE;

	/* Timed commands arrive on RDMAILBOX1 (seq_abi.h, SS7.82): no message is pending at reset,
	 * then its one flag raises IRQ 23. */
	REG32(EFR32_SEQ_TIMED_BLOCK + EFR32_SEQ_TIMED_OP) = EFR32_SEQ_TIMED_OP_NONE;
	REG32(EFR32_SEQ_RDMAILBOX1_BASE + EFR32_SEQ_RDMAILBOX_FLAGS + EFR32_ALIAS_CLR) =
		EFR32_SEQ_RDMAILBOX_TIMED_FLAG;
	REG32(EFR32_SEQ_RDMAILBOX1_BASE + EFR32_SEQ_RDMAILBOX_IEN) = EFR32_SEQ_RDMAILBOX_TIMED_FLAG;

	/* Enable the IRQ of every state event enabled in SEQIEN above, so each is acknowledged,
	 * and the timed-command mailbox.  The M33 scheduler remains the sole source of radio-job
	 * policy: it chooses each command and its instant; this core only issues it then. */
	REG32(EFR32_NVIC_ISER0) = EFR32_RAC_IRQ_STATE_EVENTS | (1U << EFR32_SEQ_RDMAILBOX_IRQ);

	__asm__ volatile("cpsie i" ::: "memory");
	for (;;) {
		__asm__ volatile("wfi" ::: "memory");
	}
}

/* The stage block's address in a register: every field is then one load at an immediate offset,
 * not a literal-pool address each (the compiler would fold a constant base into every access). */
static inline uintptr_t stage_block(void)
{
	uintptr_t blk = EFR32_SEQ_CONN_BLOCK;

	__asm__("" : "+l"(blk));
	return blk;
}

/* The received frame in the RX FIFO, which wraps: base, mask and offset read once. */
struct rx {
	const volatile uint8_t *base;
	uint32_t mask;
	uint32_t off; /* the frame's first octet */
};

static inline uint32_t rx_octet(const struct rx *rx, uint32_t i)
{
	return rx->base[(rx->off + i) & rx->mask];
}

/* The 6-octet address at octet `at` of the received frame equals the one at offset `off` of the
 * stage block. */
static inline bool rx_addr_eq(uintptr_t blk, const struct rx *rx, uint32_t at, uint32_t off)
{
	const volatile uint8_t *mine = (const volatile uint8_t *)(blk + off);

	for (uint32_t i = 0; i < EFR32_SEQ_ADV_ADDR_LEN; i++) {
		if (rx_octet(rx, at + i) != mine[i]) {
			return false;
		}
	}
	return true;
}

/* n mod d (d not 0) by shift and subtract: libgcc is not linked into this image. */
static uint32_t umod(uint32_t n, uint32_t d)
{
	uint32_t s = d;

	while ((s & 0x80000000U) == 0U && (s << 1) <= n) {
		s <<= 1;
	}
	for (;;) {
		if (n >= s) {
			n -= s;
		}
		if (s == d) {
			return n;
		}
		s >>= 1;
	}
}

/*
 * A SCAN_REQ for this advertiser (seq_abi.h EFR32_SEQ_CONN_ADV): type, length, RxAdd and AdvA,
 * as isr_rx_pdu()'s isr_rx_sr_adva_check().
 */
static NOINLINE bool adv_scan_req_match(uintptr_t blk, const struct rx *rx, uint32_t hdr,
					uint32_t len, uint32_t flags)
{
	if ((hdr & EFR32_SEQ_ADV_TYPE_MASK) != EFR32_SEQ_ADV_SCAN_REQ_TYPE ||
	    len != EFR32_SEQ_ADV_SCAN_REQ_LEN) {
		return false;
	}
	/* RxAdd of the request is the advertiser's TxAdd. */
	if (((hdr >> EFR32_SEQ_ADV_RXADD_BIT) & 1U) != ((flags & EFR32_SEQ_CONN_ADV_TXADD) ? 1U : 0U)) {
		return false;
	}
	/* AdvA, at payload octet 6, against the staged one. */
	return rx_addr_eq(blk, rx, 2 + EFR32_SEQ_ADV_SCAN_REQ_ADVA, EFR32_SEQ_CONN_ADVA);
}

/*
 * The initiator's frame ended early enough in its scan slot for the CONNECT_IND (seq_abi.h
 * EFR32_SEQ_CONN_INIT_TIMED): its end on TIMER0 is now less RAC.STIMER (counting since RX2TX
 * began) in TIMER0 counts, less the RXFRAME exit.
 */
static bool init_in_slot(uintptr_t blk)
{
	uint32_t stimer = REG32(EFR32_RAC_STIMER) & 0xffffU;
	uint32_t t0 = REG32(EFR32_SEQ_TIMER0_CNT_NS);
	uint32_t at = t0 - ((stimer * CONN(EFR32_SEQ_SCAN_T0_PER_STIMER_Q8)) >>
			    EFR32_SEQ_SCAN_Q8_SHIFT);

	at -= CONN(EFR32_SEQ_SCAN_EOF_TO_EXIT);
	at -= CONN(EFR32_SEQ_SCAN_START);
	uint32_t period = CONN(EFR32_SEQ_SCAN_PERIOD);

	if (period != 0U) {
		at = umod(at, period);
	}
	return at < CONN(EFR32_SEQ_SCAN_LATEST);
}

/*
 * Whether a scanner's or initiator's received frame is answered (seq_abi.h EFR32_SEQ_CONN_SCAN),
 * as lll_scan.c's isr_rx_pdu() decides without privacy: an active scanner's SCAN_REQ, an
 * initiator's CONNECT_IND.
 */
static NOINLINE bool scan_match(uintptr_t blk, const struct rx *rx, uint32_t hdr, uint32_t len,
				uint32_t flags)
{
	/* An RX buffer for what the link layer reports. */
	if ((flags & EFR32_SEQ_CONN_RX_FREE) == 0U) {
		return false;
	}
	uint32_t type = hdr & EFR32_SEQ_ADV_TYPE_MASK;

	if ((flags & EFR32_SEQ_CONN_INIT) == 0U) {
		/* Active scanner: an ADV_IND or ADV_SCAN_IND, with its AdvA. */
		return (flags & EFR32_SEQ_CONN_SCAN_REQ) != 0U &&
		       (type == EFR32_SEQ_ADV_IND_TYPE || type == EFR32_SEQ_ADV_SCAN_IND_TYPE) &&
		       len >= EFR32_SEQ_ADV_ADDR_LEN && len <= EFR32_SEQ_ADV_IND_LEN_MAX;
	}
	/* Initiator: from the advertiser it connects to (TxAdd, then AdvA below). */
	if (((hdr >> EFR32_SEQ_ADV_TXADD_BIT) & 1U) != ((flags & EFR32_SEQ_CONN_ADV_TXADD) ? 1U : 0U)) {
		return false;
	}
	if (type == EFR32_SEQ_ADV_IND_TYPE) {
		if (len < EFR32_SEQ_ADV_ADDR_LEN || len > EFR32_SEQ_ADV_IND_LEN_MAX) {
			return false;
		}
	} else if (type == EFR32_SEQ_ADV_DIRECT_IND_TYPE) {
		if (len != EFR32_SEQ_ADV_DIRECT_IND_LEN) {
			return false;
		}
		/* TargetA: with the extended filter policy any resolvable private address (RxAdd 1),
		 * else ours. */
		uint32_t rxadd = hdr >> EFR32_SEQ_ADV_RXADD_BIT;
		bool rpa = (flags & EFR32_SEQ_CONN_INIT_TGTA_RPA) != 0U && rxadd == 1U &&
			   (rx_octet(rx, 2 + EFR32_SEQ_ADV_DIRECT_TARGETA + EFR32_SEQ_ADDR_TYPE_OCTET) &
			    EFR32_SEQ_ADDR_TYPE_MASK) == EFR32_SEQ_ADDR_TYPE_RPA;

		if (!rpa &&
		    (rxadd != ((flags & EFR32_SEQ_CONN_INIT_RXADD) ? 1U : 0U) ||
		     !rx_addr_eq(blk, rx, 2 + EFR32_SEQ_ADV_DIRECT_TARGETA, EFR32_SEQ_CONN_INITA))) {
			return false;
		}
	} else {
		return false;
	}
	if (!rx_addr_eq(blk, rx, 2, EFR32_SEQ_CONN_ADVA)) {
		return false;
	}
	return (flags & EFR32_SEQ_CONN_INIT_TIMED) == 0U || init_in_slot(blk);
}

/* FRC frames a reply from its length octet: restore the length-pending word count the M33's
 * radio_pkt_configure() sets before every TX (a received frame left its own there), then CLEAR
 * the BUFC TX FIFO the M33 configured and write the first octet. */
static inline void tx_fifo_start(uint32_t first)
{
	REG32(EFR32_SEQ_FRC_WCNTCMP0) = EFR32_SEQ_FRC_WCNTCMP0_LENGTH_PENDING;
	REG32(EFR32_SEQ_BUFC_TX_CMD) = EFR32_SEQ_BUFC_CMD_CLEAR;
	REG32(EFR32_SEQ_BUFC_TX_WRITEDATA) = first;
}

/* The event continues: the next receive is armed and staged, as after a launched TX
 * (efr32_radio_tx_start(), SS7.63). */
static inline void arm_next_receive(uint32_t outcome)
{
	if ((outcome & EFR32_SEQ_CONN_OUT_DONE) == 0U) {
		REG32(EFR32_SEQ_RX2TX_DECISION) = EFR32_SEQ_RX2TX_STAGED;
		REG32(EFR32_RAC_RXENSRCEN + EFR32_ALIAS_SET) = EFR32_RAC_RXENSRCEN_SWRXEN_BIT;
	}
}

/* Sends the staged PDU at `pdu` (`octets` long) with first octet `first`. */
static uint32_t send(const volatile uint8_t *pdu, uint32_t octets, uint32_t first, uint32_t outcome)
{
	tx_fifo_start(first);
	for (uint32_t i = 1; i < octets; i++) {
		REG32(EFR32_SEQ_BUFC_TX_WRITEDATA) = pdu[i];
	}
	arm_next_receive(outcome);
	return 1;
}

/* The SCAN_REQ: RxAdd the frame's TxAdd, AdvA the frame's; the SCAN_RSP receive follows it.
 * OUTCOME is published once AdvA is read: the M33 then re-arms the RX FIFO (and its offset
 * word) for that receive. */
static NOINLINE uint32_t send_scan_req(uintptr_t blk, const struct rx *rx, uint32_t hdr,
				       uint32_t out)
{
	const volatile uint8_t *pdu = (const volatile uint8_t *)CONN(EFR32_SEQ_CONN_CUR);
	uint32_t first = (pdu[0] & ~(1U << EFR32_SEQ_ADV_RXADD_BIT)) |
			 (((hdr >> EFR32_SEQ_ADV_TXADD_BIT) & 1U) << EFR32_SEQ_ADV_RXADD_BIT);

	out |= EFR32_SEQ_CONN_OUT_VALID | EFR32_SEQ_CONN_OUT_REPLY;
	tx_fifo_start(first);
	for (uint32_t i = 1; i < EFR32_SEQ_SCAN_REQ_ADVA_OCTET; i++) {
		REG32(EFR32_SEQ_BUFC_TX_WRITEDATA) = pdu[i];
	}
	/* AdvA: the frame's payload octets 0-5; the FIFO wraps. */
	for (uint32_t i = 0; i < EFR32_SEQ_ADV_ADDR_LEN; i++) {
		REG32(EFR32_SEQ_BUFC_TX_WRITEDATA) = rx_octet(rx, 2 + i);
	}
	CONN(EFR32_SEQ_CONN_OUTCOME) = out;
	arm_next_receive(out);
	return 1;
}

/*
 * Staged connection reply (seq_abi.h, docs/radio-tables.md SS7.82).  Takes the stage (bounded
 * wait, as for the M33's decision), reads the received header and the CRC trailer from the RX
 * FIFO, decides as lll_conn.c's isr_rx() does, publishes OUTCOME and, when a reply goes out,
 * writes it to the BUFC TX FIFO with its SN and NESN; when the event continues it arms the next
 * receive.  Returns 1 to send the reply, 0 to send nothing.  Called from the RX->TX handler.
 */
uint32_t efr32_seq_conn_decide(void)
{
	uintptr_t blk = stage_block();
	uint32_t state;
	uint32_t wait = EFR32_SEQ_RX2TX_DECISION_WAIT_US;

	/* Assembly: each pass's cycles are part of the wait's bound (the reply must start within
	 * T_IFS), so the loop is radio_seq.S's, instruction for instruction (radio-tables.md 7.97). */
	__asm__ volatile(".syntax unified\n"
			 "1:  ldr %[state], [%[blk], #%c[off]]\n"
			 "    movs r1, #%c[staged]\n"
			 "    tst %[state], r1\n"
			 "    bne 2f\n"
			 "    movs r0, #1\n"
			 "    bl efr32_seq_delay\n"
			 "    subs %[wait], #1\n"
			 "    bne 1b\n"
			 "2:\n"
			 ".syntax divided\n"
			 : [state] "=&l"(state), [wait] "+l"(wait)
			 : [blk] "l"(blk), [off] "i"(EFR32_SEQ_CONN_STATE),
			   [staged] "i"(EFR32_SEQ_CONN_STAGED)
			 : "r0", "r1", "r2", "r3", "lr", "cc", "memory");
	if (wait == 0U) {
		/* Not staged in time: nothing is sent; the M33 finds no outcome. */
		efr32_seq_trace_count(EFR32_SEQ_TRACE_RX2TX_TIMEOUT_SLOT);
		return 0;
	}
	uint32_t out = (state >> EFR32_SEQ_CONN_GENERATION_SHIFT) << EFR32_SEQ_CONN_GENERATION_SHIFT;

	CONN(EFR32_SEQ_CONN_STATE) = 0;
	CONN(EFR32_SEQ_CONN_TAKEN_AT) = REG32(EFR32_RAC_STIMER);
	uint32_t flags = CONN(EFR32_SEQ_CONN_FLAGS);

	/* Header, length and trailer octets of the frame, the FIFO wrapping. */
	const volatile uint8_t *base = (const volatile uint8_t *)CONN(EFR32_SEQ_CONN_RX_BASE);
	uint32_t off = CONN(EFR32_SEQ_CONN_RX_OFFSET);
	uint32_t mask = CONN(EFR32_SEQ_CONN_RX_MASK);
	const struct rx rx = {.base = base, .mask = mask, .off = off};
	uint32_t hdr = rx_octet(&rx, 0);
	uint32_t len = rx_octet(&rx, 1);
	uint32_t trailer = rx_octet(&rx, 2 + len);
	bool crc_ok = len <= CONN(EFR32_SEQ_CONN_RX_MAX) &&
		      (trailer & EFR32_SEQ_FRC_TRAILER_CRCOK) != 0U;

	if (crc_ok) {
		out |= EFR32_SEQ_CONN_OUT_CRC_OK;
	}

	if ((flags & EFR32_SEQ_CONN_SCAN) != 0U) {
		/* A scanner's or initiator's receive (seq_abi.h EFR32_SEQ_CONN_SCAN). */
		if (!crc_ok || !scan_match(blk, &rx, hdr, len, flags)) {
			/* Nothing is sent; the scanner listens on. */
			CONN(EFR32_SEQ_CONN_OUTCOME) = out | EFR32_SEQ_CONN_OUT_VALID;
			return 0;
		}
		if ((flags & EFR32_SEQ_CONN_INIT) == 0U) {
			return send_scan_req(blk, &rx, hdr, out);
		}
		/* The CONNECT_IND goes out unchanged and ends the event. */
	} else if ((flags & EFR32_SEQ_CONN_ADV) != 0U) {
		/* An advertiser answers nothing but a matching SCAN_REQ; the event then ends. */
		if (!crc_ok || !adv_scan_req_match(blk, &rx, hdr, len, flags)) {
			CONN(EFR32_SEQ_CONN_OUTCOME) =
				out | EFR32_SEQ_CONN_OUT_VALID | EFR32_SEQ_CONN_OUT_DONE;
			return 0;
		}
	} else {
		/* A connection receive. */
		if (crc_ok) {
			/* Acknowledged: received NESN differs from our SN. */
			if (((hdr >> EFR32_SEQ_PDU_NESN_BIT) ^ (flags >> EFR32_SEQ_CONN_SN_SHIFT)) & 1U) {
				out |= EFR32_SEQ_CONN_OUT_ACK;
			}
			/* New data: received SN equals our NESN, and a buffer is free. */
			if ((((hdr >> EFR32_SEQ_PDU_SN_BIT) ^ (flags >> EFR32_SEQ_CONN_NESN_SHIFT)) &
			     1U) == 0U &&
			    (flags & EFR32_SEQ_CONN_RX_FREE) != 0U) {
				out |= EFR32_SEQ_CONN_OUT_ACCEPT;
			}
		} else {
			/* CRC failed (a frame too long counts as one): the second in a row closes
			 * the event; the received MD is not used. */
			if ((flags & EFR32_SEQ_CONN_CRC_CLOSES) != 0U) {
				out |= EFR32_SEQ_CONN_OUT_DONE;
			}
			hdr = EFR32_SEQ_CONN_PDU_MD;
		}
		bool next = (out & EFR32_SEQ_CONN_OUT_ACK) != 0U;
		const volatile uint8_t *pdu = (const volatile uint8_t *)CONN(
			next ? EFR32_SEQ_CONN_NEXT : EFR32_SEQ_CONN_CUR);
		uint32_t octets = CONN(next ? EFR32_SEQ_CONN_NEXT_OCTETS : EFR32_SEQ_CONN_CUR_OCTETS);

		/* Closed by a valid frame without MD answered by an empty PDU. */
		if ((out & EFR32_SEQ_CONN_OUT_CRC_OK) != 0U &&
		    (hdr & EFR32_SEQ_CONN_PDU_MD) == 0U && pdu[1] == 0U) {
			out |= EFR32_SEQ_CONN_OUT_DONE;
		}
		out |= EFR32_SEQ_CONN_OUT_VALID;
		/* A Central that is done sends nothing. */
		if ((out & EFR32_SEQ_CONN_OUT_DONE) != 0U && (flags & EFR32_SEQ_CONN_CENTRAL) != 0U) {
			CONN(EFR32_SEQ_CONN_OUTCOME) = out;
			return 0;
		}
		out |= EFR32_SEQ_CONN_OUT_REPLY;
		CONN(EFR32_SEQ_CONN_OUTCOME) = out;
		/* Reply header: SN ^ ack, NESN ^ accept. */
		uint32_t first = pdu[0] & ~(uint32_t)EFR32_SEQ_CONN_PDU_SEQ_BITS;

		first |= (((flags >> EFR32_SEQ_CONN_SN_SHIFT) ^ (out >> EFR32_SEQ_CONN_OUT_ACK_SHIFT)) &
			  1U) << EFR32_SEQ_PDU_SN_BIT;
		first |= (((flags >> EFR32_SEQ_CONN_NESN_SHIFT) ^
			   (out >> EFR32_SEQ_CONN_OUT_ACCEPT_SHIFT)) & 1U) << EFR32_SEQ_PDU_NESN_BIT;
		return send(pdu, octets, first, out);
	}
	/* A matching SCAN_REQ (advertiser) or the CONNECT_IND (initiator): the staged PDU as it is,
	 * and the event ends. */
	out |= EFR32_SEQ_CONN_OUT_VALID | EFR32_SEQ_CONN_OUT_DONE | EFR32_SEQ_CONN_OUT_REPLY;
	CONN(EFR32_SEQ_CONN_OUTCOME) = out;
	const volatile uint8_t *pdu = (const volatile uint8_t *)CONN(EFR32_SEQ_CONN_CUR);

	return send(pdu, CONN(EFR32_SEQ_CONN_CUR_OCTETS), pdu[0], out);
}
