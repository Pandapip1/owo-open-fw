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
 * TX launch: the first packet of a scheduled event (ADV_IND on the first channel, a central's
 * anchor) starts at T_advEvent = advInterval + advDelay, held to the sleep clock's 16 us. TXEN
 * to the first bit is a fixed silicon path (91.7 us + d_tx, section 7.71), the same for every
 * event, so TXEN carries the event's timing.
 */
static inline uint32_t efr32_dl_tx_launch_not_before(uint32_t instant)
{
	return efr32_dl_hold_not_before(instant, CONFIG_EFR32_RADIO_TX_LAUNCH_PRE_NS);
}

static inline int efr32_dl_tx_launch_start_by(uint32_t instant)
{
	return efr32_dl_hold_start_by(instant, CONFIG_EFR32_RADIO_TX_LAUNCH_PRE_NS,
				      BT_SLEEP_CLOCK_JITTER_US,
				      CONFIG_EFR32_RADIO_TX_LAUNCH_HOLD_LATE_NS);
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
static inline uint32_t efr32_dl_rx_window_not_before(uint32_t instant)
{
	return efr32_dl_hold_not_before(instant, CONFIG_EFR32_RADIO_RX_LAUNCH_PRE_NS);
}

static inline int efr32_dl_rx_window_start_by(uint32_t instant, uint32_t tolerance_us)
{
	return efr32_dl_hold_start_by(instant, CONFIG_EFR32_RADIO_RX_LAUNCH_PRE_NS, tolerance_us,
				      CONFIG_EFR32_RADIO_RX_LAUNCH_HOLD_LATE_NS);
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
		   EFR32_DL_MEAS_CYC(CONFIG_EFR32_RADIO_TX_LAUNCH_HOLD_LATE_NS)))
#define EFR32_DL_RX_WINDOW_LATEST_CYC \
	((int32_t)(EFR32_DL_SPEC_CYC(EVENT_TICKER_RES_MARGIN_US) - \
		   EFR32_DL_MEAS_CYC(CONFIG_EFR32_RADIO_RX_LAUNCH_PRE_NS) - \
		   EFR32_DL_MEAS_CYC(CONFIG_EFR32_RADIO_RX_LAUNCH_HOLD_LATE_NS)))
#define EFR32_DL_LAUNCH_LATEST_CYC \
	((uint32_t)MIN(EFR32_DL_TX_LAUNCH_LATEST_CYC, EFR32_DL_RX_WINDOW_LATEST_CYC))
/* Ticker expiry to the latest start of the LLL prepare it causes. */
#define EFR32_DL_LLL_WINDOW_CYC \
	(EFR32_DL_PREPARE_LEAD_CYC + EFR32_DL_LAUNCH_LATEST_CYC - \
	 EFR32_DL_MEAS_CYC(CONFIG_EFR32_RADIO_WCET_LLL_NS))
/* Ticker expiry to the latest start of the ULL worker run that serves it. */
#define EFR32_DL_ULL_WINDOW_CYC \
	(EFR32_DL_LLL_WINDOW_CYC - EFR32_DL_MEAS_CYC(CONFIG_EFR32_RADIO_LLL_DOORBELL_LATENCY_NS) - \
	 EFR32_DL_MEAS_CYC(CONFIG_EFR32_RADIO_WCET_ULL_NS))

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
