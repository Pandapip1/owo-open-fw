/*
 * Start-by deadlines of the lower link layer's jobs, derived from the Bluetooth Core
 * Specification. SPDX-License-Identifier: Apache-2.0
 *
 * Every job's deadline is the latest start that still lets the on-air behaviour it serves meet
 * the specification, worked back through measured latencies: a spec quantity below (BT_*, with
 * its section of the Core Specification v6.1), less measured run and path times (Kconfig
 * EFR32_RADIO_*, each documented with its silicon measurement). Nothing is added to them. A job
 * that serves no spec timing gets the deadline of the spec-timed work that consumes its output.
 * docs/radio-tables.md section 7.77 tabulates the derivation.
 *
 * Times are k_cycle_get_32() cycles. Spec windows convert rounding down and measured latencies
 * rounding up, so every rounding moves a deadline earlier.
 */
#ifndef EFR32_RADIO_LL_DEADLINES_H_
#define EFR32_RADIO_LL_DEADLINES_H_

#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

/* T_IFS_150: end of one packet to the start of the next on the same channel (Vol 6 Part B 4.1.1). */
#define BT_T_IFS_150_US 150U
/*
 * Active clock accuracy: instantaneous timings deviate at most 2 us from the average (Vol 6
 * Part B 4.2.1). Applies to an advertising packet -> SCAN_REQ/CONNECT_IND and to SCAN_REQ ->
 * SCAN_RSP, i.e. to both T_IFS turnarounds of a scannable advertising event.
 */
#define BT_ACTIVE_CLOCK_JITTER_US 2U
/*
 * Sleep clock accuracy: instantaneous timings deviate at most 16 us from the average (Vol 6
 * Part B 4.2.2). Applies to the advertising interval and advDelay (4.4.2.2.1) and to ACL anchor
 * points, i.e. to every scheduled event start.
 */
#define BT_SLEEP_CLOCK_JITTER_US 16U
/* Beginning of one ADV_IND (ADV_SCAN_IND, ...) to the next in one event (Vol 6 Part B 4.4.2.3). */
#define BT_ADV_PDU_SPACING_MAX_US 10000U
/*
 * Preamble and Access Address on the LE 1M PHY: 1 + 4 octets at 1 Msym/s (Vol 6 Part B 2.1):
 * a receiver recognises a packet this long after it starts.
 */
#define BT_LE_1M_US_PER_OCTET 8U
#define BT_LE_1M_PREAMBLE_AA_US ((1U + 4U) * BT_LE_1M_US_PER_OCTET)
/* The CRC that ends every packet is 3 octets (Vol 6 Part B 2.1). */
#define BT_LE_CRC_OCTETS 3U
/* Shortest scan window a host can set: LE_Scan_Window 0x0004 x 0.625 ms (Vol 4 Part E 7.8.10). */
#define BT_SCAN_WINDOW_MIN_US 2500U
/*
 * Minimum Connection Event Spacing, T_MCES: end of the Peripheral's last packet to the next
 * anchor point, 150 us by default (Vol 6 Part B 4.1.5); this controller does not support the
 * Frame Space Update procedure that could change it (nor T_IFS_ACL_CP/PC, which therefore stay
 * at T_IFS_150 in every connection event, 4.1.1).
 */
#define BT_T_MCES_US 150U
/*
 * The Peripheral's windowWidening in an ACL connection is smaller than connInterval / 2 - T_MCES
 * (Vol 6 Part B 4.2.4), its J the sleep clock's 16 us: so a connection's next event, the Central's
 * anchor or the Peripheral's widened receive window, starts at least connInterval / 2 + T_MCES -
 * J after this one's.
 */
static inline uint32_t bt_conn_next_event_min_us(uint32_t interval_us)
{
	return interval_us / 2U + BT_T_MCES_US - BT_SLEEP_CLOCK_JITTER_US;
}

/* Spec window, rounded down to cycles. */
#define EFR32_DL_SPEC_CYC(us) ((uint32_t)k_us_to_cyc_floor32(us))
/* Measured latency, rounded up to cycles. */
#define EFR32_DL_MEAS_CYC(ns) ((uint32_t)k_ns_to_cyc_ceil32(ns))

/*
 * Timed command jobs (TX launch, QEMU TIFS turnaround) issue one radio command at an instant.
 * The job runs at most pre (measured) from its start to its hold, then holds with interrupts
 * locked until the instant and issues the command at most hold_late (measured) after it. A job
 * that starts by instant - pre therefore commands within [instant, instant + hold_late], and one
 * that starts later commands that much later: with a spec tolerance, its start-by deadline is
 * instant - pre + tolerance - hold_late. Its not-before bound is earlier than instant - pre by
 * the longest exit from the kernel's not-before wait (EFR32_RADIO_SPIN_EXIT_NS), so a release
 * on time starts in time; the job is released the wake latency before that bound.
 */
static inline uint32_t efr32_dl_hold_not_before(uint32_t instant, uint32_t pre_ns)
{
	return instant - EFR32_DL_MEAS_CYC(pre_ns) -
	       EFR32_DL_MEAS_CYC(CONFIG_EFR32_RADIO_SPIN_EXIT_NS);
}

static inline int efr32_dl_hold_start_by(uint32_t instant, uint32_t pre_ns, uint32_t tolerance_us,
					 uint32_t hold_late_ns)
{
	return (int)(instant - EFR32_DL_MEAS_CYC(pre_ns) + EFR32_DL_SPEC_CYC(tolerance_us) -
		     EFR32_DL_MEAS_CYC(hold_late_ns));
}

/* Clock-event expiry to the dispatch of the job it releases (measured). */
#define EFR32_DL_WAKE_CYC EFR32_DL_MEAS_CYC(CONFIG_EFR32_RADIO_WAKE_LATENCY_NS)

/*
 * Launches timed by the sequencer (docs/radio-tables.md 7.82). The launch job runs at most pre
 * (measured, EFR32_RADIO_{TX,RX}_LAUNCH_PRE_NS) from its start to posting its command; the
 * sequencer polls the instant at most EFR32_RADIO_SEQ_TIMED_WAKE_NS after the post and issues
 * the command at most EFR32_RADIO_SEQ_TIMED_HOLD_LATE_NS after the instant, or, polling only
 * after the instant, that long after its first poll. No M33 interrupt can come between: a job
 * that starts by instant - pre - wake commands within [instant, instant + hold_late], one that
 * starts later commands that much later. With a spec tolerance its start-by deadline is
 * instant - pre - wake + tolerance - hold_late, and the latest count at which the sequencer
 * still issues the command (its limit) is instant + tolerance - hold_late.
 *
 * The job has no not-before bound (it only posts): it is released one interrupt allowance
 * (EFR32_RADIO_INTERRUPT_BLOCKING_NS) before its start-by, so an interrupt arriving with the
 * release cannot make it late, and armed the wake latency before that.
 */
static inline int efr32_dl_timed_start_by(uint32_t instant, uint32_t pre_ns, uint32_t tolerance_us)
{
	return (int)(instant - EFR32_DL_MEAS_CYC(pre_ns) -
		     EFR32_DL_MEAS_CYC(CONFIG_EFR32_RADIO_SEQ_TIMED_WAKE_NS) +
		     EFR32_DL_SPEC_CYC(tolerance_us) -
		     EFR32_DL_MEAS_CYC(CONFIG_EFR32_RADIO_SEQ_TIMED_HOLD_LATE_NS));
}

static inline uint32_t efr32_dl_timed_limit(uint32_t instant, uint32_t tolerance_us)
{
	return instant + EFR32_DL_SPEC_CYC(tolerance_us) -
	       EFR32_DL_MEAS_CYC(CONFIG_EFR32_RADIO_SEQ_TIMED_HOLD_LATE_NS);
}

static inline uint32_t efr32_dl_timed_release(int start_by)
{
	return (uint32_t)start_by - EFR32_DL_MEAS_CYC(CONFIG_EFR32_RADIO_INTERRUPT_BLOCKING_NS);
}

/*
 * TX launch: the first packet of a scheduled event (ADV_IND on the first channel, a central's
 * anchor) starts at T_advEvent = advInterval + advDelay, held to the sleep clock's 16 us. TXEN
 * to the first bit is a fixed silicon path (91.7 us + d_tx, section 7.71), the same for every
 * event, so TXEN carries the event's timing.
 */
static inline int efr32_dl_tx_launch_start_by(uint32_t instant)
{
	return efr32_dl_timed_start_by(instant, CONFIG_EFR32_RADIO_TX_LAUNCH_PRE_NS,
				       BT_SLEEP_CLOCK_JITTER_US);
}

static inline uint32_t efr32_dl_tx_launch_limit(uint32_t instant)
{
	return efr32_dl_timed_limit(instant, BT_SLEEP_CLOCK_JITTER_US);
}

/*
 * RX launch of a Peripheral's receive window: a timed command too. The Peripheral listens from
 * windowWidening before the window's start (anchor point, or transmit window start; Vol 6 Part B
 * 4.2.4, 4.5.5, 4.5.7), and a receiver that is not yet searching when a frame's preamble reaches
 * it cannot receive that frame, so the receiver must be searching by then. The controller plans
 * SWRXEN at the instant, the ready delay (EFR32_RADIO_RX_READY_NS, SWRXEN to RXSEARCH) and
 * @p tolerance_us (hal/efr32/radio_conn.h) before that latest point: SWRXEN is held to the
 * instant and may come at most @p tolerance_us late.
 */
static inline int efr32_dl_rx_window_start_by(uint32_t instant, uint32_t tolerance_us)
{
	return efr32_dl_timed_start_by(instant, CONFIG_EFR32_RADIO_RX_LAUNCH_PRE_NS, tolerance_us);
}

static inline uint32_t efr32_dl_rx_window_limit(uint32_t instant, uint32_t tolerance_us)
{
	return efr32_dl_timed_limit(instant, tolerance_us);
}

/*
 * RX launch of a scan window: opens a receive window. The spec leaves the receiver's timing free
 * (Vol 6 Part B 4.2: "The clock used to time packet reception may have any accuracy"); a scanner
 * should listen for the whole window (4.4.3.1), which lasts at least BT_SCAN_WINDOW_MIN_US. The
 * receiver must be on before that window can end: SWRXEN by the end of the shortest window.
 */
static inline int efr32_dl_rx_launch_start_by(uint32_t instant)
{
	return (int)(instant + EFR32_DL_SPEC_CYC(BT_SCAN_WINDOW_MIN_US) -
		     EFR32_DL_MEAS_CYC(CONFIG_EFR32_RADIO_WCET_RX_LAUNCH_NS));
}

/*
 * Prepare chain. A ticker expiry at E schedules its event's TX launch at
 * E + EVENT_OVERHEAD_XTAL_US + EVENT_OVERHEAD_START_US (lll_event_offset_get() and the LLL
 * prepare, both in whole ticks, less the tick efr32_radio_launch_schedule() may borrow for its
 * remainder). The LLL prepare that arms the launch shares its priority and runs to completion,
 * so it must end by the launch's start-by; it is woken by the ULL worker's doorbell, which the
 * worker rings within its run.
 */
#define EFR32_DL_TICK_CYC \
	((uint32_t)((uint64_t)CONFIG_CLOCK_EVENT_SCHED_FREQUENCY / 32768U))
#define EFR32_DL_PREPARE_LEAD_CYC \
	((HAL_TICKER_US_TO_TICKS(EVENT_OVERHEAD_XTAL_US) + \
	  HAL_TICKER_US_TO_TICKS(EVENT_OVERHEAD_START_US) - 1U) * EFR32_DL_TICK_CYC)
/*
 * The longest the same chain can be: the launch's remainder after the borrowed tick is less than
 * one tick and one microsecond, so a launch instant is less than (XTAL + START + 1) ticks after
 * the expiry that prepared it.
 */
#define EFR32_DL_PREPARE_LEAD_MAX_CYC \
	((HAL_TICKER_US_TO_TICKS(EVENT_OVERHEAD_XTAL_US) + \
	  HAL_TICKER_US_TO_TICKS(EVENT_OVERHEAD_START_US) + 1U) * EFR32_DL_TICK_CYC)
/*
 * Instant to the latest start of a timed launch: the TX launch's, and the Peripheral RX
 * window's (efr32_dl_rx_window_start_by() with EVENT_TICKER_RES_MARGIN_US); the prepare chain
 * serves either, so it takes the earlier.
 */
#define EFR32_DL_TX_LAUNCH_LATEST_CYC \
	((int32_t)(EFR32_DL_SPEC_CYC(BT_SLEEP_CLOCK_JITTER_US) - \
		   EFR32_DL_MEAS_CYC(CONFIG_EFR32_RADIO_TX_LAUNCH_PRE_NS) - \
		   EFR32_DL_MEAS_CYC(CONFIG_EFR32_RADIO_SEQ_TIMED_WAKE_NS) - \
		   EFR32_DL_MEAS_CYC(CONFIG_EFR32_RADIO_SEQ_TIMED_HOLD_LATE_NS)))
#define EFR32_DL_RX_WINDOW_LATEST_CYC \
	((int32_t)(EFR32_DL_SPEC_CYC(EVENT_TICKER_RES_MARGIN_US) - \
		   EFR32_DL_MEAS_CYC(CONFIG_EFR32_RADIO_RX_LAUNCH_PRE_NS) - \
		   EFR32_DL_MEAS_CYC(CONFIG_EFR32_RADIO_SEQ_TIMED_WAKE_NS) - \
		   EFR32_DL_MEAS_CYC(CONFIG_EFR32_RADIO_SEQ_TIMED_HOLD_LATE_NS)))
#define EFR32_DL_LAUNCH_LATEST_CYC \
	((uint32_t)MIN(EFR32_DL_TX_LAUNCH_LATEST_CYC, EFR32_DL_RX_WINDOW_LATEST_CYC))
/*
 * Link-layer AES-CCM on an encrypted connection (Kconfig EFR32_RADIO_CCM_*, measured): the RX->TX
 * completion decrypts the received PDU and encrypts the reply before RESPOND, a Central's prepare
 * encrypts its anchor PDU, and either may first wait out one lower-priority accelerator run
 * (EFR32_RADIO_AES_BLOCKING_NS). The connection runs and paths in Kconfig were measured on
 * unencrypted links, so these are added to them.
 */
#if defined(CONFIG_BT_CTLR_LE_ENC) && defined(CONFIG_EFR32_RADIO_CONN_STAGED)
/*
 * With staged replies (7.82) a receive's completion decrypts the frame and stages the next two
 * replies, both encrypted, and commits without encrypting; a prepare stages the event's first
 * two replies, a Central's after encrypting its anchor PDU.
 */
#define EFR32_DL_CCM_STAGE_NS \
	(2U * CONFIG_EFR32_RADIO_CCM_ENCRYPT_NS + CONFIG_EFR32_RADIO_AES_BLOCKING_NS)
#define EFR32_DL_CCM_RX_TX_NS (CONFIG_EFR32_RADIO_CCM_DECRYPT_NS + EFR32_DL_CCM_STAGE_NS)
/* The decrypt (and one accelerator wait) ahead of the commit. */
#define EFR32_DL_CCM_RX_NS \
	(CONFIG_EFR32_RADIO_CCM_DECRYPT_NS + CONFIG_EFR32_RADIO_AES_BLOCKING_NS)
#define EFR32_DL_CCM_PREPARE_NS \
	(EFR32_DL_CCM_STAGE_NS + COND_CODE_1(CONFIG_BT_CENTRAL, \
		    (CONFIG_EFR32_RADIO_CCM_ENCRYPT_NS), (0)))
#elif defined(CONFIG_BT_CTLR_LE_ENC)
#define EFR32_DL_CCM_RX_TX_NS \
	(CONFIG_EFR32_RADIO_CCM_DECRYPT_NS + CONFIG_EFR32_RADIO_CCM_ENCRYPT_NS + \
	 CONFIG_EFR32_RADIO_AES_BLOCKING_NS)
#define EFR32_DL_CCM_PREPARE_NS \
	COND_CODE_1(CONFIG_BT_CENTRAL, \
		    (CONFIG_EFR32_RADIO_CCM_ENCRYPT_NS + CONFIG_EFR32_RADIO_AES_BLOCKING_NS), (0))
#else
#define EFR32_DL_CCM_RX_TX_NS 0
#define EFR32_DL_CCM_PREPARE_NS 0
#endif
#if !defined(EFR32_DL_CCM_RX_NS)
#define EFR32_DL_CCM_RX_NS 0
#endif
/* The ULL worker derives the session key with one AES-ECB block (hal/ecb.c, ull_llcp_enc.c). */
#define EFR32_DL_ECB_ULL_NS \
	COND_CODE_1(CONFIG_BT_CTLR_LE_ENC, \
		    (CONFIG_EFR32_RADIO_ECB_NS + CONFIG_EFR32_RADIO_AES_BLOCKING_NS), (0))
/* The LLL context's budget: its measured run plus a Central's anchor encryption. */
#define EFR32_DL_WCET_LLL_NS (CONFIG_EFR32_RADIO_WCET_LLL_NS + EFR32_DL_CCM_PREPARE_NS)

/* Ticker expiry to the latest start of the LLL prepare it causes. */
#define EFR32_DL_LLL_WINDOW_CYC \
	(EFR32_DL_PREPARE_LEAD_CYC + EFR32_DL_LAUNCH_LATEST_CYC - \
	 EFR32_DL_MEAS_CYC(EFR32_DL_WCET_LLL_NS))
/* Ticker expiry to the latest start of the ULL worker run that serves it. */
#define EFR32_DL_ULL_WINDOW_CYC \
	(EFR32_DL_LLL_WINDOW_CYC - EFR32_DL_MEAS_CYC(CONFIG_EFR32_RADIO_LLL_DOORBELL_LATENCY_NS) - \
	 EFR32_DL_MEAS_CYC(CONFIG_EFR32_RADIO_WCET_ULL_NS + EFR32_DL_ECB_ULL_NS))

/*
 * FRC completion of a transmission that the sequencer turns to RX (ADV_IND of a scannable
 * advertiser; a connection-event packet with more to come): the LLL callback must mark the
 * window PENDING before the reply (SCAN_REQ, CONNECT_IND; the peer's next connection-event
 * packet, active clock too, 4.2.1) can be recognised, i.e. before the earliest reply (T_IFS_150
 * less the active clock's 2 us after our last bit) has sent its preamble and Access Address. The
 * last bit leaves at the earliest frame_end_to_post + one blocked interrupt before the completion
 * was posted. @p to_pending_ns is the callback's measured path from its start to the PENDING
 * store.
 */
static inline int efr32_dl_completion_tx_rx_start_by(uint32_t post, uint32_t to_pending_ns)
{
	return (int)(post +
		     EFR32_DL_SPEC_CYC(BT_T_IFS_150_US - BT_ACTIVE_CLOCK_JITTER_US +
				       BT_LE_1M_PREAMBLE_AA_US) -
		     EFR32_DL_MEAS_CYC(CONFIG_EFR32_RADIO_FRAME_END_TO_COMPLETION_POST_NS) -
		     EFR32_DL_MEAS_CYC(CONFIG_EFR32_RADIO_INTERRUPT_BLOCKING_NS) -
		     EFR32_DL_MEAS_CYC(to_pending_ns));
}

/*
 * Connection completions with staged replies (docs/radio-tables.md 7.82). The sequencer answers
 * each received frame on its own at T_IFS; the M33's work only has to be done before the next
 * frame needs it. The shortest exchange: our reply starts T_IFS less the active clock's 2 us after
 * the received frame's end and is at least an empty PDU long; the peer's next frame starts as
 * soon after that and ends an empty PDU later.
 */
#define BT_LE_1M_EMPTY_PDU_US \
	((1U + 4U + 2U + BT_LE_CRC_OCTETS) * BT_LE_1M_US_PER_OCTET)
#define EFR32_DL_CONN_TURN_US (BT_T_IFS_150_US - BT_ACTIVE_CLOCK_JITTER_US)

/*
 * Receive completion that commits a staged reply of @p reply_us on air (isr_rx(): commit the
 * outcome, stage the replies to the next receive) to the frame that ended at @p eof: it re-arms
 * the RX FIFO (BUFC CLEAR, @p to_clear_ns from its start) before the next frame can begin, and
 * posts the next stage (@p to_staged_ns) before that frame can end, when the sequencer takes it.
 */
static inline int efr32_dl_rx_staged_start_by(uint32_t eof, uint32_t reply_us,
					      uint32_t to_clear_ns, uint32_t to_staged_ns)
{
	int32_t clear = (int32_t)(eof + EFR32_DL_SPEC_CYC(2U * EFR32_DL_CONN_TURN_US + reply_us) -
				  EFR32_DL_MEAS_CYC(to_clear_ns));
	int32_t staged = (int32_t)(eof + EFR32_DL_SPEC_CYC(2U * EFR32_DL_CONN_TURN_US + reply_us +
							    BT_LE_1M_EMPTY_PDU_US) -
				   EFR32_DL_MEAS_CYC(to_staged_ns));

	return ((staged - clear) < 0) ? (int)staged : (int)clear;
}

/* In a connection event the shortest reply is the empty PDU. */
static inline int efr32_dl_conn_rx_staged_start_by(uint32_t eof, uint32_t to_clear_ns,
						   uint32_t to_staged_ns)
{
	return efr32_dl_rx_staged_start_by(eof, BT_LE_1M_EMPTY_PDU_US, to_clear_ns, to_staged_ns);
}

/*
 * Transmit completion that turns to a staged receive (isr_tx(): label the receive, arm its
 * timeout): the receive's completion is held off while it runs, so it must end before that
 * completion's start-by. Our frame ended at the earliest frame_end_to_post and one blocked
 * interrupt before @p post; the receive's frame ends at the earliest one exchange later, and a
 * reply to it is @p reply_us on air.
 */
static inline int efr32_dl_tx_staged_start_by(uint32_t post, uint32_t run_ns, uint32_t reply_us,
					      uint32_t to_clear_ns, uint32_t to_staged_ns)
{
	uint32_t eof_next = post -
			    EFR32_DL_MEAS_CYC(CONFIG_EFR32_RADIO_FRAME_END_TO_COMPLETION_POST_NS) -
			    EFR32_DL_MEAS_CYC(CONFIG_EFR32_RADIO_INTERRUPT_BLOCKING_NS) +
			    EFR32_DL_SPEC_CYC(EFR32_DL_CONN_TURN_US + BT_LE_1M_EMPTY_PDU_US);

	return (int)((uint32_t)efr32_dl_rx_staged_start_by(eof_next, reply_us, to_clear_ns,
							    to_staged_ns) -
		     EFR32_DL_MEAS_CYC(run_ns));
}

static inline int efr32_dl_conn_tx_staged_start_by(uint32_t post, uint32_t run_ns,
						   uint32_t to_clear_ns, uint32_t to_staged_ns)
{
	return efr32_dl_tx_staged_start_by(post, run_ns, BT_LE_1M_EMPTY_PDU_US, to_clear_ns,
					   to_staged_ns);
}

/*
 * Scanner and initiator replies staged before the frame ends (docs/radio-tables.md 7.87). An
 * active scanner's SCAN_REQ (ScanA, AdvA: 12 octets, Vol 6 Part B 2.3.2.1) and an initiator's
 * CONNECT_IND (34 octets, 2.3.3.1), on LE 1M with preamble, Access Address, header and CRC.
 */
#define BT_LE_1M_SCAN_REQ_US \
	((1U + 4U + 2U + 12U + BT_LE_CRC_OCTETS) * BT_LE_1M_US_PER_OCTET)
#define BT_LE_1M_CONNECT_IND_US \
	((1U + 4U + 2U + 34U + BT_LE_CRC_OCTETS) * BT_LE_1M_US_PER_OCTET)
/*
 * The transmit window after a CONNECT_IND sent on LE 1M starts transmitWindowDelay (1.25 ms)
 * plus transmitWindowOffset (WinOffset x 1.25 ms) after the CONNECT_IND's end, and the
 * Central's first packet is its first anchor point (Vol 6 Part B 4.5.3; WinOffset and WinSize
 * in 1.25 ms units, 2.3.3.1).
 */
#define BT_TRANSMIT_WINDOW_DELAY_US 1250U
#define BT_TRANSMIT_WINDOW_UNIT_US 1250U

/*
 * The initiator's connection is set up after the CONNECT_IND: its transmit completion ends the
 * scan event (lll_done()), and the ULL worker that rings, started within EFR32_DL_ULL_WINDOW_CYC,
 * sets the connection up and starts its ticker within its run (EFR32_DL_INIT_AFTER_TX_CYC). The
 * first connection event's ticker expiry must come after that: it is at most
 * EFR32_DL_PREPARE_LEAD_MAX_CYC before the event's TX launch, which is the TX ready delay before
 * the anchor. Macros, as the prepare chain's constants need lll_vendor.h.
 */
#define EFR32_DL_INIT_AFTER_TX_CYC \
	(EFR32_DL_ULL_WINDOW_CYC + \
	 EFR32_DL_MEAS_CYC(CONFIG_EFR32_RADIO_WCET_ULL_NS + EFR32_DL_ECB_ULL_NS))
#define EFR32_DL_INIT_SETUP_CYC \
	(EFR32_DL_MEAS_CYC(CONFIG_EFR32_RADIO_WCET_COMPLETION_TX_NS) + EFR32_DL_INIT_AFTER_TX_CYC)
/* CONNECT_IND end to the latest end of the setup, for WinOffset @p win_offset. */
#define EFR32_DL_INIT_WINDOW_CYC(win_offset) \
	((int32_t)(EFR32_DL_SPEC_CYC(BT_TRANSMIT_WINDOW_DELAY_US + \
				     (win_offset) * BT_TRANSMIT_WINDOW_UNIT_US) - \
		   EFR32_DL_MEAS_CYC(CONFIG_EFR32_RADIO_TX_READY_NS) - EFR32_DL_PREPARE_LEAD_MAX_CYC))
/*
 * The initiator's WinOffset (docs/radio-tables.md 7.87). The transmit window is fixed relative
 * to the CONNECT_IND, so the WinOffset the setup chain needs does not depend on when the
 * advertisement was received: it is the smallest for which the CONNECT_IND's transmit
 * completion, released at the latest frame_end_to_post and one blocked interrupt after the
 * CONNECT_IND's end, still has one more interrupt and one LLL context run (the completion job
 * shares the LLL's cooperative priority) ahead of it before its start-by. OpenISA's initiator
 * otherwise sends 0 without BT_CTLR_SCHED_ADVANCED (which this module does not support), and
 * WinOffset 0 puts the first event's ticker expiry before the CONNECT_IND has been sent.
 */
#define EFR32_DL_INIT_NEED_CYC \
	((int32_t)(EFR32_DL_INIT_SETUP_CYC + \
		   EFR32_DL_MEAS_CYC(CONFIG_EFR32_RADIO_FRAME_END_TO_COMPLETION_POST_NS) + \
		   2U * EFR32_DL_MEAS_CYC(CONFIG_EFR32_RADIO_INTERRUPT_BLOCKING_NS) + \
		   EFR32_DL_MEAS_CYC(EFR32_DL_WCET_LLL_NS)))
#define EFR32_DL_CONNECT_IND_WIN_OFFSET \
	((EFR32_DL_INIT_NEED_CYC - EFR32_DL_INIT_WINDOW_CYC(0U)) <= 0 ? 0U : \
	 DIV_ROUND_UP((uint32_t)(EFR32_DL_INIT_NEED_CYC - EFR32_DL_INIT_WINDOW_CYC(0U)), \
		      EFR32_DL_SPEC_CYC(BT_TRANSMIT_WINDOW_UNIT_US)))
/*
 * The CONNECT_IND's transmit completion, posted at @p post, of @p run_ns: it and the ULL setup
 * after it end within the setup window of the CONNECT_IND's end, which was at the earliest
 * frame_end_to_post and one blocked interrupt before the post.
 */
#define efr32_dl_init_tx_start_by(post, run_ns) \
	((int)((uint32_t)(post) - \
	       EFR32_DL_MEAS_CYC(CONFIG_EFR32_RADIO_FRAME_END_TO_COMPLETION_POST_NS) - \
	       EFR32_DL_MEAS_CYC(CONFIG_EFR32_RADIO_INTERRUPT_BLOCKING_NS) + \
	       (uint32_t)EFR32_DL_INIT_WINDOW_CYC(EFR32_DL_CONNECT_IND_WIN_OFFSET) - \
	       EFR32_DL_INIT_AFTER_TX_CYC - EFR32_DL_MEAS_CYC(run_ns)))
/*
 * The receive completion, of @p run_ns, that commits the CONNECT_IND the sequencer sent to the
 * advertisement that ended at @p eof (the CONNECT_IND ends T_IFS, active clock included, and its
 * own air time later): it, the transmit completion and the setup end within the setup window.
 */
#define efr32_dl_init_commit_start_by(eof, run_ns) \
	((int)((uint32_t)(eof) + \
	       EFR32_DL_SPEC_CYC(EFR32_DL_CONN_TURN_US + BT_LE_1M_CONNECT_IND_US) + \
	       (uint32_t)EFR32_DL_INIT_WINDOW_CYC(EFR32_DL_CONNECT_IND_WIN_OFFSET) - \
	       EFR32_DL_INIT_SETUP_CYC - EFR32_DL_MEAS_CYC(run_ns)))

/*
 * Work whose output is the next packet of an advertising event (a completion or timeout that
 * may start the next ADV_IND): it starts within BT_ADV_PDU_SPACING_MAX_US of the previous
 * ADV_IND's start. @p last_tx is that packet's TXEN (the TXEN-to-air path is the same for both),
 * @p path_ns the longest path from this job's start to the next TXEN.
 */
static inline int efr32_dl_adv_next_start_by(uint32_t last_tx, uint32_t path_ns)
{
	return (int)(last_tx + EFR32_DL_SPEC_CYC(BT_ADV_PDU_SPACING_MAX_US) -
		     EFR32_DL_MEAS_CYC(path_ns));
}

/*
 * The same for a receive window (scanner): the receiver is re-enabled before the shortest scan
 * window could end.
 */
static inline int efr32_dl_scan_next_start_by(uint32_t post, uint32_t path_ns)
{
	return (int)(post + EFR32_DL_SPEC_CYC(BT_SCAN_WINDOW_MIN_US) - EFR32_DL_MEAS_CYC(path_ns));
}

/*
 * Work that closes a connection event (the Peripheral's last TX, an abort: window timeout,
 * failed reception, event end) serves the connection's next event: it shares the LLL priority
 * with the LLL prepare of that event and runs to completion, so it must end before that prepare's
 * latest start, EFR32_DL_LLL_WINDOW_CYC after the next event's ticker expiry, which comes at
 * most EFR32_DL_PREPARE_LEAD_MAX_CYC before the next event's launch, itself at the earliest
 * bt_conn_next_event_min_us() after this event's launch @p launch. @p run_ns is the closing
 * work's run. A macro, as the prepare chain's constants need lll_vendor.h.
 */
#define efr32_dl_conn_close_start_by(launch, interval_us, run_ns) \
	((int)((uint32_t)(launch) + EFR32_DL_SPEC_CYC(bt_conn_next_event_min_us(interval_us)) - \
	       EFR32_DL_PREPARE_LEAD_MAX_CYC + EFR32_DL_LLL_WINDOW_CYC - \
	       EFR32_DL_MEAS_CYC(run_ns)))

#endif /* EFR32_RADIO_LL_DEADLINES_H_ */
