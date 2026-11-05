/*
 * Radio HAL of the EFR32BG22 lower link layer: the API contract of Zephyr's OpenISA LLL
 * (subsys/bluetooth/controller/ll_sw/openisa/hal/RV32M1/radio/radio.h, 78 functions).
 * SPDX-License-Identifier: Apache-2.0
 *
 * *** UNVALIDATED, UNFINISHED: public generated register layouts are now known,
 * but the required radio/sequencer behavior is not fully characterized. ***
 * Every function below is classified by what it is blocked on:
 *   software      needs no register access, simply not written yet
 *   regs-*        needs register knowledge listed in docs/radio-tables.md (T1..T9)
 * Until those exist the functions only keep software state; anything that would put a packet on the
 * air asserts through EFR32_RADIO_TODO() instead of writing guessed registers.
 */
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/irq.h>
#include <zephyr/kernel/clock_event.h>
#include <zephyr/kernel/deadline_miss.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <soc.h>
#include "hal/efr32/radioaes.h"

#include "hal/ccm.h"
#include "hal/cntr.h"
#include "hal/efr32/cntr_efr32.h"
#include "hal/efr32/ll_threads.h"
#include "hal/efr32/ll_deadlines.h"
#include "hal/efr32/seq_abi.h"
#include "hal/efr32/ticker.h"
#include "hal/efr32/radio_conn.h"
#include "lll/lll_vendor.h"
#include "abort_lifecycle.h"
#include "completion_dispatch.h"
#include "hal/efr32/ll_irqs.h"
#include "hal/radio.h"
#include "hal/debug.h"
#include "packet_stage_lifecycle.h"
#include "rx_pdu_delivery.h"
#if defined(CONFIG_EFR32_RADIO_QEMU_RX_DESCRIPTOR)
#include "rx_descriptor.h"
#endif
#if defined(CONFIG_EFR32_RADIO_QEMU_TIFS)
#include "qemu_tifs_state.h"
#endif
#include "timer_state.h"
#if defined(CONFIG_EFR32_RADIO_TX_POWER)
#include "tx_power.h"
#endif
#if defined(CONFIG_EFR32_RADIO_AMATEUR)
#include "amateur_radio.h"
#endif

#include "util/mem.h"
#include "util/memq.h"
#include "util/mayfly.h"
#include "common/rpa.h"

#include "lll/pdu_vendor.h"
#include "ll_sw/pdu.h"

/*
 * Marks a function that cannot be implemented without the radio register map: logs once. With
 * immediate logging a printk blocks its caller for the whole line at the console baud rate
 * (milliseconds); even deferred, formatting it took 40-150 us in QEMU, and the lower-link-layer
 * contexts and radio jobs (mayfly_is_running()) have start-by deadlines that do not include it:
 * print only from other threads, with the console deferred.
 */
#define EFR32_RADIO_TODO(tag) \
	do { \
		static bool efr32_once_; \
		if (!IS_ENABLED(CONFIG_LOG_MODE_IMMEDIATE) && !efr32_once_ && \
		    !k_is_in_isr() && !mayfly_is_running()) { \
			efr32_once_ = true; \
			printk("efr32-radio: %s needs %s (docs/radio-tables.md)\n", __func__, tag); \
		} \
	} while (0)

/* Same, for the functions that would start air activity: refuse to run on stubs. */
#define EFR32_RADIO_AIR_TODO(tag) \
	do { \
		EFR32_RADIO_TODO(tag); \
		LL_ASSERT_MSG(!IS_ENABLED(CONFIG_EFR32_RADIO_STUB_REGISTERS), \
			      "radio register map unknown"); \
	} while (0)

#if defined(CONFIG_EFR32_RADIO_BRINGUP_TRACE)
#define EFR32_RADIO_TRACE_ONCE(format, ...) \
	do { \
		static bool efr32_trace_once_; \
		if (!efr32_trace_once_) { \
			efr32_trace_once_ = true; \
			printk("efr32-radio trace: " format "\n", ##__VA_ARGS__); \
		} \
	} while (0)
#else
#define EFR32_RADIO_TRACE_ONCE(...) do { } while (0)
#endif

/*
 * OpenISA's abort paths install a cleanup callback and then call
 * radio_disable().  The RV32M1 HAL completes that sequence with a timer IRQ;
 * RAC's RXDIS/TXDIS commands do not produce either FRC completion latch, so
 * retain an explicit, non-PDU completion reason for the same controller
 * lifecycle.
 */
static struct efr32_radio_abort_lifecycle efr32_radio_abort;

#if defined(CONFIG_BT_CTLR_PRIVACY)
#define EFR32_RADIO_AR_NO_MATCH 0xffU
/*
 * ull_filter_lll_irks_get() supplies AAR-order (big-endian) IRKs. Retain an
 * LE snapshot because bt_rpa_irk_matches() implements BLE ah in that order,
 * and because the caller does not promise the source array's lifetime.
 */
static struct {
	bool armed;
	uint8_t count;
	uint8_t match;
	uint8_t irks[CONFIG_BT_CTLR_RL_SIZE][16];
} efr32_radio_ar = {
	.match = EFR32_RADIO_AR_NO_MATCH,
};

/*
 * This is deliberately reached only from the normalized-PDU delivery seam.
 * In particular, raw BUFC channel-1 bytes and descriptor data are not valid
 * inputs: their framing and appended-info policy remain unproven.
 */
static void efr32_radio_ar_execute(const uint8_t *pdu, uint16_t bytes)
{
	const struct pdu_adv *adv = (const struct pdu_adv *)pdu;
	bt_addr_t rpa;
	uint8_t i;

	if (!efr32_radio_ar.armed) {
		return;
	}

	efr32_radio_ar.armed = false;
	efr32_radio_ar.match = EFR32_RADIO_AR_NO_MATCH;
	/* Two PDU header octets plus the advertiser address are required. */
	if (bytes < 8U || adv->len < sizeof(rpa.val) ||
	    bytes < (uint16_t)(PDU_AC_LL_HEADER_SIZE + adv->len) || !adv->tx_addr) {
		return;
	}

	memcpy(rpa.val, adv->payload, sizeof(rpa.val));
	if (!BT_ADDR_IS_RPA(&rpa)) {
		return;
	}

	for (i = 0U; i < efr32_radio_ar.count; i++) {
		if (bt_rpa_irk_matches(efr32_radio_ar.irks[i], &rpa)) {
			efr32_radio_ar.match = i;
			break;
		}
	}
}
#endif /* CONFIG_BT_CTLR_PRIVACY */
/*
 * This state is intentionally distinct from PROTIMER.  It mirrors only
 * OpenISA software bookkeeping; it never makes a timer expire or starts a
 * radio operation.  See timer_state.c for the reference line citations.
 */
static struct efr32_radio_timer_state efr32_radio_timer;
static atomic_t efr32_radio_launch_generation;
static atomic_t efr32_radio_launch_active;
/* Set once by the missed-deadline policy: no wake may release radio work again. */
static atomic_t efr32_radio_halted;
/* Generation mailboxes pair the coalescing event latch with its expiry. */
static atomic_t efr32_radio_tx_pending_generation;
static atomic_t efr32_radio_rx_pending_generation;
/* Instant the pending TX launch holds TXEN to, and the latest it may still issue it. */
static atomic_t efr32_radio_tx_instant;
static atomic_t efr32_radio_tx_limit;
/* The pending RX launch's instant, and whether it holds SWRXEN to it (a Peripheral's window). */
static atomic_t efr32_radio_rx_instant;
static atomic_t efr32_radio_rx_limit;
static atomic_t efr32_radio_rx_hold;
/*
 * Whether the current radio event began with a scheduled TX (an advertising event) rather than a
 * receive window, and the TXEN of its latest packet started by radio_tx_enable(): the work that
 * may start the next ADV_IND is due BT_ADV_PDU_SPACING_MAX_US after it.
 */
static bool efr32_radio_event_tx;
static uint32_t efr32_radio_last_txen;
static int efr32_radio_next_start_by(uint32_t now, uint32_t path_ns);
/*
 * Event timer (docs/radio-tables.md 7.79). OpenISA's radio_tmr_*_get() microseconds count from
 * the start tick of the event's radio_tmr_start(), here efr32_radio_start_cyc on the scheduler
 * clock; efr32_radio_launch_cyc is the launch instant (start tick + remainder) and
 * efr32_radio_ready_us when the radio is planned to be ready: the receiver searching, or the
 * first preamble bit leaving.
 */
static uint32_t efr32_radio_start_cyc;
static uint32_t efr32_radio_launch_cyc;
static uint32_t efr32_radio_ready_us;
/*
 * End (MODEM EOF) of the frame last received in this event and its PDU length (header and
 * payload octets), taken from RAC.STIMER while the RAC still holds the RX2TX state it entered at
 * the frame's end (efr32_radio_rx_end_capture()).
 */
static uint32_t efr32_radio_rx_eof_cyc;
static uint16_t efr32_radio_rx_pdu_octets;
static bool efr32_radio_rx_eof_valid;
/*
 * Connection interval of the connection event being run, 0 outside one, and the receive-window
 * tolerance of the next RX launch, 0 for a scan window (hal/efr32/radio_conn.h).
 */
static uint32_t efr32_radio_conn_interval_us;
static uint32_t efr32_radio_rx_window_tolerance_us;

/*
 * Hold a timed command to @p instant (interrupts locked by the caller). One read of the
 * scheduler clock's logical time takes about 2 us in QEMU, the whole active clock tolerance, and
 * one read through the active provider about 1.1 us on silicon (it waits for TIMER0's
 * SYNCBUSY), so the instant is mapped once onto the active provider's count and TIMER0.CNT, that
 * count, is polled directly: the hold ends at most two counter loads after the instant (Kconfig
 * *_HOLD_LATE_NS, measured with the command store), and never before it, as the mapping rounds
 * up. efr32_radio_hold_init() checks that TIMER0 is the active provider.
 */
#define EFR32_HOLD_TIMER ((TIMER_TypeDef *)DT_REG_ADDR(DT_NODELABEL(timer0)))
static bool efr32_radio_hold_direct;

#if defined(CONFIG_EFR32_RADIO_TIMING_PROBE)
/* The TIMER0 count the last direct hold waited for, 0 when it did not wait (timing probe). */
static uint32_t efr32_radio_hold_waited_count;
/* When the last direct hold, its instant mapped, began to poll. */
static uint32_t efr32_radio_hold_entry;
#endif

static inline void efr32_radio_hold_until(uint32_t instant)
{
	uint32_t count;

	if (k_clock_event_sched_active_target(instant, &count) != 0) {
		while ((int32_t)(k_cycle_get_32() - instant) < 0) {
		}
	} else if (efr32_radio_hold_direct) {
#if defined(CONFIG_EFR32_RADIO_TIMING_PROBE)
		efr32_radio_hold_waited_count =
			(int32_t)(EFR32_HOLD_TIMER->CNT - count) < 0 ? count : 0U;
		efr32_radio_hold_entry = k_cycle_get_32();
#endif
		while ((int32_t)(EFR32_HOLD_TIMER->CNT - count) < 0) {
		}
	} else {
		while ((int32_t)(k_clock_event_sched_active_count() - count) < 0) {
		}
	}
}

/* TIMER0.CNT is the active provider's count when a direct read falls between two of its reads. */
static void efr32_radio_hold_init(void)
{
	unsigned int key = irq_lock();
	uint32_t before = k_clock_event_sched_active_count();
	uint32_t direct = EFR32_HOLD_TIMER->CNT;
	uint32_t after = k_clock_event_sched_active_count();

	irq_unlock(key);
	efr32_radio_hold_direct = (direct - before) <= (after - before);
}/* The longest completion kind: registers the completion event for admission. */
#define EFR32_RADIO_WCET_COMPLETION_MAX_NS \
	MAX(MAX(MAX(CONFIG_EFR32_RADIO_WCET_COMPLETION_TX_RX_NS, \
		    MAX(CONFIG_EFR32_RADIO_WCET_COMPLETION_RX_TX_NS, \
			CONFIG_EFR32_RADIO_WCET_SCAN_COMMIT_NS)), \
		MAX(CONFIG_EFR32_RADIO_WCET_COMPLETION_RX_NS, \
		    CONFIG_EFR32_RADIO_WCET_COMPLETION_TX_NS)), \
	    MAX(CONFIG_EFR32_RADIO_WCET_COMPLETION_ABORT_NS, \
		COND_CODE_1(CONFIG_BT_CONN, \
			    (MAX(MAX(CONFIG_EFR32_RADIO_WCET_CONN_COMPLETION_TX_RX_NS, \
				     CONFIG_EFR32_RADIO_WCET_CONN_COMPLETION_RX_TX_NS + \
				     EFR32_DL_CCM_RX_TX_NS), \
				 CONFIG_EFR32_RADIO_WCET_CONN_COMPLETION_CLOSE_NS)), (0))))
K_DEADLINE_EVENT_DEFINE(efr32_radio_tx_event);
K_DEADLINE_EVENT_DEFINE(efr32_radio_rx_event);
K_DEADLINE_EVENT_DEFINE(efr32_radio_completion_event);

/* The FRC vector publishes this mailbox; only the persistent job calls LLL. */
static struct {
	radio_isr_cb_t callback;
	void *param;
	bool abort;
	/* The kind efr32_radio_completion_window() chose, and whether in a connection event. */
	uint8_t kind;
	bool conn;
} efr32_radio_completion_work;

/* Completion kinds: each has its own measured run and start-by rule. */
enum efr32_radio_completion_kind {
	EFR32_COMPLETION_TX_RX,
	EFR32_COMPLETION_RX_TX,
	EFR32_COMPLETION_RX,
	EFR32_COMPLETION_TX,
	EFR32_COMPLETION_ABORT,
	EFR32_COMPLETION_KINDS,
};

#if defined(CONFIG_EFR32_RADIO_TIMING_PROBE)
/*
 * Longest times seen, scheduler-clock cycles, for a debugger to read (docs/radio-tables.md
 * 7.79 gives the procedure); [conn] is 1 in a connection event. Paths count from the
 * completion job's (or RX launch's) start.
 */
struct efr32_radio_probe {
	uint32_t completion_run[2][EFR32_COMPLETION_KINDS];
	uint32_t to_pending[2];
	uint32_t to_respond[2];
	uint32_t rx_launch_pre;
	uint32_t rx_launch_hold_late;
	uint32_t rx_launch_run;
	uint32_t samples;
	/*
	 * Timed commands (TIMER0 counts): post to the sequencer's first poll, instant to the
	 * command store, and the earliest the sequencer was polling ahead of the instant;
	 * launch job start to the post (scheduler-clock cycles).
	 */
	uint32_t seq_wake;
	uint32_t seq_hold_late;
	uint32_t seq_lead_min;
	uint32_t launch_pre[2];
	uint32_t timed_samples;
	/* Staged connection replies: completion start to the stage posted; wait for the outcome. */
	uint32_t to_staged[2];
	uint32_t conn_outcome_wait;
	uint32_t conn_outcome_after_eof;
	/* RAC.STIMER (from the RX2TX entry) when the sequencer took the stage, ticks. */
	uint32_t conn_taken_at;
};
struct efr32_radio_probe efr32_radio_probe;
static uint32_t efr32_radio_probe_job_start;
#define EFR32_PROBE_MAX(field, value) \
	do { \
		uint32_t v_ = (value); \
		if (v_ > efr32_radio_probe.field) { \
			efr32_radio_probe.field = v_; \
		} \
	} while (0)
#define EFR32_PROBE_PATH(field) \
	EFR32_PROBE_MAX(field[efr32_radio_completion_work.conn ? 1 : 0], \
			k_cycle_get_32() - efr32_radio_probe_job_start)
#else
#define EFR32_PROBE_MAX(field, value) do { } while (0)
#define EFR32_PROBE_PATH(field) do { } while (0)
#endif
static struct efr32_radio_completion_dispatch efr32_radio_completion_dispatch;

#if defined(CONFIG_EFR32_RADIO_QEMU_TIFS)
/*
 * This is a simulator-only replacement for the unproven PROTIMER/RAC TIFS
 * sequencer.  The timer callback only wakes this pre-created job; all RAC
 * commands and timeout handling remain scheduler-owned.
 */
static struct efr32_qemu_tifs_state efr32_qemu_tifs;
/* The LLL records the action after the operation it is now preparing. */
static enum efr32_qemu_tifs_next_action efr32_qemu_tifs_next_action;
static bool efr32_qemu_tifs_transition_requested;
/* RXDIS has claimed a recorded turn-around; wait for the callback to stage TX. */
static bool efr32_qemu_tifs_rx_to_tx_staging;
/* Coalesces an IRQ publication while the persistent TIFS job is running. */
K_DEADLINE_EVENT_DEFINE(efr32_qemu_tifs_event);
K_MUTEX_DEFINE(efr32_qemu_tifs_op_lock);

/* Instant the pending TIFS turnaround holds its RXEN/TXEN to. */
static uint32_t efr32_qemu_tifs_instant;

/*
 * Arm the job @p delay_us from now. A turnaround (RXEN or TXEN at T_IFS) is a timed command held
 * to the active clock's 2 us (Vol 6 Part B 4.2.1) around the seam's T_IFS instant; the
 * no-response timeout serves only the event's next packet, which follows through radio_disable(),
 * an FRC abort and the abort completion.
 */
static void efr32_qemu_tifs_schedule(uint32_t delay_us, uint32_t generation, bool turnaround)
{
	uint32_t now = k_cycle_get_32();
	uint32_t instant = now + EFR32_DL_SPEC_CYC(delay_us);

	if (turnaround) {
		efr32_qemu_tifs_instant = instant;
		cntr_qemu_tifs_arm_at(efr32_dl_hold_not_before(instant,
							       CONFIG_EFR32_RADIO_QEMU_TIFS_PRE_NS),
				      efr32_dl_hold_start_by(instant,
							     CONFIG_EFR32_RADIO_QEMU_TIFS_PRE_NS,
							     BT_ACTIVE_CLOCK_JITTER_US,
							     CONFIG_EFR32_RADIO_QEMU_TIFS_HOLD_LATE_NS),
				      generation, true);
	} else {
		cntr_qemu_tifs_arm_at(instant,
				      efr32_radio_next_start_by(now,
					      CONFIG_EFR32_RADIO_WCET_QEMU_TIFS_NS +
					      /* the FRC interrupt and one interrupt ahead of it */
					      2U * CONFIG_EFR32_RADIO_INTERRUPT_BLOCKING_NS +
					      CONFIG_EFR32_RADIO_WCET_COMPLETION_ABORT_NS),
				      generation, false);
	}
}

static void efr32_qemu_tifs_cancel(void)
{
	/* Serialize cancellation with the worker's final claim and RXEN side effect. */
	k_mutex_lock(&efr32_qemu_tifs_op_lock, K_FOREVER);
	unsigned int key = irq_lock();

	efr32_qemu_tifs_state_cancel(&efr32_qemu_tifs);
	efr32_qemu_tifs_rx_to_tx_staging = false;
	irq_unlock(key);

	/*
	 * Invalidate before clearing the latch: an already-consumed wake is still
	 * stopped by the generation/phase claim, while an unread old wake cannot
	 * become the next arm's work item.
	 */
	k_deadline_event_reset(&efr32_qemu_tifs_event);
	/* A stale BURTC wake is harmless: the worker rechecks phase/generation. */
	cntr_qemu_tifs_cmp_cancel();
	k_mutex_unlock(&efr32_qemu_tifs_op_lock);
}

static void efr32_qemu_tifs_arm(uint32_t hcto_us)
{
	unsigned int key;
	uint32_t generation;
	bool stale;

	/* Invalidate and discard any old wake before publishing a new generation. */
	cntr_qemu_tifs_cmp_cancel();
	key = irq_lock();
	efr32_qemu_tifs_state_cancel(&efr32_qemu_tifs);
	irq_unlock(key);
	k_deadline_event_reset(&efr32_qemu_tifs_event);
	key = irq_lock();
	generation = efr32_qemu_tifs_state_arm(&efr32_qemu_tifs, hcto_us);
	irq_unlock(key);

	efr32_qemu_tifs_schedule(efr32_radio_timer.tifs_us, generation, true);

	/* If reset/teardown raced the start, do not resurrect the cancelled job. */
	key = irq_lock();
	stale = efr32_qemu_tifs.phase != EFR32_QEMU_TIFS_WAIT_RX ||
		efr32_qemu_tifs.generation != generation;
	irq_unlock(key);
	if (stale) {
		cntr_qemu_tifs_cmp_cancel();
	}
}

static void efr32_qemu_tifs_begin_rx_to_tx(void);
static void efr32_qemu_tifs_arm_staged_rx_to_tx(void);
static bool efr32_qemu_tifs_tx_staged(void);
static void efr32_qemu_tifs_start_staged_tx(uint32_t instant);
static void efr32_qemu_tifs_start_rx(uint32_t instant);

static void efr32_qemu_tifs_job(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	for (;;) {
		enum efr32_qemu_tifs_phase phase;
		uint32_t generation;
		uint32_t hcto_us;
		unsigned int key;

		/* Atomically consume a TIFS publication or become its idle consumer. */
		k_deadline_event_wait(&efr32_qemu_tifs_event);
		key = irq_lock();
		phase = efr32_qemu_tifs.phase;
		generation = efr32_qemu_tifs.generation;
		hcto_us = efr32_qemu_tifs.hcto_us;
		irq_unlock(key);

		if (phase == EFR32_QEMU_TIFS_WAIT_RX) {
			/*
			 * Re-check at the final cancellation point before RXEN.  A reset,
			 * disable, or timer stop may have invalidated this dequeued work
			 * while the worker was waiting to run.
			 */
			/* Cancellation cannot interleave between claim and RXEN. */
			k_mutex_lock(&efr32_qemu_tifs_op_lock, K_FOREVER);
			key = irq_lock();
			bool claimed = efr32_qemu_tifs_state_claim_rx(
				&efr32_qemu_tifs, generation);
			if (!claimed) {
				irq_unlock(key);
				k_mutex_unlock(&efr32_qemu_tifs_op_lock);
				continue;
			}

			/* radio_pkt_rx_set() was completed before HCTO was configured. */
			if (radio_is_idle()) {
				efr32_qemu_tifs_start_rx(efr32_qemu_tifs_instant);
			} else {
				efr32_qemu_tifs_state_cancel(&efr32_qemu_tifs);
			}
			irq_unlock(key);
			k_mutex_unlock(&efr32_qemu_tifs_op_lock);

			/* Arm the same persistent job for the bounded no-response path. */
			key = irq_lock();
			if (efr32_qemu_tifs.phase == EFR32_QEMU_TIFS_WAIT_HCTO &&
			    efr32_qemu_tifs.generation == generation) {
				irq_unlock(key);
				efr32_qemu_tifs_schedule(hcto_us, generation, false);
			} else {
				irq_unlock(key);
			}
		} else if (phase == EFR32_QEMU_TIFS_WAIT_TX) {
			/* The LLL callback must have staged a fresh response by TIFS. */
			k_mutex_lock(&efr32_qemu_tifs_op_lock, K_FOREVER);
			key = irq_lock();
			bool claimed = efr32_qemu_tifs_state_claim_tx(
				&efr32_qemu_tifs, generation);
			if (claimed && radio_is_idle() && efr32_qemu_tifs_tx_staged()) {
				efr32_qemu_tifs_start_staged_tx(efr32_qemu_tifs_instant);
			}
			irq_unlock(key);
			k_mutex_unlock(&efr32_qemu_tifs_op_lock);
		} else if (phase == EFR32_QEMU_TIFS_WAIT_HCTO) {
			key = irq_lock();
			if (efr32_qemu_tifs_state_begin_timeout(&efr32_qemu_tifs,
							 generation)) {
				irq_unlock(key);
				/* A completed response owns cleanup; only abort silence. */
				if (!radio_is_done()) {
					radio_disable();
				}
			} else {
				irq_unlock(key);
			}
		}
	}
}

K_THREAD_DEFINE(efr32_qemu_tifs_job_thread,
		CONFIG_EFR32_RADIO_COMPLETION_STACK_SIZE,
		efr32_qemu_tifs_job, NULL, NULL, NULL,
		EFR32_LLL_THREAD_PRIO, 0, 0);
#endif

/* Hardware RX-window generation; see efr32_hw_turnaround_reset(). */
static bool efr32_rac_in_rxsearch(void);
static atomic_t efr32_hw_hcto_generation;

/*
 * Called from the shared BURTC CC0 alarm.  QEMU builds publish work only;
 * hardware builds end a silent RX window here with radio_disable(), which is
 * the interrupt-safe OpenISA abort path.
 */
void efr32_radio_qemu_tifs_wake(uint32_t generation, uint32_t not_before, int deadline)
{
	if (atomic_get(&efr32_radio_halted) != 0) {
		return;
	}
#if defined(CONFIG_EFR32_RADIO_QEMU_TIFS)
	bool valid;
	unsigned int key = irq_lock();

	valid = efr32_qemu_tifs.phase != EFR32_QEMU_TIFS_IDLE &&
		efr32_qemu_tifs.generation == generation;
	irq_unlock(key);
	if (valid) {
		/*
		 * The clock-event expiry is this job's release, armed the wake
		 * latency early; the job waits out its not-before bound and is
		 * checked against its start-by deadline (efr32_qemu_tifs_schedule()).
		 */
		k_deadline_event_post_at(&efr32_qemu_tifs_event, not_before, deadline);
	}
#else
	ARG_UNUSED(not_before);
	ARG_UNUSED(deadline);
	/*
	 * HCTO bounds the search for a frame's header, not a frame already
	 * being received: once the RAC has left RXSEARCH (RXFRAME, or the
	 * RX2TX/TX of a response) the frame's own completion owns cleanup.
	 * Aborting it here cancelled every in-progress SCAN_REQ (SS7.65).
	 */
	if (generation == (uint32_t)atomic_get(&efr32_hw_hcto_generation) &&
	    !radio_is_done() && efr32_rac_in_rxsearch()) {
		radio_disable();
	}
#endif
}

static void efr32_radio_completion_job(void *a, void *b, void *c);

#if defined(CONFIG_EFR32_RADIO_BRINGUP_TRACE)
/* Diagnostic only: RAC.STIMER (~4.8 ticks/us) into spare SEQRAM word n of
 * the M33-side RX->TX timing record, same timebase as the sequencer's. */
#define EFR32_RX2TX_M33_STAMP(n) \
	(((volatile uint32_t *)0xB0004F50UL)[(n)] = \
		*(volatile uint32_t *)0xB8020030UL)
#else
#define EFR32_RX2TX_M33_STAMP(n) do { } while (0)
#endif

#if defined(CONFIG_EFR32_RADIO_QEMU_RX_DESCRIPTOR)
static void efr32_radio_qemu_rx_descriptor_isr(void *arg);
static void efr32_radio_qemu_rx_descriptor_quiesce(void);
static void efr32_radio_qemu_rx_descriptor_setup(void);
static inline bool efr32_qemu_rx_descriptor_pending(void);
/*
 * The drain runs at a meta-IRQ priority, so it is released only through a
 * deadline event: a thread at that priority without its own EDF key
 * would keep the key it had, the zero it was created with, which the
 * scheduler ranks before every real deadline for half the cycle counter's
 * range and after them all for the other half.
 */
K_DEADLINE_EVENT_DEFINE(efr32_qemu_rx_descriptor_event);

/*
 * The drain gates the RX completion: after it the FRC interrupt posts the completion, whose LLL
 * callback may start the event's next packet (efr32_radio_next_start_by()).
 */
static inline void efr32_qemu_rx_descriptor_release(void)
{
	if (atomic_get(&efr32_radio_halted) != 0) {
		return;
	}
	k_deadline_event_post(&efr32_qemu_rx_descriptor_event,
			      efr32_radio_next_start_by(k_cycle_get_32(),
				      CONFIG_EFR32_RADIO_WCET_QEMU_RX_DRAIN_NS +
				      /* the FRC interrupt and one interrupt ahead of it */
				      2U * CONFIG_EFR32_RADIO_INTERRUPT_BLOCKING_NS +
				      CONFIG_EFR32_RADIO_WCET_COMPLETION_RX_NS));
}
static struct {
	uint16_t descriptor_read_offset;
	bool descriptor_crc_valid;
} efr32_qemu_rx_descriptor;
#endif

static void efr32_radio_tx_start(bool hold, uint32_t instant, uint32_t limit);
static void efr32_radio_rx_start(bool hold, uint32_t instant, uint32_t limit);

static void efr32_radio_tx_job(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	for (;;) {
		atomic_val_t generation;

		k_deadline_event_wait(&efr32_radio_tx_event);
#if defined(CONFIG_EFR32_RADIO_TIMING_PROBE)
		efr32_radio_probe_job_start = k_cycle_get_32();
#endif
		generation = atomic_get(&efr32_radio_tx_pending_generation);
		if (atomic_cas(&efr32_radio_launch_active,
			    generation, 0)) {
			/*
			 * TXEN goes to the sequencer with its instant (SS7.82); the register
			 * sequence before it stays atomic, as one radio command.
			 */
			unsigned int key = irq_lock();

			efr32_radio_tx_start(true, (uint32_t)atomic_get(&efr32_radio_tx_instant),
					     (uint32_t)atomic_get(&efr32_radio_tx_limit));
			irq_unlock(key);
#if CONFIG_EFR32_RADIO_TEST_OVERRUN_US > 0
			static bool efr32_test_overrun_done;

			/* Hold the LLL priority while the frame is sent: its completion waits. */
			if (!efr32_test_overrun_done &&
			    k_uptime_get() >= CONFIG_EFR32_RADIO_TEST_OVERRUN_AFTER_MS) {
				efr32_test_overrun_done = true;
				k_busy_wait(CONFIG_EFR32_RADIO_TEST_OVERRUN_US);
			}
#endif
		}
	}
}

static void efr32_radio_rx_job(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	for (;;) {
		atomic_val_t generation;

		k_deadline_event_wait(&efr32_radio_rx_event);
#if defined(CONFIG_EFR32_RADIO_TIMING_PROBE)
		efr32_radio_probe_job_start = k_cycle_get_32();
#endif
		generation = atomic_get(&efr32_radio_rx_pending_generation);
		if (atomic_cas(&efr32_radio_launch_active,
			    generation, 0)) {
			/* A Peripheral's window holds SWRXEN to its instant, like TXEN. */
			bool hold = atomic_get(&efr32_radio_rx_hold) != 0;
			unsigned int key = irq_lock();

			efr32_radio_rx_start(hold, (uint32_t)atomic_get(&efr32_radio_rx_instant),
					     (uint32_t)atomic_get(&efr32_radio_rx_limit));
			irq_unlock(key);
			EFR32_PROBE_MAX(rx_launch_run,
					k_cycle_get_32() - efr32_radio_probe_job_start);
		}
	}
}

K_THREAD_DEFINE(efr32_radio_tx_job_thread,
		CONFIG_EFR32_RADIO_TX_LAUNCH_STACK_SIZE,
		efr32_radio_tx_job, NULL, NULL, NULL,
		EFR32_LLL_THREAD_PRIO, 0, 0);
K_THREAD_DEFINE(efr32_radio_rx_job_thread,
		CONFIG_EFR32_RADIO_RX_LAUNCH_STACK_SIZE,
		efr32_radio_rx_job, NULL, NULL, NULL,
		EFR32_LLL_THREAD_PRIO, 0, 0);
K_THREAD_DEFINE(efr32_radio_completion_job_thread,
		CONFIG_EFR32_RADIO_COMPLETION_STACK_SIZE,
		efr32_radio_completion_job, NULL, NULL, NULL,
		EFR32_LLL_THREAD_PRIO, 0, 0);

/* ISR-safe: publish only a generation and wake the pre-created job. */
void efr32_radio_launch_wake(uint8_t trx, bool hold, uint32_t generation, uint32_t instant,
			     uint32_t limit, int deadline)
{
	if (atomic_get(&efr32_radio_halted) != 0) {
		return;
	}
	if ((uint32_t)atomic_get(&efr32_radio_launch_active) != generation) {
		return;
	}
	/*
	 * A timed launch only posts its command to the sequencer, which waits for the instant
	 * (SS7.82): the job runs as soon as it is released, no not-before wait.
	 */
	if (trx) {
		atomic_set(&efr32_radio_tx_pending_generation, generation);
		atomic_set(&efr32_radio_tx_instant, (atomic_val_t)instant);
		atomic_set(&efr32_radio_tx_limit, (atomic_val_t)limit);
		k_deadline_event_post(&efr32_radio_tx_event, deadline);
	} else if (hold) {
		atomic_set(&efr32_radio_rx_pending_generation, generation);
		atomic_set(&efr32_radio_rx_instant, (atomic_val_t)instant);
		atomic_set(&efr32_radio_rx_limit, (atomic_val_t)limit);
		atomic_set(&efr32_radio_rx_hold, 1);
		k_deadline_event_post(&efr32_radio_rx_event, deadline);
	} else {
		ARG_UNUSED(limit);
		atomic_set(&efr32_radio_rx_pending_generation, generation);
		atomic_set(&efr32_radio_rx_instant, (atomic_val_t)instant);
		atomic_set(&efr32_radio_rx_hold, 0);
		k_deadline_event_post(&efr32_radio_rx_event, deadline);
	}
}

/* Teardown never owns the persistent job threads. It only makes any queued
 * mailbox wake stale and releases the shared BURTC deadline. */
static void efr32_seq_timed_cancel(void);
static void efr32_seq_timed_check(struct k_deadline_event *event, const char *name);

static void efr32_radio_launch_disarm(void)
{
	/* A command the sequencer refused as late is reported before it is forgotten. */
	efr32_seq_timed_check(&efr32_radio_tx_event, "efr32 sequencer timed command");
	efr32_seq_timed_cancel();
	atomic_set(&efr32_radio_launch_active, 0);
	/* A stale latch must not consume a later launch generation early. */
	k_deadline_event_reset(&efr32_radio_tx_event);
	k_deadline_event_reset(&efr32_radio_rx_event);
	cntr_radio_cmp_cancel();
}
/*
 * OpenISA uses sleep/wake as a reference-counted clock-availability
 * contract.  xG22 radio power sequencing has not been characterized, so
 * this intentionally represents immediate software availability only; it is
 * not a claim that RAC idle, an EN bit, or any other MMIO state is powered
 * down.  radio_setup() establishes the initial caller-held reference.
 */
static atomic_t efr32_radio_power_ref = ATOMIC_INIT(1);
static uint8_t MALIGN(4) pkt_empty[PDU_EM_LL_SIZE_MAX];
/*
 * Largest payload length any radio_pkt_configure() caller declares in this configuration
 * (docs/zephyr-ll-port.md, "Packet-target bounds"): legacy advertising and scanning PDUs,
 * PDU_AC_LEG_PAYLOAD_SIZE_MAX (lll_adv.c, lll_scan.c, also with extended advertising); a data
 * PDU, its effective data length -- PDU_DC_PAYLOAD_SIZE_MIN without data length extension --
 * plus the MIC when encrypted (lll_conn.c); DTM, 255 (lll_test.c). radio_pkt_configure()
 * asserts it, so every RX target and TX source below holds a whole configured PDU.
 */
#if defined(CONFIG_BT_CTLR_DTM)
#define EFR32_PKT_PAYLOAD_MAX UINT8_MAX
#elif defined(CONFIG_BT_CTLR_DATA_LENGTH)
#define EFR32_PKT_PAYLOAD_MAX \
	MAX(PDU_AC_LEG_PAYLOAD_SIZE_MAX, \
	    MAX(LL_LENGTH_OCTETS_RX_MAX, LL_LENGTH_OCTETS_TX_MAX) + PDU_MIC_SIZE)
#else
#define EFR32_PKT_PAYLOAD_MAX \
	MAX(PDU_AC_LEG_PAYLOAD_SIZE_MAX, PDU_DC_PAYLOAD_SIZE_MIN + PDU_MIC_SIZE)
#endif
BUILD_ASSERT(EFR32_PKT_PAYLOAD_MAX <= UINT8_MAX, "the LL length field is one octet");
/* Header octet, length octet, payload: the OpenISA PDU layout (pdu.h). */
#define EFR32_PKT_SIZE_MAX (PDU_DC_LL_HEADER_SIZE + EFR32_PKT_PAYLOAD_MAX)
/*
 * Encrypted RX lands here before decryption, CCM TX output is built here, and the advertising,
 * scanning and DTM paths use it through radio_pkt_scratch_get().
 */
static uint8_t MALIGN(4) pkt_scratch[EFR32_PKT_SIZE_MAX];
/*
 * BUFC_BUF_CTRL.SIZE selects a power-of-two FIFO of 64 << SIZE octets: SIZE64 = 0 up to
 * SIZE4096 = 6 (efr32bg21_bufc.h; RAIL's SL_RAIL_MINIMUM_FIFO_BYTES is the same 64). The
 * FIFO is plain system RAM at BUF.ADDR, so it only has to hold the largest frame staged in it.
 */
#define EFR32_BUFC_FIFO_BYTES_MIN 64U
#define EFR32_BUFC_FIFO_BYTES(frame) MAX(EFR32_BUFC_FIFO_BYTES_MIN, NHPOT(frame))
#define EFR32_BUFC_FIFO_CONFIG(bytes) (LOG2(bytes) - LOG2(EFR32_BUFC_FIFO_BYTES_MIN))
/*
 * TX: radio_pkt_tx_set() stages exactly one PDU, at most EFR32_PKT_SIZE_MAX octets (it asserts
 * the configured length), and FRC appends the CRC itself. RAIL's fixed 512 octets are
 * SIZE512 = 3; without data length extension and DTM the largest PDU (39 octets) needs SIZE64.
 */
#if defined(CONFIG_EFR32_RADIO_AMATEUR)
/*
 * The amateur Morse identification is one fixed-length OOK frame (efr32_radio_cw_transmit()):
 * RAIL's 512 octets hold a 12-character call sign at 20 WPM (12 symbols a unit, at most 22 units
 * a character: 396 octets).
 */
#define EFR32_CW_FRAME_OCTETS_MAX 512U
#define EFR32_TX_FRAME_MAX MAX(EFR32_PKT_SIZE_MAX, EFR32_CW_FRAME_OCTETS_MAX)
/* The staged TX is a Morse identification frame. */
static bool efr32_cw_frame;
#if defined(CONFIG_EFR32_RADIO_AMATEUR_ID_DATA)
/* The staged TX is a data-identification PDU (efr32_radio_id_adv_transmit()). */
static bool efr32_id_adv_frame;
#define EFR32_ID_COMPLETION_NS \
	(CONFIG_EFR32_RADIO_WCET_COMPLETION_TX_NS + CONFIG_EFR32_RADIO_WCET_AMATEUR_ID_COMPLETION_NS)
#endif
#else
#define EFR32_TX_FRAME_MAX EFR32_PKT_SIZE_MAX
#endif
static uint8_t MALIGN(4) efr32_tx_fifo[EFR32_BUFC_FIFO_BYTES(EFR32_TX_FRAME_MAX)];
#if defined(CONFIG_EFR32_RADIO_QEMU_RX_DESCRIPTOR)
/*
 * The synthetic descriptor producer is a QEMU-only test seam.  Keep its
 * backing ring in storage owned by this image rather than reusing the stock
 * firmware's observed address, which may overlap unrelated Zephyr RAM.
 */
static uint8_t MALIGN(4) efr32_qemu_rx_descriptor_ring_storage[
	EFR32_RADIO_QEMU_RX_DESCRIPTOR_RING_BYTES];
#endif

/*
 * Packet buffers have two distinct ownership domains on this part:
 *
 * - the controller owns the PDU passed to radio_pkt_{rx,tx}_set(); and
 * - BUFC owns efr32_{rx,tx}_fifo while the peripheral is active.
 *
 * Keep the relationship explicit now, before a later FRC handoff teaches us
 * how to transfer an RX frame out of BUFC.  In particular, RX must not be
 * copied speculatively: the FRC framing and BUFC read-pointer semantics are
 * not yet evidenced.  The stored RX target is the eventual completion target,
 * not a claim that BUFC already writes directly to it.
 */
static struct {
	void *rx_target;
	const void *tx_source;
	uint16_t tx_bytes;
	/*
	 * The OpenISA LLL supplies these immediately before packet setup.  They
	 * describe the on-air PDU, not the number of bytes currently in BUFC:
	 * FRC's FIFO framing is still unknown.  Retain them so an eventual RX
	 * completion path can reject an impossible length before it copies data.
	 */
	uint8_t bits_len;
	uint8_t max_len;
	uint8_t flags;
	bool packet_configured;
	bool rx_fifo_configured;
	bool tx_fifo_configured;
	struct efr32_radio_rx_pdu_delivery rx_delivery;
	struct efr32_radio_packet_lifecycle lifecycle;
} efr32_packet_stage;

/*
 * A future normalized-PDU producer can reject a completed frame before the
 * LLL callback.  This is deliberately separate from FRC.TRAILRXDATA.CRCOK:
 * it represents a software delivery failure, not a second hardware CRC.
 */
static bool efr32_rx_delivery_invalid;
/* CRC result of the frame last handed to the LLL (hardware RX handoff). */
static bool efr32_rx_crc_ok;
#if defined(CONFIG_EFR32_RADIO_RX_CHANNEL_STATS)
/* Bluetooth channel index (0-39) last given to radio_whiten_iv_set(). */
static uint8_t efr32_radio_channel_index;
#define EFR32_BLE_CHANNEL_INDEX_COUNT 40U
/* Diagnostic: [channel][crc_ok] RXDONE counts, read by a debugger. */
volatile uint32_t efr32_radio_rx_channel_stats[EFR32_BLE_CHANNEL_INDEX_COUNT][2];
#endif

static void efr32_packet_stage_reset(void)
{
	memset(&efr32_packet_stage, 0, sizeof(efr32_packet_stage));
	efr32_packet_lifecycle_reset(&efr32_packet_stage.lifecycle);
}

#define BLE_CCM_MIC_SIZE 4U
#define BLE_CCM_NONCE_SIZE 13U
#define BLE_CCM_AAD_MASK 0xE3U

#define EFR32_SYNTH_BASE 0xB8018000UL
#define EFR32_SYNTH_CHCTRL_OFFSET 0x040U
#define EFR32_SYNTH_CHSP_OFFSET 0x044U
#define EFR32_SYNTH_CHSP_CHSP_MASK 0x3FU
#define EFR32_SYNTH_FREQ_OFFSET 0x034U

/*
 * SYNTH.FREQ (public field FREQ, bits 27:0) is the carrier in units of f_HFXO / 2^19: the three
 * words captured at the working stage-2 channel-preparation boundary, 0x01F46AAB (2402 MHz),
 * 0x01F96AAB (2426 MHz) and 0x0204AAAB (2480 MHz), are each round(f * 2^19 / 38.4 MHz)
 * exactly, and stock programs every channel with CHCTRL = 0 (docs/radio-tables.md 7.80).
 */
#define EFR32_SYNTH_FREQ_FRACTION_BITS 19U
#define EFR32_SYNTH_REFERENCE_HZ 38400000ULL
#define EFR32_BLE_CHANNEL_BASE_MHZ 2400U
#define EFR32_KHZ_PER_MHZ 1000U
#define EFR32_HZ_PER_KHZ 1000ULL

#define EFR32_RFCRC_BASE 0xB8010000UL
#define EFR32_RFCRC_EN_OFFSET 0x004U
#define EFR32_RFCRC_CTRL_OFFSET 0x008U
#define EFR32_RFCRC_CMD_OFFSET 0x010U
#define EFR32_RFCRC_INIT_OFFSET 0x018U
#define EFR32_RFCRC_POLY_OFFSET 0x020U
#define EFR32_RFCRC_BLE_CTRL 0x00000708U
#define EFR32_RFCRC_CMD_INITIALIZE BIT(0)

#define EFR32_FRC_BASE 0xB8004000UL
#define EFR32_AGC_BASE 0xB800C000UL
#define EFR32_FRC_WHITEPOLY_OFFSET 0x034U
#define EFR32_FRC_WHITEINIT_OFFSET 0x038U
#define EFR32_FRC_RXCTRL_OFFSET 0x04CU
#define EFR32_FRC_CMD_OFFSET 0x02CU
#define EFR32_FRC_CMD_RXABORT BIT(0)
#define EFR32_FRC_CMD_CONVINIT BIT(9)
#define EFR32_FRC_FECCTRL_OFFSET 0x03CU
#define EFR32_FRC_FECCTRL_BLOCKWHITEMODE_MASK 0x7U
#define EFR32_FRC_BLE_BLOCKWHITEMODE 0x1U
#define EFR32_FRC_TRAILRXDATA_OFFSET 0x054U
#define EFR32_FRC_WCNTCMP0_OFFSET 0x020U
#define EFR32_FRC_WCNTCMP2_OFFSET 0x028U
#define EFR32_FRC_IF_OFFSET 0x068U
#define EFR32_FRC_IEN_OFFSET 0x06CU
#define EFR32_FRC_FCD0_OFFSET 0x104U
#define EFR32_FRC_FCD_STRIDE 0x004U
#define EFR32_FRC_FCD_COUNT 4U
#define EFR32_FRC_FCD_SKIPWHITE BIT(14)
#define EFR32_FRC_DYNAMIC_LENGTH_PENDING_WORD_COUNT 0x00FFU
#define EFR32_FRC_BLE_WHITEPOLY 0x0044U
#define EFR32_FRC_BLE_WHITEINIT_MSB 0x0040U
#define EFR32_FRC_IF_TXDONE (1U << 0)
#define EFR32_FRC_IF_RXDONE (1U << 4)
#define EFR32_FRC_IF_COMPLETION (EFR32_FRC_IF_TXDONE | EFR32_FRC_IF_RXDONE)
#define EFR32_FRC_TRAILRXDATA_CRCOK (1U << 1)
#define EFR32_FRC_RXCTRL_ACCEPTCRCERRORS (1U << 1)
/*
 * The appended octet that TRAILRXDATA.CRCOK selects.  [I] Bit 7 is the CRC
 * result: it was set on every frame while FRC discarded CRC failures, and
 * clear on a frame received with a deliberately corrupted RFCRC.INIT; the
 * low bits vary with the signal (SS7.66).
 */
#define EFR32_FRC_TRAILER_CRCOK_BIT (1U << 7)
#define EFR32_FRC_TRAILER_OCTETS 1U
/*
 * RX: FRC writes whatever frame arrives, before this configuration rejects it: the header
 * octet, a length octet of up to 255 and that payload (FRC.MAXLENGTH is 257 in the PHY
 * tables), then the trailer. What FRC does when a frame overflows the FIFO is unmeasured, so
 * the FIFO holds the longest frame: SIZE512, as RAIL's.
 */
#define EFR32_RX_FRAME_BYTES_MAX (PDU_DC_LL_HEADER_SIZE + UINT8_MAX + EFR32_FRC_TRAILER_OCTETS)
static uint8_t MALIGN(4) efr32_rx_fifo[EFR32_BUFC_FIFO_BYTES(EFR32_RX_FRAME_BYTES_MAX)];

#define EFR32_MODEM_BASE 0xB8014000UL
#define EFR32_MODEM_SYNC0_OFFSET 0x068U

#define EFR32_PROTIMER_BASE 0xB801C000UL

#define EFR32_AGC_FRAMERSSI_OFFSET 0x01CU
#define EFR32_AGC_IF_OFFSET 0x044U
#define EFR32_AGC_CMD_OFFSET 0x04CU
#define EFR32_AGC_CMD_RSSISTART (1U << 0)
#define EFR32_AGC_IF_RSSIVALID (1U << 0)
#define EFR32_AGC_RSSI_CODE_SHIFT 6U
#define EFR32_AGC_RSSI_CODE_MASK 0x3FFU
#define EFR32_AGC_RSSI_INVALID (-512)
#define EFR32_AGC_RSSI_OFFSET_QUARTER_DB 200

#define EFR32_RAC_BASE 0xB8020000UL
#define EFR32_RAC_SR0_OFFSET 0x040U
#define EFR32_RAC_SR1_OFFSET 0x044U
#define EFR32_RAC_SR2_OFFSET 0x048U
/*
 * A live single-step trace of the working stock M33 (RAC_SEQ_IRQn, stage2-
 * rail-starttx.json) clears bit 14 of this register as its very first
 * store, before anything else in its TX-start sequence; this project's
 * own QEMU test suite separately already documents stock clearing bit 30
 * of it via its CLR window (test_rac_sr3_clear_alias). Static decomp of
 * the open sequencer object (docs/radio-tables.md SS7) independently finds
 * both fn_0x894 and fn_0xf9c read this exact register to gate their own
 * behavior. Three independent sources agreeing this register matters.
 *
 * Only bits 14 and 30 are ever cleared by stock, and only at TX-start, via
 * SR3's CLR alias -- NOT a full-register write, and NOT at sequencer
 * install (see radio_reset()'s SR0..SR2 clear and its own comment for why
 * SR3 is excluded there). hardware-lockstep RAC captures confirm SR3 holds
 * a fixed, nonzero value on stock across every checkpoint including the
 * very first one, with bits 14/30 already clear by the time of capture --
 * consistent with being cleared once, early, and never again, not
 * zeroed wholesale. The clear itself lives in radio_tx_enable().
 */
#define EFR32_RAC_SR3_OFFSET 0x04CU
#define EFR32_RAC_SR3_TX_START_CLEAR_MASK (BIT(14) | BIT(30))
#define EFR32_RAC_SR0_INIT_CLEAR (1U << 6)
#define EFR32_RAC_SR0_TX_PREPARE_SET BIT(7)
#define EFR32_RAC_SR0_TX_PREPARE_CLEAR BIT(3)
/* SR0 bit 1: hardware-confirmed (stage-1/Arduino BGAPI lockstep) that
 * stock's RAIL_StartTx clears this via SR0's CLR alias for a plain
 * advertisement, before anything below. Tried porting it as the very
 * first store in this function and tested on real hardware: regression,
 * not a fix -- RAC.STATUS never reached/held TX afterward, reverted. See
 * docs/radio-tables.md SS7.11 before retrying this. */
#define EFR32_RAC_RXENSRCEN_OFFSET 0x008U
#define EFR32_RAC_STATUS_OFFSET 0x00CU
#define EFR32_RAC_STIMER_OFFSET 0x030U
#define EFR32_RAC_STIMER_MASK 0xFFFFU
#define EFR32_RAC_CMD_OFFSET 0x010U
#define EFR32_RAC_CTRL_OFFSET 0x014U
#define EFR32_RAC_SEQIF_OFFSET 0x028U
#define EFR32_RAC_SEQCTRL_OFFSET 0x038U
#define EFR32_RAC_PAENCTRL_OFFSET 0x0B4U
#define EFR32_RAC_PATRIM1_OFFSET 0x114U
#define EFR32_RAC_PATRIM3_OFFSET 0x11CU
#define EFR32_RAC_PACTRL_OFFSET 0x120U
#define EFR32_REGISTER_SET_OFFSET 0x1000U
#define EFR32_REGISTER_CLEAR_OFFSET 0x2000U
#define EFR32_RAC_STATUS_STATE_SHIFT 24U
#define EFR32_RAC_STATUS_STATE_MASK 0xFU
#define EFR32_RAC_STATE_OFF 0U
#define EFR32_RAC_STATE_SHUTDOWN 13U
#define EFR32_RAC_CMD_TXEN (1U << 0)
#define EFR32_RAC_CMD_TXDIS (1U << 5)
#define EFR32_RAC_CMD_RXDIS (1U << 8)
#define EFR32_RAC_RXENSRCEN_SWRXEN (1U << 1)
#define EFR32_RAC_CTRL_FORCEDISABLE (1U << 0)
#define EFR32_RAC_CTRL_SEQRESET BIT(24)
#define EFR32_RAC_CTRL_ACTIVE_HIGH BIT(7)
#define EFR32_RAC_CTRL_PA_ENABLE_HIGH BIT(8)
#define EFR32_RAC_CTRL_LNA_ENABLE_HIGH BIT(9)
#define EFR32_RAC_CTRL_CPUWAITDIS BIT(26)
#define EFR32_RAC_SEQIF_STIMCMPEVSEQ BIT(1)
/* Observed initial sequencer-control mode; its two set fields remain RE debt. */
#define EFR32_RAC_SEQCTRL_STAGE2_STARTUP 0x00000011U

#define EFR32_SEQ_RAM_BASE 0xB0000000UL
#define EFR32_SEQ_RAM_SIZE 0x00008000U
#define EFR32_SEQ_STACK_TOP 0x00003E00U
#define EFR32_SEQ_IDENTITY_ADDRESS 0xB0004FE0UL
/* The RX->TX decision word shared with the sequencer (see efr32_hw_rx2tx_armed). */
#define EFR32_SEQ_RX2TX_DECISION_ADDRESS 0xB0004FA4UL
#define EFR32_SEQ_RX2TX_IDLE 0U
#define EFR32_SEQ_RX2TX_PENDING 3U
#define EFR32_SEQ_RX2TX_RESPOND 1U
#define EFR32_SEQ_RX2TX_CANCEL 2U
#define EFR32_SEQ_IDENTITY_OPEN_V1 0x4F535131U
#define EFR32_SEQ_TRACE_TXWARM_ADDRESS 0xB0004FC0UL
#define EFR32_SEQ_TRACE_TX_ADDRESS 0xB0004FD0UL

extern const uint8_t efr32_radio_seq_image[];
extern const size_t efr32_radio_seq_image_size;
extern const bool efr32_radio_seq_image_is_open;

/*
 * The final proprietary TX primitive clears this sequencer flag immediately
 * before RAC.CMD.TXEN.  The public register model does not name the shared-RAM
 * word, so keep the address and bit isolated behind a provenance-based name
 * until the open sequencer ABI gives it a semantic field name.
 */
#define EFR32_SEQ_TX_PREPARE_FLAGS_ADDRESS 0xB0003EC4UL
#define EFR32_SEQ_RX_AFTER_TX_INHIBIT BIT(5)

/*
 * A live single-step trace of the working stock M33's own RAC_SEQ IRQ
 * handler (tests/hardware-lockstep/stage2-rail-starttx.json, step 1081)
 * writes this exact value to this exact shared-RAM word before RAC.CMD.TXEN.
 * Static decomp of the open sequencer object independently identified this
 * same address as one the shared RAC-service dispatcher (docs/radio-tables.md
 * SS7.3) dereferences but this image never initializes -- two analyses from
 * opposite directions landing on the same word is why this is named and
 * written here rather than left alone, but its field meaning is NOT
 * established and this write is UNTESTED on hardware (SS7.5-7.6): remove it
 * if testing shows no effect, do not treat its presence as a confirmed fix.
 */
#define EFR32_SEQ_UNNAMED_FLAG_3EEC_ADDRESS 0xB0003EECUL
#define EFR32_SEQ_UNNAMED_FLAG_3EEC_OBSERVED_VALUE 0x00000012UL

static inline volatile uint32_t *radio_reg32(uintptr_t address);

#if defined(CONFIG_EFR32_RADIO_BRINGUP_TRACE)
static void efr32_radio_trace_previous_seq_launch(void)
{
	static bool reported;
	volatile uint32_t *txwarm = radio_reg32(EFR32_SEQ_TRACE_TXWARM_ADDRESS);
	volatile uint32_t *tx = radio_reg32(EFR32_SEQ_TRACE_TX_ADDRESS);

	/* These words are written before the M0+ acknowledges each state event.
	 * The first M33 launch sees the reset zeros; the next scheduled launch is
	 * the first safe point at which to report the completed prior handoff.
	 */
	if (!reported && (txwarm[0] || txwarm[1] || txwarm[2] || txwarm[3] ||
			 tx[0] || tx[1] || tx[2] || tx[3])) {
		reported = true;
		printk("efr32-radio trace: seq-prev txwarm=%08x/%08x/%u/%u "
		       "tx=%08x/%08x/%u/%u\n",
		       txwarm[0], txwarm[1], txwarm[2], txwarm[3],
		       tx[0], tx[1], tx[2], tx[3]);
	}
}
#else
static inline void efr32_radio_trace_previous_seq_launch(void) { }
#endif

/*
 * Temporary board-specific PA characterization from the working stage-2
 * controller at the IRQ8/TX preparation boundary.  These names deliberately
 * describe provenance, not a guessed dBm mapping.  Replace this fixed preset
 * with DEVINFO/board calibration and public PA field definitions before the
 * driver supports selectable transmit power.
 */
#define EFR32_RAC_STAGE2_PAENCTRL 0x10000000U
#define EFR32_RAC_STAGE2_PATRIM1 0x00038997U
#define EFR32_RAC_STAGE2_PATRIM3 0x00322D2AU
#define EFR32_RAC_STAGE2_PACTRL 0x00000010U

#define EFR32_BUFC_BASE 0xBA000000UL
#define EFR32_BUFC_TX_BASE_OFFSET 0x00CU
#define EFR32_BUFC_RX_BASE_OFFSET 0x04CU
#define EFR32_BUFC_RX_DESCRIPTOR_BASE_OFFSET 0x08CU
#define EFR32_BUFC_CFG_OFFSET 0x000U
#define EFR32_BUFC_ADDR_OFFSET 0x004U
#define EFR32_BUFC_WRITEOFFSET 0x008U
#define EFR32_BUFC_READOFFSET_OFFSET 0x00CU
#define EFR32_BUFC_WRITEDATA_OFFSET 0x018U
#define EFR32_BUFC_THRESHOLD_OFFSET 0x024U
#define EFR32_BUFC_CMD_OFFSET 0x028U
#define EFR32_BUFC_IF_OFFSET 0x114U
#define EFR32_BUFC_IEN_OFFSET 0x118U
#define EFR32_BUFC_CMD_CLEAR 1U
/* [regs-public] BUFC_BUF_CMD bit 1 (efr32bg21_bufc.h). Deliberately
 * unused: it belongs to RAIL_SetTxFifo's bulk/zero-copy WRITEOFFSET-
 * assignment path, not the per-byte WRITEDATA fill this driver uses
 * (matching stock's own observed per-packet pattern). Tested anyway as
 * a hardware hypothesis and understood why it's a no-op here, not just
 * guessed to be irrelevant -- see docs/radio-tables.md §7.17. */
#define EFR32_BUFC_CMD_PREFETCH 2U
#define EFR32_BUFC_RX_THRESHOLD 0x0FFFU
#define EFR32_BUFC_DESCRIPTOR_THRESHOLD 7U
#define EFR32_BUFC_DESCRIPTOR_PENDING BIT(18)

#define EFR32_RADIO_EN_OFFSET 0x004U

static inline volatile uint32_t *radio_reg32(uintptr_t address)
{
	return (volatile uint32_t *)address;
}

/*
 * Timed commands on the sequencer (hal/efr32/seq_abi.h, docs/radio-tables.md SS7.82). The M33
 * reaches the radio window, BUFC and SEQRAM through the 0xBxxx_xxxx aliases of the M0+'s
 * 0xAxxx_xxxx and 0x0000_xxxx addresses (SS7.74).
 */
#define EFR32_SEQ_M33_RADIO_ALIAS 0x10000000UL
#define EFR32_SEQ_TIMED_WORD(offset) \
	radio_reg32(EFR32_SEQ_RAM_BASE + EFR32_SEQ_TIMED_BLOCK + (offset))
#define EFR32_SEQ_MAILBOX_FLAGS_SET \
	(EFR32_SEQ_RDMAILBOX1_BASE + EFR32_SEQ_M33_RADIO_ALIAS + EFR32_REGISTER_SET_OFFSET + \
	 EFR32_SEQ_RDMAILBOX_FLAGS)

/* The command last posted (generation in its upper half), its TIMER0 instant and post count. */
static uint32_t efr32_seq_timed_op;
static uint32_t efr32_seq_timed_target;
static uint32_t efr32_seq_timed_posted;
static uint32_t efr32_seq_timed_release;
static int efr32_seq_timed_deadline;
static uint16_t efr32_seq_timed_generation;

/*
 * Post @p op, due at scheduler-clock @p instant and to be issued no later than @p limit, to the
 * sequencer. Both are mapped onto the active provider's count (TIMER0, checked by
 * efr32_radio_hold_init()), rounding up, as the M33 hold did. Returns false when the service
 * is not on that provider: the caller then holds the command itself.
 */
static bool efr32_seq_timed_post(uint32_t op, uint32_t instant, uint32_t limit)
{
	uint32_t target;
	uint32_t latest;

	if (!efr32_radio_hold_direct || k_clock_event_sched_active_target(instant, &target) != 0 ||
	    k_clock_event_sched_active_target(limit, &latest) != 0) {
		return false;
	}
	efr32_seq_timed_generation++;
	efr32_seq_timed_op = op | ((uint32_t)efr32_seq_timed_generation <<
				   EFR32_SEQ_TIMED_GENERATION_SHIFT);
	efr32_seq_timed_target = target;
	*EFR32_SEQ_TIMED_WORD(EFR32_SEQ_TIMED_TARGET) = target;
	*EFR32_SEQ_TIMED_WORD(EFR32_SEQ_TIMED_LIMIT) = latest;
	*EFR32_SEQ_TIMED_WORD(EFR32_SEQ_TIMED_DONE) = 0U;
	__DSB();
	*EFR32_SEQ_TIMED_WORD(EFR32_SEQ_TIMED_OP) = efr32_seq_timed_op;
	__DSB();
	*radio_reg32(EFR32_SEQ_MAILBOX_FLAGS_SET) = EFR32_SEQ_RDMAILBOX_TIMED_FLAG;
	efr32_seq_timed_posted = EFR32_HOLD_TIMER->CNT;
	return true;
}

/* Withdraw a posted command the sequencer has not issued yet. */
static void efr32_seq_timed_cancel(void)
{
	if (efr32_seq_timed_op != 0U) {
		*EFR32_SEQ_TIMED_WORD(EFR32_SEQ_TIMED_OP) = EFR32_SEQ_TIMED_OP_NONE;
		efr32_seq_timed_op = 0U;
	}
}

#if defined(CONFIG_EFR32_RADIO_TIMING_PROBE)
static void efr32_seq_timed_probe(void);
#endif

/*
 * Once the radio has completed after a timed command: a command the sequencer refused as past
 * its limit missed the instant it served, which goes to the missed-deadline policy as a late
 * start (the radio then stops: the on-air timing the command served cannot be met).
 */
static void efr32_seq_timed_check(struct k_deadline_event *event, const char *name)
{
	uint32_t done;

	if (efr32_seq_timed_op == 0U) {
		return;
	}
	done = *EFR32_SEQ_TIMED_WORD(EFR32_SEQ_TIMED_DONE);
#if defined(CONFIG_EFR32_RADIO_TIMING_PROBE)
	if (done == efr32_seq_timed_op) {
		efr32_seq_timed_probe();
	}
#endif
	efr32_seq_timed_op = 0U;
	if ((done & EFR32_SEQ_TIMED_LATE) != 0U) {
		struct k_deadline_miss miss = {
			.kind = K_DEADLINE_MISS_START,
			.object = event,
			.name = name,
			.release = efr32_seq_timed_release,
			.deadline = efr32_seq_timed_deadline,
			.now = k_cycle_get_32(),
		};

		z_deadline_miss_report(&miss);
	}
}

#if defined(CONFIG_EFR32_RADIO_TIMING_PROBE)
static void efr32_seq_timed_probe(void)
{
	uint32_t entry = *EFR32_SEQ_TIMED_WORD(EFR32_SEQ_TIMED_ENTRY);
	uint32_t at = *EFR32_SEQ_TIMED_WORD(EFR32_SEQ_TIMED_AT);
	uint32_t lead = efr32_seq_timed_target - entry;

	efr32_radio_probe.timed_samples++;
	EFR32_PROBE_MAX(seq_wake, entry - efr32_seq_timed_posted);
	EFR32_PROBE_MAX(seq_hold_late, at - efr32_seq_timed_target);
	if (efr32_radio_probe.seq_lead_min == 0U || (int32_t)lead < 0 ||
	    lead < efr32_radio_probe.seq_lead_min) {
		efr32_radio_probe.seq_lead_min = lead;
	}
}
#endif

#define EFR32_RAC_STATE_RXSEARCH_VALUE 2U

static bool efr32_rac_in_rxsearch(void)
{
	return ((*radio_reg32(EFR32_RAC_BASE + EFR32_RAC_STATUS_OFFSET) >>
		 EFR32_RAC_STATUS_STATE_SHIFT) & EFR32_RAC_STATUS_STATE_MASK) ==
	       EFR32_RAC_STATE_RXSEARCH_VALUE;
}

static void efr32_radio_sequencer_install(void)
{
	volatile uint8_t *destination = (volatile uint8_t *)EFR32_SEQ_RAM_BASE;
	static bool installed;
	uint32_t initial_sp;
	uint32_t reset_vector;

	/* radio_reset() runs in every LLL prepare. Stock RAIL loads its image
	 * once at initialization; re-copying, re-verifying and rebooting the
	 * private core per event cost time proportional to the image size ahead
	 * of every launch. The core keeps running between events, and its reset
	 * handler holds no per-event state. */
	if (installed) {
		return;
	}
	installed = true;

	LL_ASSERT(efr32_radio_seq_image_size >= (2U * sizeof(uint32_t)));
	LL_ASSERT(efr32_radio_seq_image_size <= EFR32_SEQ_RAM_SIZE);
	memcpy(&initial_sp, &efr32_radio_seq_image[0], sizeof(initial_sp));
	memcpy(&reset_vector, &efr32_radio_seq_image[4], sizeof(reset_vector));
	if (efr32_radio_seq_image_is_open) {
		LL_ASSERT(initial_sp == EFR32_SEQ_STACK_TOP);
	} else {
		/* Stage 2.5 accepts the captured vendor image's own aligned stack
		 * placement, but still rejects a malformed vector table. */
		LL_ASSERT(initial_sp != 0U && initial_sp <= 0x4000U &&
			  (initial_sp & 3U) == 0U);
	}
	LL_ASSERT((reset_vector & 1U) != 0U);
	LL_ASSERT((reset_vector & ~1U) < efr32_radio_seq_image_size);

	/* SEQRAM retains across an M33 reset.  Hold the private core in reset for
	 * the entire copy so an old image can never execute concurrently.  The
	 * base-register SEQRESET strobe is the sequence proven by the standalone
	 * silicon launcher; unlike ordinary CTRL fields, aliasing this strobe has
	 * not been established on hardware. */
	*radio_reg32(EFR32_RAC_BASE + EFR32_RAC_CTRL_OFFSET) =
		EFR32_RAC_CTRL_ACTIVE_HIGH | EFR32_RAC_CTRL_PA_ENABLE_HIGH |
		EFR32_RAC_CTRL_LNA_ENABLE_HIGH | EFR32_RAC_CTRL_SEQRESET;
	__DSB();
	for (size_t index = 0U; index < efr32_radio_seq_image_size; index++) {
		destination[index] = efr32_radio_seq_image[index];
	}
	__DSB();
	for (size_t index = 0U; index < efr32_radio_seq_image_size; index++) {
		LL_ASSERT(destination[index] == efr32_radio_seq_image[index]);
	}
	/* A retained marker cannot satisfy the open image's post-release startup
	 * handshake. The diagnostic vendor image owns this shared word and does
	 * not implement our OSQ1 ABI. */
	if (efr32_radio_seq_image_is_open) {
		*radio_reg32(EFR32_SEQ_IDENTITY_ADDRESS) = 0U;
		/*
		 * The decision word retains too: a PENDING left by the previous image made the
		 * sequencer request TX at every received frame of a window never armed for a
		 * response (an RX2TX hold and a decision timeout per frame, measured on
		 * silicon, docs/radio-tables.md 7.80).
		 */
		*radio_reg32(EFR32_SEQ_RX2TX_DECISION_ADDRESS) = EFR32_SEQ_RX2TX_IDLE;
	}
	__DSB();
}

static void efr32_radio_sequencer_wait_ready(void)
{
	if (!efr32_radio_seq_image_is_open) {
		/* Stage 2.5 observes the vendor image through the normal radio state
		 * boundaries; it cannot claim the open firmware's private ABI. */
		return;
	}

	/* The open reset handler publishes identity before enabling interrupts.
	 * This is a bounded core-start handshake, not radio-job scheduling. */
	for (uint32_t elapsed_us = 0U; elapsed_us < 10000U; elapsed_us++) {
		if (*radio_reg32(EFR32_SEQ_IDENTITY_ADDRESS) ==
		    EFR32_SEQ_IDENTITY_OPEN_V1) {
			return;
		}
		k_busy_wait(1U);
	}

	LL_ASSERT_MSG(false, "open radio sequencer did not publish identity");
}

/*
 * The OpenISA LLL programs a packet format and a direction-specific packet
 * target before it asks the HAL to arm RAC. Keep that ordering as a checked
 * HAL contract. A completion IRQ without a current RX target cannot be
 * treated as packet delivery, and TXEN must not consume a stale BUFC FIFO.
 *
 * This validates only controller-owned metadata. It does not imply that FRC
 * has consumed or produced bytes in either BUFC FIFO; that relationship still
 * needs evidence.
 */
static void efr32_packet_stage_arm_keep(enum efr32_radio_packet_direction direction,
					bool clear_latches);

static void efr32_packet_stage_arm(enum efr32_radio_packet_direction direction)
{
	efr32_packet_stage_arm_keep(direction, true);
}

/*
 * With @p clear_latches false a completion already latched is kept: a staged connection
 * receive can complete before the LLL labels it (its frame follows the sequencer's reply).
 */
static void efr32_packet_stage_arm_keep(enum efr32_radio_packet_direction direction,
					bool clear_latches)
{
	LL_ASSERT(efr32_packet_stage.packet_configured);
	LL_ASSERT(efr32_packet_stage.lifecycle.active_direction == EFR32_RADIO_PACKET_IDLE);

	if (direction == EFR32_RADIO_PACKET_RX) {
		LL_ASSERT(efr32_packet_stage.rx_target != NULL);
		LL_ASSERT(efr32_packet_stage.rx_fifo_configured);
	#if defined(CONFIG_EFR32_RADIO_QEMU_RX_DESCRIPTOR)
		efr32_qemu_rx_descriptor.descriptor_crc_valid = false;
	#endif
	} else {
		LL_ASSERT(direction == EFR32_RADIO_PACKET_TX);
		LL_ASSERT(efr32_packet_stage.tx_source != NULL);
		LL_ASSERT(efr32_packet_stage.tx_fifo_configured);
		LL_ASSERT(efr32_packet_stage.tx_bytes >= 2U);
#if defined(CONFIG_EFR32_RADIO_AMATEUR)
		/* A Morse identification frame is not a link-layer PDU (efr32_radio_cw_transmit()). */
		LL_ASSERT(efr32_cw_frame || efr32_packet_stage.tx_bytes <=
			  (uint32_t)efr32_packet_stage.max_len + 2U);
#else
		LL_ASSERT(efr32_packet_stage.tx_bytes <=
			  (uint32_t)efr32_packet_stage.max_len + 2U);
#endif
	}

	/* The LLL reads completion state from its ISR callback, so clear only
	 * before the operation is armed—not from the ISR itself. */
	if (clear_latches) {
		*radio_reg32(EFR32_FRC_BASE + EFR32_REGISTER_CLEAR_OFFSET +
			     EFR32_FRC_IF_OFFSET) = EFR32_FRC_IF_COMPLETION;
	}
	efr32_rx_delivery_invalid = false;
	efr32_packet_lifecycle_arm(&efr32_packet_stage.lifecycle, direction);
}

static void efr32_tx_fifo_publish(void)
{
	uintptr_t base = EFR32_BUFC_BASE + EFR32_BUFC_TX_BASE_OFFSET;

	LL_ASSERT(efr32_packet_stage.tx_fifo_configured);
	LL_ASSERT(efr32_packet_stage.tx_bytes <= sizeof(efr32_tx_fifo));
	*radio_reg32(base + EFR32_BUFC_CMD_OFFSET) = EFR32_BUFC_CMD_CLEAR;
	*radio_reg32(base + EFR32_BUFC_CFG_OFFSET) = EFR32_BUFC_FIFO_CONFIG(sizeof(efr32_tx_fifo));
	*radio_reg32(base + EFR32_BUFC_ADDR_OFFSET) = (uint32_t)(uintptr_t)efr32_tx_fifo;
	for (uint32_t index = 0U; index < efr32_packet_stage.tx_bytes; index++) {
		*radio_reg32(base + EFR32_BUFC_WRITEDATA_OFFSET) = efr32_tx_fifo[index];
	}
}

#if defined(CONFIG_EFR32_RADIO_QEMU_TIFS)
static void efr32_qemu_tifs_arm_tx(void)
{
	unsigned int key;
	uint32_t generation;
	bool stale;

	/* The completion job stopped RX; replace any older scheduler generation. */
	cntr_qemu_tifs_cmp_cancel();
	key = irq_lock();
	efr32_qemu_tifs_state_cancel(&efr32_qemu_tifs);
	irq_unlock(key);
	k_deadline_event_reset(&efr32_qemu_tifs_event);
	key = irq_lock();
	generation = efr32_qemu_tifs_state_arm_tx(&efr32_qemu_tifs);
	irq_unlock(key);
	efr32_qemu_tifs_schedule(efr32_radio_timer.tifs_us, generation, true);

	key = irq_lock();
	stale = efr32_qemu_tifs.phase != EFR32_QEMU_TIFS_WAIT_TX ||
		efr32_qemu_tifs.generation != generation;
	irq_unlock(key);
	if (stale) {
		cntr_qemu_tifs_cmp_cancel();
	}
}

/*
 * This deliberately bypasses radio_tx_enable(): that public helper retains
 * OpenISA's legacy busy-wait contract.  The QEMU turn-around worker instead
 * has a final, cancellable idle check and must never spin from a callback or
 * interrupt context.
 */
static bool efr32_qemu_tifs_tx_staged(void)
{
	return efr32_packet_stage.lifecycle.active_direction ==
		EFR32_RADIO_PACKET_IDLE &&
		efr32_packet_stage.tx_source != NULL &&
		efr32_packet_stage.tx_fifo_configured &&
		efr32_packet_stage.tx_bytes >= 2U;
}

/* RXEN held to @p instant (interrupts locked by the caller); the RAC is idle. */
static void efr32_qemu_tifs_start_rx(uint32_t instant)
{
	efr32_packet_stage_arm(EFR32_RADIO_PACKET_RX);
	efr32_radio_hold_until(instant);
	*radio_reg32(EFR32_RAC_BASE + EFR32_REGISTER_SET_OFFSET +
		     EFR32_RAC_RXENSRCEN_OFFSET) = EFR32_RAC_RXENSRCEN_SWRXEN;
}

static void efr32_qemu_tifs_start_staged_tx(uint32_t instant)
{
	efr32_tx_fifo_publish();
	efr32_packet_stage_arm(EFR32_RADIO_PACKET_TX);
	efr32_radio_hold_until(instant);
	*radio_reg32(EFR32_RAC_BASE + EFR32_RAC_CMD_OFFSET) = EFR32_RAC_CMD_TXEN;
}

/*
 * Called by the scheduler-owned FRC completion job before it invokes the LLL
 * RX callback. The RV32M1 reference has already programmed its next command
 * by this point; QEMU reserves this turn-around and stops RX now, but does
 * not start its TIFS timer until the callback has staged the response PDU.
 */
static void efr32_qemu_tifs_begin_rx_to_tx(void)
{
	bool claimed;
	unsigned int key;

	k_mutex_lock(&efr32_qemu_tifs_op_lock, K_FOREVER);
	key = irq_lock();
	claimed = efr32_qemu_tifs_next_action_claim(
		&efr32_qemu_tifs_next_action, EFR32_QEMU_TIFS_NEXT_TX);
	/* A received PDU wins over the no-response deadline. */
	if (claimed) {
		efr32_qemu_tifs_state_cancel(&efr32_qemu_tifs);
	}
	irq_unlock(key);
	if (claimed) {
		cntr_qemu_tifs_cmp_cancel();
		/*
		 * Do not call radio_disable(): RXDIS here is not an LLL abort.  Like
		 * radio_disable(), withdraw SWRXEN first: with the source still
		 * asserted RXDIS does not stop receive on silicon (SS7.63), and a
		 * source left asserted turns the following TX around into RX.
		 */
		*radio_reg32(EFR32_RAC_BASE + EFR32_REGISTER_CLEAR_OFFSET +
			     EFR32_RAC_RXENSRCEN_OFFSET) = EFR32_RAC_RXENSRCEN_SWRXEN;
		*radio_reg32(EFR32_RAC_BASE + EFR32_RAC_CMD_OFFSET) = EFR32_RAC_CMD_RXDIS;
		key = irq_lock();
		efr32_qemu_tifs_rx_to_tx_staging = true;
		irq_unlock(key);
	}
	k_mutex_unlock(&efr32_qemu_tifs_op_lock);
}

/*
 * Run after the LLL completion callback. A callback may have rejected the
 * packet or torn the event down, so re-check both the reservation and the
 * staged PDU while holding the same lock used by teardown before arming TIFS.
 */
static void efr32_qemu_tifs_arm_staged_rx_to_tx(void)
{
	bool staged;
	unsigned int key;

	k_mutex_lock(&efr32_qemu_tifs_op_lock, K_FOREVER);
	key = irq_lock();
	staged = efr32_qemu_tifs_rx_to_tx_staging;
	efr32_qemu_tifs_rx_to_tx_staging = false;
	irq_unlock(key);
	if (staged && radio_is_idle() && efr32_qemu_tifs_tx_staged()) {
		efr32_qemu_tifs_arm_tx();
	}
	k_mutex_unlock(&efr32_qemu_tifs_op_lock);
}
#endif

/*
 * The public RAIL configurator uses this compact, address-independent format
 * for radio tables. Do not put SEQ.PHYINFO records here: their data words are
 * pointers into the vendor image and are not portable to this firmware.
 */
#define EFR32_RADIO_TABLE_END 0xFFFFFFFFU
#define EFR32_RADIO_TABLE_OP_WRITE 0U
#define EFR32_RADIO_TABLE_OP_AND 1U
#define EFR32_RADIO_TABLE_OP_OR 3U

#define EFR32_SEQ_PHYINFO_ADDRESS 0xB0004FFCUL

/*
 * The generated xG22 BLE configuration does not merely write MODEM/FRC
 * registers: it publishes this relocatable PHY description to the radio
 * sequencer.  The old importer dropped the record because the generated
 * pointer belonged to a different ELF.  Keep the pointed-to objects in this
 * image and relocate their pointers normally instead.
 *
 * The member meanings beyond the three named configuration pointers are not
 * public, so retain the generator's word-oriented representation and names
 * instead of inventing field semantics.  This is the exact Zlib-licensed
 * `phyInfo_0` data for xG22 BLE 1M; a live stage-2 capture at TXEN matched all
 * non-pointer words and its three pointed-to objects.
 */
static const uint8_t MALIGN(4) efr32_ble_ir_cal_config[] = {
	25, 63, 1, 6, 4, 16, 1, 0, 0, 1, 1, 6, 0, 16, 39, 0, 0, 12, 0, 0, 0, 0, 0, 0, 0, 0,
};

static const int32_t efr32_ble_1m_timing_config[] = {
	50000, 8700, 435, 435,
};

static const uint8_t MALIGN(4) efr32_ble_hfxo_retiming_config[] = {
	2, 0, 0, 0, 0x00, 0xf0, 0x49, 0x02, 6, 20, 0, 0,
	0x00, 0xe0, 0x93, 0x04, 5, 56, 0, 0, 0xa0, 0x08, 0, 0,
	0, 0, 0x58, 0x09, 1, 4, 7, 6, 0x10, 0x0a, 1, 4,
	7, 7, 0xc8, 0x0a, 0, 4, 8, 7, 0x80, 0x0b, 0, 4,
	8, 8, 0x38, 0x0c, 0, 4, 9, 8, 0x61, 0x08, 0, 0,
	0, 0, 0x68, 0x08, 0, 0, 0, 0, 0, 0xc7, 0x09, 1, 4,
	4, 3, 0x2c, 0x0b, 1, 4, 4, 4, 0x92, 0x0c, 1, 4,
	5, 4,
};

static const uintptr_t efr32_ble_1m_phy_info[] = {
	20U,
	0x00800000U,
	0U,
	(uintptr_t)efr32_ble_ir_cal_config,
	(uintptr_t)efr32_ble_1m_timing_config,
	0x00000000U,
	4396932U,
	16000000U,
	1000000U,
	0x00F50101U,
	0x03502555U,
	0U,
	(uintptr_t)efr32_ble_hfxo_retiming_config,
	0U, 0U, 0U,
	999992U,
	0U, 0U, 0U, 0U,
};

BUILD_ASSERT(sizeof(uintptr_t) == sizeof(uint32_t));
BUILD_ASSERT(ARRAY_SIZE(efr32_ble_1m_phy_info) == 21U);

static void efr32_ble_phy_info_publish(const uintptr_t *phy_info)
{
	*radio_reg32(EFR32_SEQ_PHYINFO_ADDRESS) = (uint32_t)(uintptr_t)phy_info;
}

/*
 * Public xG22 BLE viterbi 1M configuration, copied as table operations rather
 * than as a vendor-addressed PHYINFO record. Source: the Zlib-licensed
 * ble_efr32xg22_configurator_out.c arrays at lines 225-319 and 474-534 in
 * the sibling Simplicity SDK checkout. radio_aa_set(), radio_crc_configure(),
 * and radio_whiten_iv_set() deliberately replace its dynamic defaults later.
 */
static const uint32_t efr32_ble_1m_base_table[] = {
	/*
	 * Every FRC value below is decoded against the public field model
	 * (build/reference-models/FRC_S_field.py) rather than left as a bare
	 * cited register name -- each named field's bit offset/width comes
	 * directly from that generated model, not re-derived by hand.
	 */
	0x0001400CU, 0x00018101U, /* FRC.DFLCTRL: DFLMODE=1, DFLOFFSET=1,
				    * DFLBITS=8, MINLENGTH=1 (DFLBITORDER,
				    * DFLSHIFT, DFLINCLUDECRC, DFLBOIOFFSET=0) */
	0x00024020U, 0x00000000U, 0x00000001U, /* FRC.WCNTCMP0.FRAMELENGTH=0,
						  * FRC.WCNTCMP1.LENGTHFIELDLOC=1 */
	0x00034030U, 0x00000000U, 0x00000000U, 0x00000000U, /* FRC.WHITE*: all zero */
	0x00024040U, 0x00000000U, 0x00004000U, /* FRC RAM addresses */
	0x00014060U, 0x00000101U, /* FRC.PUNCTCTRL: PUNCT0=1, PUNCT1=1 */
	0x000140A8U, 0x00000007U, /* FRC.AUTOCG.AUTOCGEN=7 */
	0x000440BCU, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
	0x00034108U, 0x00004CFFU, 0x00004DFFU, 0x00004DFFU, /* FRC.FCD1..3: all
				    * three have WORDS=255, INCLUDECRC=1,
				    * CALCCRC=1, SKIPWHITE=1; FCD1.BUFFER=0,
				    * FCD2/3.BUFFER=1 (SKIPCRC/ADDTRAILTXDATA/
				    * EXCLUDESUBFRAMEWCNT=0 on all three) */
	/*
	 * AGC/RFCRC values below are likewise decoded against their public
	 * field models (AGC_S_field.py/RFCRC_S_field.py). The two AND/OR
	 * pairs (table opcodes 1=AND-mask, 3=OR-value, radio.c's own
	 * EFR32_RADIO_TABLE_OP_* above) are decoded as what they actually
	 * are -- a preserved-field mask, then a written value -- not as if
	 * the raw words were final register contents.
	 */
	0x1001C020U, 0x0007F800U, /* AGC.CTRL0 AND-mask: preserves RSSISHIFT
				     * only, clears every other field first */
	0x3001C020U, 0x000802F5U, /* AGC.CTRL0 OR-value: PWRTARGET=245, MODE=2,
				     * DISCFLOOPADJ=1 (all other fields 0) */
	0x1001C024U, 0x000000FFU, /* AGC.CTRL1 AND-mask: preserves CCATHRSH
				     * only, clears every other field first */
	0x3001C024U, 0x00001300U, /* AGC.CTRL1 OR-value: RSSIPERIOD=3,
				     * PWRPERIOD=1 (all other fields 0) */
	0x0007C028U, 0x83B380ECU, 0x51407543U, 0x48000FA0U, 0x00004030U,
		0x00000000U, 0x00000000U, 0x0000022EU, /* AGC.CTRL2: SAFEMODETHD=3,
				     * REHICNTTHD=7, RELOTHD=4, RELBYCHPWR=3,
				     * RELTARGETPWR=236, DISRFPKD=1.
				     * AGC.CTRL3: IFPKDDEB/RFPKDDEB=1,
				     * IFPKDDEBTHD/RFPKDDEBTHD=1,
				     * IFPKDDEBPRD/RFPKDDEBPRD=40,
				     * IFPKDDEBRST/RFPKDDEBRST=10.
				     * AGC.CTRL4: PERIODRFPKD=4000, RFPKDPRDGEAR=4,
				     * FRZPKDEN=1. AGC.CTRL5: PNUPDISTHD=48,
				     * PNUPRELTHD=4. AGC.CTRL6: all fields 0.
				     * AGC.RSSISTEPTHR: all fields 0.
				     * AGC.MANGAIN: MANGAINIFPGA=7, MANGAINLNA=1,
				     * MANGAINPN=1 (MANGAINEN=0, so inactive) */
	0x0004C050U, 0x04301151U, 0xE6092D0EU, 0x08070654U, 0x0002B6D1U,
				     /* Previously unnamed. AGC.GAINRANGE:
				      * LNAINDEXBORDER=1, PGAINDEXBORDER=5,
				      * GAININCSTEP=1, PNGAINSTEP=1, HIPWRTHD=3,
				      * BOOSTLNA=1. AGC.AGCPERIOD: PERIODHI=14,
				      * PERIODLO=45, MAXHICNTTHD=9,
				      * SETTLETIMEIF=6, SETTLETIMERF=14.
				      * AGC.HICNTREGION: HICNTREGION0..4=4..8.
				      * AGC.STEPDWN: STEPDWN0..5=1,2,3,3,3,5 */
	0x000AC064U, 0x1C003004U, 0x09183040U, 0x2079640DU, 0x01FBFCEBU,
		0x03E8F67FU, 0x15724BBDU, 0x0518A311U, 0x76543210U,
		0x00000A98U, 0x00000000U, /* AGC.GAINSTEPLIM: CFLOOPSTEPMAX=4,
				     * HYST=3, PNINDEXMAX=14. AGC.PNRFATT0..3 and
				     * LNAMIXCODE0..1: a monotonic LNA/mixer
				     * attenuation-code and slice-code table
				     * (LNAMIXRFATT1..17, LNAMIXSLICE1..10),
				     * values listed in this register's own
				     * name, not re-copied into this comment.
				     * AGC.PGACODE0..1: PGAGAIN1..11=0..10
				     * (a plain ascending index). AGC.LBT: all
				     * fields 0 (LBT/CCA timing unused here) */
	0x01010008U, 0x00000708U, /* RFCRC.CTRL: CRCWIDTH=2, BITSPERWORD=7 */
	0x01010018U, 0x00AAAAAAU, /* RFCRC.INIT=0xAAAAAA (BLE's standard CRC
				     * preset, not a per-field bitmask) */
	0x01010020U, 0x00DA6000U, /* RFCRC.POLY=0xDA6000 (BLE's standard CRC
				     * polynomial, not a per-field bitmask) */
	/* MODEM/SYNTH values below decoded against MODEM_S_field.py/
	 * SYNTH_S_field.py the same way; fields not named below are 0. */
	0x0101401CU, 0x00000010U, /* MODEM.MIXCTRL: DIGIQSWAPEN=1 */
	0x0103402CU, 0x00008000U, 0x03000000U, 0x00000000U, /* MODEM.CTRL3:
				     * TSAMPDEL=2. MODEM.CTRL4: ADCSATLEVEL=6.
				     * MODEM.CTRL5: all fields 0 */
	0x01024068U, 0x71764129U, 0x00000000U, /* MODEM.SYNC0=0x71764129
				     * (BLE access-address-derived sync word).
				     * MODEM.SYNC1=0 */
	0x010240BCU, 0x00000000U, 0x00000000U, /* MODEM.SHAPING10..11: all
				     * fields 0 */
	0x010240E0U, 0x00000033U, 0x00000000U, /* MODEM.DCCOMP: DCESTIEN=1,
				     * DCCOMPEN=1, DCCOMPGEAR=3.
				     * MODEM.DCCOMPFILTINIT: all fields 0 */
	0x010240F4U, 0x07830464U, 0x3AC81388U, /* MODEM.DSATHD0: SPIKETHD=100,
				     * UNMODTHD=4, FDEVMINTHD=12, FDEVMAXTHD=120.
				     * MODEM.DSATHD1: POWABSTHD=5000, DSARSTCNT=2,
				     * RSSIJMPTHD=6, FREQLATDLY=1, PWRFLTBYP=1,
				     * AMPFLTBYP=1, PWRDETDIS=1 */
	0x01014124U, 0x00000000U, /* MODEM.AUTOCG: all fields 0 */
	0x01024130U, 0x0C660664U, 0x0000010CU, /* MODEM.DSATHD2:
				     * POWABSTHDLOG=100, JUMPDETEN=1, FDADJTHD=1,
				     * PMDETPASSTHD=6, FREQESTTHD=6,
				     * INTERFERDET=6. MODEM.DIRECTMODE:
				     * SYNCPREAM=3, CLKWIDTH=1 */
	0x01014154U, 0x00000101U, /* MODEM.LRFRC: CI500=1, LRCORRMODE=1 */
	0x01024168U, 0x07830464U, 0x00821388U, /* MODEM.DSATHD3: SPIKETHDLO=100,
				     * UNMODTHDLO=4, FDEVMINTHDLO=12,
				     * FDEVMAXTHDLO=120. MODEM.DSATHD4:
				     * POWABSTHDLO=5000, ARRTOLERTHD0LO=2,
				     * ARRTOLERTHD1LO=4 */
	0x01014230U, 0x00000000U, /* MODEM.BLEIQDSA: all fields 0 */
	0x01024238U, 0x00000000U, 0x00000000U, /* MODEM.SYNCPROPERTIES,
				     * DIGIGAINCTRL: all fields 0 */
	0x01014244U, 0x00000000U, /* MODEM.PADEBUG: all fields 0 */
	0x01094254U, 0x00000000U, 0x003C0000U, 0x00000000U, 0x00000000U,
		0x55555555U, 0x00000017U, 0x00000000U, 0x00000000U, 0x0006AAAAU,
				     /* MODEM.ETSCTRL: all fields 0.
				      * MODEM.ANTSWCTRL: EXTDSTOPPULSECNT=30.
				      * MODEM.ANTSWSTART/END: 0.
				      * MODEM.TRECPMPATT=0x55555555 (alternating
				      * preamble-match pattern, not a bitmask).
				      * MODEM.TRECPMDET: PMACQUINGWIN=7,
				      * PMCOSTVALTHD=2. MODEM.CFGANTPATT: 0.
				      * MODEM.ETSTIM: 0. MODEM.ANTSWCTRL1:
				      * TIMEPERIOD=436906 */
	0x0102427CU, 0x00000000U, 0x00000000U, /* MODEM.ANTDIVCTRL,
				     * BLEIQDSAEXT2: all fields 0 */
	0x01018010U, 0x00000003U, /* SYNTH.CTRL: LOCKTHRESHOLD=3 */
	0x0101803CU, 0x00000001U, /* SYNTH.DIVCTRL: LODIVFREQCTRL=1 */
	0x010280B0U, 0x02000300U, 0x01000037U, /* SYNTH.DSMCTRLRX: DSMMODERX=1,
				     * LSBFORCERX=1, MASHORDERRX=1.
				     * SYNTH.DSMCTRLTX: DITHERDSMINPUTTX=1,
				     * DITHERDSMOUTPUTTX=3, DITHERDACTX=3,
				     * DEMMODETX=1 */
	/* RAC values below decoded against RAC_S_field.py the same way. */
	0x02020098U, 0x04000C00U, 0x0000004CU, /* RAC.SYNTHREGCTRL:
				     * MMDLDOVREFTRIM=3, CHPLDOVREFTRIM=4.
				     * RAC.VCOCTRL: VCOAMPLITUDE=12,
				     * VCODETAMPLITUDE=4 */
	0x020100A4U, 0x00000400U, /* RAC.SYNTHCTRL: MMDPOWERBALANCEDISABLE=1 */
	0x020200D0U, 0xAA400005U, 0x00000188U, /* RAC.CLKMULTEN0:
				     * CLKMULTBWCAL=1, CLKMULTDISICO=1,
				     * CLKMULTFREQCAL=1, CLKMULTREG2ADJI=2,
				     * CLKMULTREG1ADJV=2, CLKMULTREG2ADJV=2,
				     * CLKMULTREG3ADJV=2 (all other fields 0).
				     * RAC.CLKMULTEN1: CLKMULTINNIBBLE=8,
				     * CLKMULTRDNIBBLE=3 */
	0x020200F4U, 0x00000000U, 0x1108213DU, /* RAC.LNAMIXDEBUG: all fields
				     * 0. RAC.LNAMIXTRIM0: LNAMIXCURCTRL=61,
				     * LNAMIXLOWCUR=1, LNAMIXRFPKDBWSEL=2,
				     * LNAMIXRFPKDCALCM=32, LNAMIXRFPKDCALDM=16,
				     * LNAMIXTRIMVREG=8 */
	0x120100FCU, 0x0000003FU, /* RAC.LNAMIXTRIM1 AND-mask: preserves
				     * LNAMIXIBIASADJ only, clears the rest first */
	0x320100FCU, 0x00045400U, /* RAC.LNAMIXTRIM1 OR-value: LNAMIXMXRBIAS=2,
				     * LNAMIXNCASADJ=2, LNAMIXPCASADJ=2,
				     * LNAMIXVOUTADJ=8 */
	0x02010130U, 0x02510060U, /* RAC.PGACTRL: PGAENLATCHI=1, PGAENLATCHQ=1,
				     * PGATHRPKDLOSEL=1, PGATHRPKDHISEL=5,
				     * LNAMIXRFPKDTHRESHSEL=2 */
	0x02010168U, 0x00000400U, /* RAC.SYMMDCTRL: SYMMDMODE=2 */
	EFR32_RADIO_TABLE_END,
};

static const uint32_t efr32_ble_1m_delta_table[] = {
	/* The omitted first record is the non-portable SEQ.PHYINFO pointer. */
	0x00014010U, 0x00004101U, /* FRC.MAXLENGTH: MAXLENGTH=257, INILENGTH=4 */
	0x0001403CU, 0x00000000U, /* FRC.FECCTRL: all fields zero (no FEC on
				    * an uncoded 1M PHY) */
	0x00014048U, 0x000407A0U, /* FRC.CTRL: TXFCDMODE=2, RXFCDMODE=2,
				    * BITSPERWORD=7, LPMODEDIS=1 (RANDOMTX,
				    * UARTMODE, BITORDER, RATESELECT,
				    * TXPREFETCH, SEQHANDSHAKE, PRBSTEST,
				    * WAITEOFEN, RXABORTIGNOREDIS=0);
				    * docs/radio-tables.md §7.16 */
	0x00014050U, 0x00000000U, /* FRC.TRAILTXDATACTRL: all fields zero */
	0x0001405CU, 0x00000000U, /* FRC.CONVGENERATOR: all fields zero */
	0x00014104U, 0x00004CFFU, /* FRC.FCD0: WORDS=255, BUFFER=0,
				    * INCLUDECRC=1, CALCCRC=1, SKIPWHITE=1
				    * (SKIPCRC/ADDTRAILTXDATA/
				    * EXCLUDESUBFRAMEWCNT=0) */
	/* MODEM/SYNTH/RAC values below decoded against *_S_field.py the
	 * same way as efr32_ble_1m_base_table; fields not named are 0. */
	0x01034020U, 0x86000000U, 0x0001C01FU, 0x00006000U, /* MODEM.CTRL0:
				     * SHAPING=3, FRAMEDETDEL=2. MODEM.CTRL1:
				     * SYNCBITS=31, COMPMODE=3, RESYNCPER=1.
				     * MODEM.CTRL2: DATAFILTER=6 */
	0x01014038U, 0x40000000U, /* MODEM.CTRL6: RXBRCALCDIS=1 */
	0x01044058U, 0x00FF04C8U, 0x00000840U, 0x00000001U, 0x00040092U,
				     /* MODEM.TXBR: TXBRNUM=1224, TXBRDEN=255.
				      * MODEM.RXBR: RXBRDEN=2, RXBRINT=2.
				      * MODEM.CF: DEC0=1. MODEM.PRE: BASE=2,
				      * BASEBITS=1, PREERRORS=1, TXBASES=4 */
	0x010F4080U, 0x020C008CU, 0x00000000U, 0x002203B6U, 0x00000000U,
		0x00000000U, 0x0E050200U, 0x53432F1CU, 0x61686962U, 0x04324355U,
		0x0E0E1016U, 0x080A0C0DU, 0x04050607U, 0x02020303U, 0x01010102U,
		0x00010101U, /* MODEM.TIMING: TIMTHRESH=140, FDM0THRESH=3,
				      * OFFSUBDEN=1. MODEM.DSSS0: 0.
				      * MODEM.MODINDEX: MODINDEXM=22,
				      * MODINDEXE=29, FREQGAINE=2, FREQGAINM=4.
				      * MODEM.AFC/AFCADJLIM: 0.
				      * MODEM.SHAPING0..9: a Gaussian pulse-
				      * shaping FIR coefficient table
				      * (COEFF1..38), the individual taps named
				      * in each register, not re-copied here */
	0x010140ECU, 0x8C2BE890U, /* MODEM.SRCCHF: SRCRATIO1=144, SRCENABLE1=1,
				     * SRCRATIO2=17086, SRCENABLE2=1, INTOSR=1 */
	0x010340FCU, 0x000A2090U, 0x80585B05U, 0x4A413747U, /* MODEM.DSACTRL:
				     * ARRTHD=4, ARRTOLERTHD0=2, ARRTOLERTHD1=4,
				     * FREQAVGSYM=1, DSARSTON=1.
				     * MODEM.VITERBIDEMOD: VTDEMODEN=1,
				     * VITERBIKSI1=65, VITERBIKSI2=45,
				     * VITERBIKSI3=24, SYNTHAFC=1, DISDEMODOF=1.
				     * MODEM.VTCORRCFG0.EXPECTPATT=0x4A413747
				     * (a fixed correlation pattern, not a
				     * per-field bitmask) */
	0x0103410CU, 0x005262F5U, 0x2949192AU, 0x40403B89U, /* MODEM.DIGMIXCTRL:
				     * DIGMIXFREQ=156405, DIGMIXMODE=1,
				     * DIGMIXFB=1. MODEM.VTCORRCFG1:
				     * CORRSHFTLEN=42, VTFRQLIM=100,
				     * EXPSYNCLEN=146, BUFFHEAD=2, EXPECTHT=5.
				     * MODEM.VTTRACK: FREQTRACKMODE=1,
				     * TIMTRACKTHD=2, TIMEACQUTHD=238,
				     * HIPWRTHD=1, SYNCTIMEOUTSEL=1 */
	0x01014128U, 0x0000DDFFU, /* MODEM.CGCLKSTOP.FORCEOFF=0xDDFF (a
				     * per-sub-block clock-gate bitmask, not a
				     * single named field) */
	0x01074138U, 0x00FA53E8U, 0x00000000U, 0x00000000U, 0x00000000U,
		0x00000000U, 0x00000000U, 0x00000000U, /* MODEM.LONGRANGE:
				     * LRCORRTHD=1000, LRCORRSCHWIN=10,
				     * LRTIMCORRTHD=250. MODEM.LONGRANGE1..6:
				     * all fields 0 (unused outside LE Coded) */
	0x01014170U, 0x000003C1U, /* MODEM.VTBLETIMING: VTBLETIMINGSEL=1,
				     * TIMINGDELAY=60 */
	0x01014234U, 0x0E000000U, /* MODEM.BLEIQDSAEXT1: MAXCORRCNTIQDSA=7 */
	0x01014248U, 0xC01EAA58U, /* MODEM.REALTIMCFE: MINCOSTTHD=600,
				     * RTSCHWIN=10, TRACKINGWIN=5,
				     * SYNCACQWIN=7, VTAFCFRAME=1, RTCFEEN=1 */
	0x01014278U, 0x00000000U, /* MODEM.COCURRMODE: all fields 0 */
	0x01018038U, 0x00102555U, /* SYNTH.IFFREQ: IFFREQ=9557, LOSIDE=1 */
	0x0103809CU, 0x00000000U, 0x00037870U, 0x000000D0U, /* SYNTH.LPFCTRL1CAL:
				     * 0. SYNTH.LPFCTRL1RX: OP1COMPRX=7,
				     * RPVALRX=7, RZVALRX=13. SYNTH.LPFCTRL1TX:
				     * OP1COMPTX=13 */
	0x110180A8U, 0x000001F0U, /* SYNTH.LPFCTRL2RX AND-mask: preserves
				     * CALCRX only, clears the rest first */
	0x310180A8U, 0x01CB4205U, /* SYNTH.LPFCTRL2RX OR-value: LPFSWENRX=1,
				     * LPFINCAPRX=2, CASELRX=1, CAVALRX=16,
				     * CZSELRX=1, CZVALRX=229 */
	0x110180ACU, 0x000001F0U, /* SYNTH.LPFCTRL2TX AND-mask: preserves
				     * CALCTX only, clears the rest first */
	0x310180ACU, 0x00FD3E05U, /* SYNTH.LPFCTRL2TX OR-value: LPFSWENTX=1,
				     * LPFINCAPTX=2, CASELTX=1, CAVALTX=15,
				     * CZSELTX=1, CZVALTX=126 */
	0x020100E4U, 0x13512C6EU, /* RAC.IFADCTRIM0: IFADCENHALFMODE=1,
				     * IFADCLDOSERIESAMPLVL=3,
				     * IFADCLDOSHUNTAMPLVL1=3,
				     * IFADCLDOSHUNTCURLVL1=6,
				     * IFADCLDOSHUNTCURLVL2=2, IFADCOTACURRENT=2,
				     * IFADCREFBUFAMPLVL=4, IFADCREFBUFCURLVL=2,
				     * IFADCSIDETONEAMP=3, IFADCSIDETONEFREQ=2 */
	0x02010154U, 0x00023FC4U, /* RAC.SYTRIM1: SYLODIVLDOTRIMNDIO=1,
				     * SYMMDREPLICA1CURRADJ=7,
				     * SYMMDREPLICA2CURRADJ=7,
				     * SYTRIMMMDREGAMPBIAS=3,
				     * SYLODIVRLOADCCLKSEL=1 */
	EFR32_RADIO_TABLE_END,
};

/* Public BLE viterbi 2M delta at configurator_out.c:537-598; PHYINFO omitted. */
static const uint32_t efr32_ble_2m_delta_table[] = {
	/* Same FRC field decode as efr32_ble_1m_delta_table above -- the
	 * values are identical between the two PHYs. */
	0x00014010U, 0x00004101U, /* FRC.MAXLENGTH: MAXLENGTH=257, INILENGTH=4 */
	0x0001403CU, 0x00000000U, /* FRC.FECCTRL: all fields zero (no FEC on
				    * an uncoded 2M PHY) */
	0x00014048U, 0x000407A0U, /* FRC.CTRL: TXFCDMODE=2, RXFCDMODE=2,
				    * BITSPERWORD=7, LPMODEDIS=1 (all other
				    * fields 0); docs/radio-tables.md §7.16 */
	0x00014050U, 0x00000000U, /* FRC.TRAILTXDATACTRL: all fields zero */
	0x0001405CU, 0x00000000U, /* FRC.CONVGENERATOR: all fields zero */
	0x00014104U, 0x00004CFFU, /* FRC.FCD0: WORDS=255, BUFFER=0,
				    * INCLUDECRC=1, CALCCRC=1, SKIPWHITE=1 */
	/* MODEM/SYNTH/RAC values below decoded against *_S_field.py the
	 * same way as efr32_ble_1m_delta_table; fields not named are 0. */
	0x01034020U, 0x86000000U, 0x0001C01FU, 0x00006000U, /* MODEM.CTRL0:
				     * SHAPING=3, FRAMEDETDEL=2. MODEM.CTRL1:
				     * SYNCBITS=31, COMPMODE=3, RESYNCPER=1.
				     * MODEM.CTRL2: DATAFILTER=6 (identical to
				     * the 1M PHY's own CTRL0..2) */
	0x01014038U, 0x40000000U, /* MODEM.CTRL6: RXBRCALCDIS=1 (identical
				     * to the 1M PHY) */
	0x01044058U, 0x00FF0264U, 0x00000840U, 0x00000001U, 0x00080092U,
				     /* MODEM.TXBR: TXBRNUM=612, TXBRDEN=255 (the
				      * 2M-PHY-specific bit rate; differs from
				      * 1M's TXBRNUM=1224). MODEM.RXBR:
				      * RXBRDEN=2, RXBRINT=2. MODEM.CF: DEC0=1.
				      * MODEM.PRE: BASE=2, BASEBITS=1,
				      * PREERRORS=1, TXBASES=8 (differs from
				      * 1M's TXBASES=4) */
	0x010F4080U, 0x020C008CU, 0x00000000U, 0x002203D7U, 0x00000000U,
		0x00000000U, 0x0D050200U, 0x503F2D1AU, 0x63696860U, 0x25354657U,
		0x08090D15U, 0x05050607U, 0x03040404U, 0x02030303U, 0x02020202U,
		0x01010101U, /* MODEM.TIMING: TIMTHRESH=140, FDM0THRESH=3,
				      * OFFSUBDEN=1 (identical to 1M).
				      * MODEM.DSSS0/AFC/AFCADJLIM: 0.
				      * MODEM.MODINDEX: MODINDEXM=23,
				      * MODINDEXE=30, FREQGAINE=2, FREQGAINM=4
				      * (differs slightly from 1M's
				      * MODINDEXM=22/MODINDEXE=29).
				      * MODEM.SHAPING0..9: the 2M PHY's own
				      * Gaussian pulse-shaping FIR coefficients
				      * (COEFF1..39), narrower than 1M's since
				      * 2M halves the symbol period */
	0x010140ECU, 0x8C2C3890U, /* MODEM.SRCCHF: SRCRATIO1=144, SRCENABLE1=1,
				     * SRCRATIO2=17091, SRCENABLE2=1, INTOSR=1 */
	0x010340FCU, 0x000A209CU, 0x80585B05U, 0x4A413747U, /* MODEM.DSACTRL:
				     * ARRTHD=7, ARRTOLERTHD0=2, ARRTOLERTHD1=4,
				     * FREQAVGSYM=1, DSARSTON=1 (ARRTHD differs
				     * from 1M's 4). MODEM.VITERBIDEMOD:
				     * VTDEMODEN=1, VITERBIKSI1=65,
				     * VITERBIKSI2=45, VITERBIKSI3=24,
				     * SYNTHAFC=1, DISDEMODOF=1 (identical to
				     * 1M). MODEM.VTCORRCFG0.EXPECTPATT=
				     * 0x4A413747 (identical to 1M) */
	0x0103410CU, 0x005255B8U, 0x2949192AU, 0x40403B89U, /* MODEM.DIGMIXCTRL:
				     * DIGMIXFREQ=153016, DIGMIXMODE=1,
				     * DIGMIXFB=1 (DIGMIXFREQ differs from 1M's
				     * 156405, matching the 2M PHY's own IF).
				     * MODEM.VTCORRCFG1/VTTRACK: identical to
				     * the 1M PHY's own values */
	0x01014128U, 0x0000DDFFU, /* MODEM.CGCLKSTOP.FORCEOFF=0xDDFF
				     * (identical to 1M) */
	0x01074138U, 0x00FA53E8U, 0x00000000U, 0x00000000U, 0x00000000U,
		0x00000000U, 0x00000000U, 0x00000000U, /* MODEM.LONGRANGE/
				     * LONGRANGE1..6: identical to 1M (unused
				     * outside LE Coded, same on every
				     * uncoded PHY) */
	0x01014170U, 0x000003C1U, /* MODEM.VTBLETIMING: identical to 1M */
	0x01014234U, 0x0E000000U, /* MODEM.BLEIQDSAEXT1: identical to 1M */
	0x01014248U, 0xC01EAA58U, /* MODEM.REALTIMCFE: identical to 1M */
	0x01014278U, 0x00000000U, /* MODEM.COCURRMODE: all fields 0 */
	0x01018038U, 0x00104911U, /* SYNTH.IFFREQ: IFFREQ=18705, LOSIDE=1
				     * (differs from 1M's IFFREQ=9557, matching
				     * the 2M PHY's wider channel spacing) */
	0x0103809CU, 0x00000000U, 0x00037870U, 0x0002C0FFU, /* SYNTH.LPFCTRL1CAL:
				     * 0. SYNTH.LPFCTRL1RX: OP1COMPRX=7,
				     * RPVALRX=7, RZVALRX=13 (identical to 1M).
				     * SYNTH.LPFCTRL1TX: OP1BWTX=15,
				     * OP1COMPTX=15, RZVALTX=11 (wider TX
				     * filter bandwidth than 1M's OP1COMPTX=13
				     * alone, matching 2M's doubled symbol
				     * rate) */
	0x110180A8U, 0x000001F0U, /* SYNTH.LPFCTRL2RX AND-mask: preserves
				     * CALCRX only, clears the rest first */
	0x310180A8U, 0x01CB4205U, /* SYNTH.LPFCTRL2RX OR-value: identical to
				     * 1M (RX filter unaffected by TX PHY rate) */
	0x110180ACU, 0x000001F0U, /* SYNTH.LPFCTRL2TX AND-mask: preserves
				     * CALCTX only, clears the rest first */
	0x310180ACU, 0x000D0A05U, /* SYNTH.LPFCTRL2TX OR-value: LPFSWENTX=1,
				     * LPFINCAPTX=2, CASELTX=1, CAVALTX=2,
				     * CZSELTX=1, CZVALTX=6 (narrower than 1M's
				     * CAVALTX=15/CZVALTX=126, matching 2M's
				     * wider TX filter bandwidth above) */
	0x020100E4U, 0x11512C6CU, /* RAC.IFADCTRIM0: IFADCLDOSERIESAMPLVL=3,
				     * IFADCLDOSHUNTAMPLVL1=3,
				     * IFADCLDOSHUNTCURLVL1=6,
				     * IFADCLDOSHUNTCURLVL2=2, IFADCOTACURRENT=2,
				     * IFADCREFBUFAMPLVL=4, IFADCREFBUFCURLVL=2,
				     * IFADCSIDETONEAMP=1, IFADCSIDETONEFREQ=2
				     * (IFADCENHALFMODE=0 and
				     * IFADCSIDETONEAMP=1 differ from 1M) */
	0x02010154U, 0x00003FC4U, /* RAC.SYTRIM1: SYLODIVLDOTRIMNDIO=1,
				     * SYMMDREPLICA1CURRADJ=7,
				     * SYMMDREPLICA2CURRADJ=7,
				     * SYTRIMMMDREGAMPBIAS=3
				     * (SYLODIVRLOADCCLKSEL=0 differs from 1M) */
	EFR32_RADIO_TABLE_END,
};

/* Public BLE long-range 125 kb/s delta at configurator_out.c:600-661. */
static const uint32_t efr32_ble_coded_s8_delta_table[] = {
	/* The omitted first record is the non-portable SEQ.PHYINFO pointer.
	 * Unlike the 1M/2M tables above, this table and the S2 one below
	 * previously had no per-register naming at all; every entry below,
	 * including the MODEM/SYNTH/RAC calibration words, is now named and
	 * field-decoded against build/reference-models/*_S_field.py, the
	 * same way as the 1M/2M tables. */
	0x00014010U, 0x00004100U, /* FRC.MAXLENGTH: MAXLENGTH=256, INILENGTH=4 */
	0x0001403CU, 0x00000010U, /* FRC.FECCTRL: CONVMODE=1 (coded PHY uses
				    * the convolutional encoder; all other
				    * fields 0) */
	0x00014048U, 0x00040720U, /* FRC.CTRL: TXFCDMODE=2, RXFCDMODE=0,
				    * BITSPERWORD=7, LPMODEDIS=1 (all other
				    * fields 0) */
	0x00014050U, 0x00000400U, /* FRC.TRAILTXDATACTRL: TRAILTXDATACNT=4
				    * (all other fields 0) */
	0x0001405CU, 0x00000F0DU, /* FRC.CONVGENERATOR: GENERATOR0=13,
				    * GENERATOR1=15 */
	0x00014104U, 0x0001C003U, /* FRC.FCD0: WORDS=3, SKIPWHITE=1,
				    * ADDTRAILTXDATA=1, EXCLUDESUBFRAMEWCNT=1
				    * (BUFFER/INCLUDECRC/CALCCRC/SKIPCRC=0) */
	/* MODEM/SYNTH/RAC values below now also decoded against *_S_field.py,
	 * the same way as the 1M/2M tables; fields not named are 0. */
	0x01034020U, 0x46000030U, 0x0001C81FU, 0x10002000U, /* MODEM.CTRL0:
				     * CODING=3, SHAPING=3, FRAMEDETDEL=1.
				     * MODEM.CTRL1: SYNCBITS=31, SYNCDATA=1,
				     * COMPMODE=3, RESYNCPER=1. MODEM.CTRL2:
				     * DATAFILTER=2, RATESELMODE=2 */
	0x01014038U, 0x00000000U, /* MODEM.CTRL6: all fields 0 */
	0x01044058U, 0x00050018U, 0x00000840U, 0x00000009U, 0x000A0092U,
				     /* MODEM.TXBR: TXBRNUM=24, TXBRDEN=5 (LE
				      * Coded's own, much lower, symbol rate).
				      * MODEM.RXBR: RXBRDEN=2, RXBRINT=2.
				      * MODEM.CF: DEC0=1, DEC1=1. MODEM.PRE:
				      * BASE=2, BASEBITS=1, PREERRORS=1,
				      * TXBASES=10 */
	0x010F4080U, 0x022C008CU, 0x00000000U, 0x00120012U, 0x1B8C805BU,
		0x00000000U, 0x0E050200U, 0x53432F1CU, 0x61686962U, 0x04324355U,
		0x0E0E1016U, 0x080A0C0DU, 0x04050607U, 0x02020303U, 0x01010102U,
		0x00010101U, /* MODEM.TIMING: TIMTHRESH=140, FDM0THRESH=3,
				      * OFFSUBNUM=1, OFFSUBDEN=1. MODEM.DSSS0: 0.
				      * MODEM.MODINDEX: MODINDEXM=18,
				      * FREQGAINE=2, FREQGAINM=2. MODEM.AFC:
				      * AFCSCALEM=27, AFCSCALEE=2, AFCRXCLR=1,
				      * AFCDEL=12, AFCAVGPER=4, AFCLIMRESET=1,
				      * AFCONESHOT=1, AFCDSAFREQOFFEST=1,
				      * AFCDELDET=1 (LE Coded is the only PHY
				      * here with a nonzero MODEM.AFC -- 1M/2M
				      * leave automatic frequency correction
				      * off). MODEM.AFCADJLIM: 0.
				      * MODEM.SHAPING0..9: coefficients
				      * (COEFF1..38) identical to the 1M PHY's
				      * own pulse-shaping table */
	0x010140ECU, 0x8CB1B080U, /* MODEM.SRCCHF: SRCRATIO1=128,
				     * SRCRATIO2=19227, SRCENABLE2=1, INTOSR=1 */
	0x010340FCU, 0x000A209CU, 0x80206101U, 0x123556B7U, /* MODEM.DSACTRL:
				     * ARRTHD=7, ARRTOLERTHD0=2, ARRTOLERTHD1=4,
				     * FREQAVGSYM=1, DSARSTON=1.
				     * MODEM.VITERBIDEMOD: VTDEMODEN=1,
				     * VITERBIKSI1=64, VITERBIKSI2=48,
				     * VITERBIKSI3=32, DISDEMODOF=1 (no
				     * SYNTHAFC here, unlike 1M/2M).
				     * MODEM.VTCORRCFG0.EXPECTPATT=0x123556B7
				     * (LE Coded's own correlation pattern,
				     * differs from 1M/2M's 0x4A413747) */
	0x0103410CU, 0x001255B8U, 0x29041BA0U, 0x40408808U, /* MODEM.DIGMIXCTRL:
				     * DIGMIXFREQ=153016, DIGMIXMODE=1 (no
				     * DIGMIXFB here, unlike 1M/2M).
				     * MODEM.VTCORRCFG1: CORRSHFTLEN=32,
				     * VTFRQLIM=110, EXPSYNCLEN=8, BUFFHEAD=2,
				     * EXPECTHT=5. MODEM.VTTRACK:
				     * TIMTRACKTHD=2, TIMEACQUTHD=32,
				     * TIMEOUTMODE=1, HIPWRTHD=1,
				     * SYNCTIMEOUTSEL=1 (no FREQTRACKMODE here,
				     * unlike 1M/2M) */
	0x01014128U, 0x00000000U, /* MODEM.CGCLKSTOP: all fields 0 */
	0x01074138U, 0x3BE8D44CU, 0x306F1904U, 0x1A140E08U, 0x322C2620U,
		0x32103E38U, 0x0A987654U, 0x00005F1DU, /* MODEM.LONGRANGE:
				     * LRCORRTHD=1100, LRCORRSCHWIN=10,
				     * LRBLE=1, LRTIMCORRTHD=1000, LRBLEDSA=1,
				     * LRDEC=3 (S8-specific; S2 below has the
				     * same first two LONGRANGE/LONGRANGE1
				     * words but different LRCHPWRTH thresholds
				     * from LONGRANGE2 onward). MODEM.LONGRANGE1:
				     * LRSS=4, LRTIMEOUTTHD=400, CHPWRACCUDEL=3,
				     * HYSVAL=3, AVGWIN=3, LOGICBASEDPUGATE=1,
				     * LOGICBASEDLRDEMODGATE=1.
				     * MODEM.LONGRANGE2..6 (S8): LRCHPWRTH1..10
				     * step from 8 to 62 in steps of 6,
				     * LRCHPWRSH2..11 step from 1 to 10,
				     * LRCHPWRSPIKETH=29, LRSPIKETHD=95 */
	0x01014170U, 0x00000000U, 0x01014234U, 0x0E000800U, /* MODEM.VTBLETIMING:
				     * all fields 0. MODEM.BLEIQDSAEXT1:
				     * BLEIQDSATHCOMB=1, MAXCORRCNTIQDSA=7 */
	0x01014248U, 0x001F81F4U, 0x01014278U, 0x00000000U, /* MODEM.REALTIMCFE:
				     * MINCOSTTHD=500, TRACKINGWIN=7,
				     * SYNCACQWIN=7 (no RTCFEEN here, unlike
				     * 1M/2M). MODEM.COCURRMODE: all fields 0 */
	0x01018038U, 0x00104911U, /* SYNTH.IFFREQ: IFFREQ=18705, LOSIDE=1
				     * (identical to 2M) */
	0x0103809CU, 0x000240EBU, 0x00033870U, 0x000000D0U, /* SYNTH.LPFCTRL1CAL:
				     * OP1BWCAL=11, OP1COMPCAL=14, RZVALCAL=9
				     * (LE Coded is the only PHY here with a
				     * nonzero LPFCTRL1CAL). SYNTH.LPFCTRL1RX:
				     * OP1COMPRX=7, RPVALRX=7, RZVALRX=12
				     * (close to but not identical to 1M/2M's
				     * RZVALRX=13). SYNTH.LPFCTRL1TX:
				     * OP1COMPTX=13 (identical to 1M) */
	0x110180A8U, 0x000001F0U, /* SYNTH.LPFCTRL2RX AND-mask: preserves
				     * CALCRX only, clears the rest first */
	0x310180A8U, 0x01FD5A05U, /* SYNTH.LPFCTRL2RX OR-value: LPFSWENRX=1,
				     * LPFINCAPRX=2, CASELRX=1, CAVALRX=22,
				     * CZSELRX=1, CZVALRX=254 (wider than 1M/2M's
				     * CAVALRX=16/CZVALRX=229, matching LE
				     * Coded's narrower effective bandwidth) */
	0x110180ACU, 0x000001F0U, /* SYNTH.LPFCTRL2TX AND-mask: preserves
				     * CALCTX only, clears the rest first */
	0x310180ACU, 0x00FD3E05U, /* SYNTH.LPFCTRL2TX OR-value: identical to
				     * the 1M PHY's own LPFCTRL2TX OR-value */
	0x020100E4U, 0x11512C6CU, 0x02010154U, 0x00003FC4U, /* RAC.IFADCTRIM0,
				     * RAC.SYTRIM1: identical to the 2M PHY's
				     * own values */
	EFR32_RADIO_TABLE_END,
};

/* Public BLE long-range 500 kb/s delta at configurator_out.c:663-724. */
static const uint32_t efr32_ble_coded_s2_delta_table[] = {
	/* The omitted first record is the non-portable SEQ.PHYINFO pointer.
	 * FRC entries named/decoded the same way as the S8 table above. */
	0x00014010U, 0x00004100U, /* FRC.MAXLENGTH: MAXLENGTH=256, INILENGTH=4 */
	0x0001403CU, 0x00000010U, /* FRC.FECCTRL: CONVMODE=1 (all other
				    * fields 0) */
	0x00014048U, 0x00041720U, /* FRC.CTRL: TXFCDMODE=2, RXFCDMODE=0,
				    * BITSPERWORD=7, RATESELECT=2, LPMODEDIS=1
				    * (all other fields 0) */
	0x00014050U, 0x00000401U, /* FRC.TRAILTXDATACTRL: TRAILTXDATA=1,
				    * TRAILTXDATACNT=4 */
	0x0001405CU, 0x00000F0DU, /* FRC.CONVGENERATOR: GENERATOR0=13,
				    * GENERATOR1=15 */
	0x00014104U, 0x0001C003U, /* FRC.FCD0: WORDS=3, SKIPWHITE=1,
				    * ADDTRAILTXDATA=1, EXCLUDESUBFRAMEWCNT=1
				    * (BUFFER/INCLUDECRC/CALCCRC/SKIPCRC=0) */
	/* MODEM/SYNTH/RAC entries from here down are identical to the S8
	 * table's own values and decode above, except MODEM.LONGRANGE2..6,
	 * which carry S2's own LRCHPWRTH thresholds (and one extra field,
	 * LRCHPWRTH11/LRCHPWRSH12, that S8's narrower table omits). */
	0x01034020U, 0x46000030U, 0x0001C81FU, 0x10002000U,
	0x01014038U, 0x00000000U,
	0x01044058U, 0x00050018U, 0x00000840U, 0x00000009U, 0x000A0092U,
	0x010F4080U, 0x022C008CU, 0x00000000U, 0x00120012U, 0x1B8C805BU,
		0x00000000U, 0x0E050200U, 0x53432F1CU, 0x61686962U, 0x04324355U,
		0x0E0E1016U, 0x080A0C0DU, 0x04050607U, 0x02020303U, 0x01010102U,
		0x00010101U,
	0x010140ECU, 0x8CB1B080U,
	0x010340FCU, 0x000A209CU, 0x80206101U, 0x123556B7U,
	0x0103410CU, 0x001255B8U, 0x29041BA0U, 0x40408808U,
	0x01014128U, 0x00000000U,
	0x01074138U, 0x3BE8D44CU, 0x306F1904U, 0x1C16100AU, 0x342E2822U,
		0x3210403AU, 0x0A987654U, 0xA4805F1DU, /* MODEM.LONGRANGE2..6
				     * (S2): LRCHPWRTH1..10 step from 10 to 64
				     * in steps of 6, LRCHPWRSH2..11 step from
				     * 1 to 10, LRCHPWRSPIKETH=29,
				     * LRSPIKETHD=95, plus LRCHPWRTH11=72 and
				     * LRCHPWRSH12=10 in the top bits of
				     * LONGRANGE6 (S8's own LONGRANGE6 leaves
				     * those two fields at 0) */
	0x01014170U, 0x00000000U, 0x01014234U, 0x0E000800U,
	0x01014248U, 0x001F81F4U, 0x01014278U, 0x00000000U,
	0x01018038U, 0x00104911U,
	0x0103809CU, 0x000240EBU, 0x00033870U, 0x000000D0U,
	0x110180A8U, 0x000001F0U, 0x310180A8U, 0x01FD5A05U,
	0x110180ACU, 0x000001F0U, 0x310180ACU, 0x00FD3E05U,
	0x020100E4U, 0x11512C6CU, 0x02010154U, 0x00003FC4U,
	EFR32_RADIO_TABLE_END,
};

static void efr32_radio_table_apply(const uint32_t *table)
{
	for (size_t index = 0U; table[index] != EFR32_RADIO_TABLE_END;) {
		uint32_t header = table[index++];
		uint32_t operation = header >> 28;
		uint32_t block = (header >> 24) & 0xFU;
		uint32_t count = (header >> 16) & 0xFFU;
		/*
		 * The public configurator encodes secure 0xA8xx_xxxx block
		 * addresses. Zephyr's xG22 radio devicetree exposes radio RAM at
		 * 0xB0000000, and the generated *_NS models place these blocks in
		 * the non-secure 0xB8xx_xxxx window used by this application.
		 */
		uintptr_t address = 0xB8000000UL + ((uintptr_t)block << 16) +
			(header & 0xFFFFU);

		LL_ASSERT(count != 0U);
		for (uint32_t word = 0U; word < count; word++, address += sizeof(uint32_t)) {
			uint32_t value = table[index++];

			switch (operation) {
			case EFR32_RADIO_TABLE_OP_WRITE:
				*radio_reg32(address) = value;
				break;
			case EFR32_RADIO_TABLE_OP_AND:
				*radio_reg32(address) &= value;
				break;
			case EFR32_RADIO_TABLE_OP_OR:
				*radio_reg32(address) |= value;
				break;
			default:
				LL_ASSERT(false);
			}
		}
	}
}

static uint32_t bit_reverse24(uint32_t value)
{
	uint32_t reversed = 0U;

	for (uint32_t bit = 0U; bit < 24U; bit++) {
		reversed = (reversed << 1) | (value & 1U);
		value >>= 1;
	}

	return reversed;
}

static struct {
	struct ccm config;
	struct pdu_data *rx_in;
	struct pdu_data *rx_out;
	bool pending_rx;
	bool done;
	bool mic_valid;
} ccm_ctx;

static void ccm_nonce_build(const struct ccm *ccm, uint8_t nonce[BLE_CCM_NONCE_SIZE])
{
	uint64_t counter = ccm->counter;

	memcpy(nonce, &counter, 5);
	nonce[4] |= ccm->direction << 7;
	memcpy(&nonce[5], ccm->iv, sizeof(ccm->iv));
}

BUILD_ASSERT(BLE_CCM_NONCE_SIZE == EFR32_RADIOAES_CCM_NONCE_SIZE);

/*
 * The link layer's AES-CCM (Core v6.1 Vol 6 Part E 2: 13-byte nonce, the header's first octet
 * masked as the one octet of associated data, a 4-octet MIC) on RADIOAES, called directly: no
 * PSA key slot, key schedule or allocation on the radio's deadline path. ccm->key is the session
 * key in the byte order AES takes it (ull_llcp_enc.c stores ecb_encrypt()'s big-endian output).
 */
static int ccm_run(bool encrypt, const struct ccm *ccm, const uint8_t *pdu_in, uint8_t *pdu_out,
		   size_t len, uint8_t *mic)
{
	uint8_t nonce[BLE_CCM_NONCE_SIZE];
	uint8_t aad = pdu_in[0] & BLE_CCM_AAD_MASK;

#if defined(CONFIG_EFR32_RADIO_AMATEUR)
	if (!efr32_radio_amateur_ccm_permitted()) {
		return -EPERM;
	}
#endif
	ccm_nonce_build(ccm, nonce);
	return efr32_radioaes_ccm(encrypt, ccm->key, sizeof(ccm->key), nonce, sizeof(nonce), &aad,
				  sizeof(aad), pdu_in + PDU_DC_LL_HEADER_SIZE,
				  pdu_out + PDU_DC_LL_HEADER_SIZE, len, mic, BLE_CCM_MIC_SIZE);
}

/*
 * Connection replies staged before the frame ends (hal/efr32/seq_abi.h, docs/radio-tables.md
 * 7.82). efr32_conn_staged: a stage is posted for the next armed receive. efr32_conn_received:
 * that receive completed; its outcome is still to be committed. efr32_conn_in_flight: the
 * sequencer's reply may be on air, so nothing that frames or tunes the radio (FRC packet
 * setup, PHY tables) may be rewritten, and a TXDONE of that reply must not be cleared.
 * efr32_conn_committing: isr_rx() is committing a reply the sequencer sent.
 * efr32_init_sent: the reply committed was an initiator's CONNECT_IND (7.87), whose transmit
 * completion is still to come.
 */
#if defined(CONFIG_EFR32_RADIO_CONN_STAGED)
#define EFR32_SEQ_CONN_WORD(offset) \
	radio_reg32(EFR32_SEQ_RAM_BASE + EFR32_SEQ_CONN_BLOCK + (offset))
static uint8_t MALIGN(4) efr32_conn_image[2][EFR32_PKT_SIZE_MAX];
static uint16_t efr32_conn_image_octets[2];
static uint16_t efr32_conn_generation;
static uint32_t efr32_conn_flags;
/* RX FIFO offset the staged receive's frame starts at (the sequencer reads it from there). */
static uint32_t efr32_conn_rx_offset;
static bool efr32_init_sent;
static bool efr32_conn_staged;
static bool efr32_conn_received;
static bool efr32_conn_in_flight;
static bool efr32_conn_committing;
static uint32_t efr32_conn_outcome_word;
/* SN/NESN octet the link layer wrote into its reply, to check against the sequencer's. */
static int16_t efr32_conn_reply_header = -1;

/* One reply image: header, length, payload (encrypted with its MIC when the link is). */
static uint16_t efr32_conn_image_build(uint8_t *image, const struct efr32_radio_conn_reply *r)
{
	image[0] = r->header;
	image[1] = r->len;
	if (r->len == 0U) {
		return PDU_DC_LL_HEADER_SIZE;
	}
	LL_ASSERT(r->len + (r->ccm != NULL ? BLE_CCM_MIC_SIZE : 0U) <= EFR32_PKT_PAYLOAD_MAX);
#if defined(CONFIG_EFR32_RADIO_AMATEUR)
	/* Refused (and the radio shut down) under amateur rules: nothing is staged. */
	if (r->ccm != NULL && !efr32_radio_amateur_ccm_permitted()) {
		return 0U;
	}
#endif
	if (r->ccm != NULL) {
		struct ccm ccm = *r->ccm;
		uint8_t nonce[BLE_CCM_NONCE_SIZE];
		uint8_t aad = r->header & BLE_CCM_AAD_MASK;

		ccm.counter = r->counter;
		ccm_nonce_build(&ccm, nonce);
		LL_ASSERT(efr32_radioaes_ccm(true, ccm.key, sizeof(ccm.key), nonce, sizeof(nonce),
					     &aad, sizeof(aad), r->payload,
					     image + PDU_DC_LL_HEADER_SIZE, r->len,
					     image + PDU_DC_LL_HEADER_SIZE + r->len,
					     BLE_CCM_MIC_SIZE) == 0);
		image[1] = r->len + BLE_CCM_MIC_SIZE;
	} else {
		memcpy(image + PDU_DC_LL_HEADER_SIZE, r->payload, r->len);
	}
	return PDU_DC_LL_HEADER_SIZE + image[1];
}

static void efr32_seq_stage(uint32_t flags, uint8_t rx_max,
			    const struct efr32_radio_conn_reply *cur,
			    const struct efr32_radio_conn_reply *next, const uint8_t *adva,
			    const struct efr32_radio_scan_stage *scan);

void efr32_radio_conn_stage(bool central, bool rx_free, bool crc_closes, uint8_t sn, uint8_t nesn,
			    uint8_t rx_max, const struct efr32_radio_conn_reply *cur,
			    const struct efr32_radio_conn_reply *next)
{
	efr32_seq_stage((central ? EFR32_SEQ_CONN_CENTRAL : 0U) |
			(rx_free ? EFR32_SEQ_CONN_RX_FREE : 0U) |
			(crc_closes ? EFR32_SEQ_CONN_CRC_CLOSES : 0U) |
			((sn & 1U) << EFR32_SEQ_CONN_SN_SHIFT) |
			((nesn & 1U) << EFR32_SEQ_CONN_NESN_SHIFT),
			rx_max, cur, next, NULL, NULL);
}

void efr32_radio_adv_stage(const uint8_t *scan_rsp, const uint8_t *adva, bool tx_addr)
{
	struct efr32_radio_conn_reply r = {
		.header = scan_rsp[0],
		.len = scan_rsp[1],
		.payload = scan_rsp + PDU_DC_LL_HEADER_SIZE,
	};

	/* The receive the ADV_IND turns to is configured: its length limit. */
	efr32_seq_stage(EFR32_SEQ_CONN_ADV | (tx_addr ? EFR32_SEQ_CONN_ADV_TXADD : 0U),
			efr32_packet_stage.max_len, &r, &r, adva, NULL);
}

void efr32_radio_scan_stage(const struct efr32_radio_scan_stage *stage)
{
	struct efr32_radio_conn_reply r = {
		.header = stage->reply[0],
		.len = stage->reply[1],
		.payload = stage->reply + PDU_AC_LL_HEADER_SIZE,
	};
	uint32_t flags = EFR32_SEQ_CONN_SCAN | (stage->rx_free ? EFR32_SEQ_CONN_RX_FREE : 0U);

	if (stage->connect) {
		flags |= EFR32_SEQ_CONN_INIT |
			 (stage->adv_addr_type ? EFR32_SEQ_CONN_ADV_TXADD : 0U) |
			 (stage->init_addr_type ? EFR32_SEQ_CONN_INIT_RXADD : 0U) |
			 (stage->tgta_rpa ? EFR32_SEQ_CONN_INIT_TGTA_RPA : 0U) |
			 (stage->timed ? EFR32_SEQ_CONN_INIT_TIMED : 0U);
	} else {
		flags |= EFR32_SEQ_CONN_SCAN_REQ;
	}
	/* The receive is configured (prepare_cb(), isr_common_done()): its length limit. */
	efr32_seq_stage(flags, efr32_packet_stage.max_len, &r, &r,
			stage->connect ? stage->adv_addr : NULL, stage);
}

/*
 * TIMER0 counts per millisecond of the scheduler clock, from the active provider's mapping (the
 * sequencer reads TIMER0, hal/efr32/seq_abi.h), 0 while the service is not on that provider.
 */
static uint32_t efr32_scan_t0_per_ms(void)
{
	static uint32_t per_ms;
	uint32_t now = k_cycle_get_32();
	uint32_t from;
	uint32_t to;

	if (per_ms == 0U && k_clock_event_sched_active_target(now, &from) == 0 &&
	    k_clock_event_sched_active_target(now + k_us_to_cyc_floor32(USEC_PER_MSEC), &to) == 0) {
		per_ms = to - from;
	}
	return per_ms;
}

static uint32_t efr32_scan_us_to_t0(uint32_t us, uint32_t per_ms)
{
	return (uint32_t)((uint64_t)us * per_ms / USEC_PER_MSEC);
}

/*
 * The event timer's start on TIMER0, for an initiator's slot check (EFR32_SEQ_CONN_INIT_TIMED);
 * when it cannot be mapped LATEST becomes 0 and nothing is answered.
 */
static void efr32_scan_start_write(void)
{
	uint32_t start;

	if (k_clock_event_sched_active_target(efr32_radio_start_cyc, &start) != 0) {
		*EFR32_SEQ_CONN_WORD(EFR32_SEQ_SCAN_LATEST) = 0U;
		return;
	}
	*EFR32_SEQ_CONN_WORD(EFR32_SEQ_SCAN_START) = start;
}

/*
 * The initiator's slot check on TIMER0. Without the provider's rate (not on it) LATEST is 0 and
 * the sequencer answers nothing, which isr_rx_pdu() then follows (lll/lll_scan_stage.c).
 */
static void efr32_scan_timing_write(const struct efr32_radio_scan_stage *stage)
{
	uint32_t per_ms = efr32_scan_t0_per_ms();
	/* RAC.STIMER ticks per millisecond (seq_abi.h gives them per 10 us). */
	uint32_t stimer_per_ms = EFR32_SEQ_RAC_STIMER_TICKS_PER_10US * (USEC_PER_MSEC / 10U);

	*EFR32_SEQ_CONN_WORD(EFR32_SEQ_SCAN_LATEST) = efr32_scan_us_to_t0(stage->latest_us, per_ms);
	*EFR32_SEQ_CONN_WORD(EFR32_SEQ_SCAN_PERIOD) = efr32_scan_us_to_t0(stage->period_us, per_ms);
	*EFR32_SEQ_CONN_WORD(EFR32_SEQ_SCAN_T0_PER_STIMER_Q8) =
		DIV_ROUND_CLOSEST(per_ms << EFR32_SEQ_SCAN_Q8_SHIFT, stimer_per_ms);
	*EFR32_SEQ_CONN_WORD(EFR32_SEQ_SCAN_EOF_TO_EXIT) =
		(uint32_t)((uint64_t)per_ms * CONFIG_EFR32_RADIO_RX_EOF_TO_EXIT_NS / NSEC_PER_MSEC);
	efr32_scan_start_write();
}

static void efr32_seq_addr_write(uint32_t offset, const uint8_t *addr)
{
	volatile uint8_t *a = (volatile uint8_t *)EFR32_SEQ_CONN_WORD(offset);

	for (uint32_t i = 0U; i < EFR32_SEQ_ADV_ADDR_LEN; i++) {
		a[i] = addr[i];
	}
}

static void efr32_seq_stage(uint32_t flags, uint8_t rx_max,
			    const struct efr32_radio_conn_reply *cur,
			    const struct efr32_radio_conn_reply *next, const uint8_t *adva,
			    const struct efr32_radio_scan_stage *scan)
{
	uintptr_t rx = EFR32_BUFC_BASE + EFR32_BUFC_RX_BASE_OFFSET;
	uintptr_t tx = EFR32_BUFC_BASE + EFR32_BUFC_TX_BASE_OFFSET;

	/* Withdraw a stage the sequencer has not taken before rewriting its images. */
	*EFR32_SEQ_CONN_WORD(EFR32_SEQ_CONN_STATE) = 0U;
	__DSB();
	efr32_conn_image_octets[0] = efr32_conn_image_build(efr32_conn_image[0], cur);
	efr32_conn_image_octets[1] = efr32_conn_image_build(efr32_conn_image[1], next);
	efr32_conn_flags = flags;
	*EFR32_SEQ_CONN_WORD(EFR32_SEQ_CONN_FLAGS) = efr32_conn_flags;
	if (adva != NULL) {
		efr32_seq_addr_write(EFR32_SEQ_CONN_ADVA, adva);
	}
	if (scan != NULL && scan->connect) {
		efr32_seq_addr_write(EFR32_SEQ_CONN_INITA, scan->init_addr);
		if (scan->timed) {
			efr32_scan_timing_write(scan);
		}
	}
	*EFR32_SEQ_CONN_WORD(EFR32_SEQ_CONN_RX_BASE) = (uint32_t)(uintptr_t)efr32_rx_fifo;
	/* Every earlier frame has been consumed: the next one starts at READOFFSET. */
	efr32_conn_rx_offset = *radio_reg32(rx + EFR32_BUFC_READOFFSET_OFFSET) %
			       sizeof(efr32_rx_fifo);
	*EFR32_SEQ_CONN_WORD(EFR32_SEQ_CONN_RX_OFFSET) = efr32_conn_rx_offset;
	*EFR32_SEQ_CONN_WORD(EFR32_SEQ_CONN_RX_MASK) = sizeof(efr32_rx_fifo) - 1U;
	*EFR32_SEQ_CONN_WORD(EFR32_SEQ_CONN_RX_MAX) = rx_max;
	for (uint32_t i = 0U; i < 2U; i++) {
		*EFR32_SEQ_CONN_WORD(i == 0U ? EFR32_SEQ_CONN_CUR : EFR32_SEQ_CONN_NEXT) =
			(uint32_t)(uintptr_t)efr32_conn_image[i];
		*EFR32_SEQ_CONN_WORD(i == 0U ? EFR32_SEQ_CONN_CUR_OCTETS :
					       EFR32_SEQ_CONN_NEXT_OCTETS) =
			efr32_conn_image_octets[i];
	}
	*EFR32_SEQ_CONN_WORD(EFR32_SEQ_CONN_OUTCOME) = 0U;
	/*
	 * The sequencer CLEARs the TX FIFO and writes the reply through WRITEDATA: the channel's
	 * size and address are those efr32_tx_fifo_publish() sets for every TX. Written only while
	 * no reply is on air (they never change once set).
	 */
	if (!efr32_conn_in_flight) {
		*radio_reg32(tx + EFR32_BUFC_CFG_OFFSET) =
			EFR32_BUFC_FIFO_CONFIG(sizeof(efr32_tx_fifo));
		*radio_reg32(tx + EFR32_BUFC_ADDR_OFFSET) = (uint32_t)(uintptr_t)efr32_tx_fifo;
	}
	efr32_conn_generation++;
	__DSB();
	*EFR32_SEQ_CONN_WORD(EFR32_SEQ_CONN_STATE) =
		((uint32_t)efr32_conn_generation << EFR32_SEQ_CONN_GENERATION_SHIFT) |
		EFR32_SEQ_CONN_STAGED;
	efr32_conn_staged = true;
#if defined(CONFIG_EFR32_RADIO_TIMING_PROBE)
	/* Staged by the completion committing a reply (not by a prepare): its path. */
	if (efr32_conn_in_flight) {
		EFR32_PROBE_PATH(to_staged);
	}
#endif
}

int efr32_radio_conn_outcome(uint32_t *outcome)
{
	uint32_t start = k_cycle_get_32();
	uint32_t from = efr32_radio_rx_eof_valid ? efr32_radio_rx_eof_cyc : start;
	uint32_t word;
	bool valid;

	if (!efr32_conn_received) {
		return 0;
	}
	efr32_conn_received = false;
	/*
	 * The sequencer decides as soon as RX2TX begins (EFR32_RADIO_RX_EOF_TO_EXIT_NS after the
	 * frame's end) and in any case before its RX2TX hold ends (EFR32_SEQ_RX2TX_TX_AT_100NS
	 * later), when it has either sent the reply or nothing: no outcome by then means none.
	 */
	do {
		word = *EFR32_SEQ_CONN_WORD(EFR32_SEQ_CONN_OUTCOME);
		valid = (word & EFR32_SEQ_CONN_OUT_VALID) != 0U &&
			(word >> EFR32_SEQ_CONN_GENERATION_SHIFT) == efr32_conn_generation;
	} while (!valid && k_cycle_get_32() - from <=
		 k_ns_to_cyc_ceil32(CONFIG_EFR32_RADIO_RX_EOF_TO_EXIT_NS_MAX +
				    EFR32_SEQ_RX2TX_TX_AT_100NS * 100U));
#if defined(CONFIG_EFR32_RADIO_TIMING_PROBE)
	EFR32_PROBE_MAX(conn_outcome_wait, k_cycle_get_32() - start);
	if (valid && efr32_radio_rx_eof_valid) {
		EFR32_PROBE_MAX(conn_outcome_after_eof, k_cycle_get_32() - from);
	}
	if (valid) {
		EFR32_PROBE_MAX(conn_taken_at, *EFR32_SEQ_CONN_WORD(EFR32_SEQ_CONN_TAKEN_AT) &
						      EFR32_RAC_STIMER_MASK);
	}
#endif
	if (!valid) {
		efr32_conn_in_flight = false;
		return -1;
	}
	efr32_conn_outcome_word = word;
	efr32_conn_committing = (word & EFR32_SEQ_CONN_OUT_REPLY) != 0U;
	if (!efr32_conn_committing) {
		efr32_conn_in_flight = false;
	}
	*outcome = word;
	return 1;
}

bool efr32_radio_conn_continues(void)
{
	return (efr32_conn_outcome_word & (EFR32_SEQ_CONN_OUT_REPLY | EFR32_SEQ_CONN_OUT_DONE)) ==
	       EFR32_SEQ_CONN_OUT_REPLY;
}

/*
 * The SCAN_REQ isr_rx_pdu() committed is the one the sequencer sent: the staged one with RxAdd
 * the received frame's TxAdd and AdvA the frame's (still in the RX FIFO where it was staged).
 */
static void efr32_scan_req_check(const uint8_t *pdu)
{
	const uint8_t *image = efr32_conn_image[0];
	uint8_t rx_header = efr32_rx_fifo[efr32_conn_rx_offset];
	uint8_t rxadd = BIT(EFR32_SEQ_ADV_RXADD_BIT);

	LL_ASSERT((pdu[0] & ~rxadd) == (image[0] & ~rxadd));
	LL_ASSERT(((pdu[0] >> EFR32_SEQ_ADV_RXADD_BIT) & 1U) ==
		  ((rx_header >> EFR32_SEQ_ADV_TXADD_BIT) & 1U));
	LL_ASSERT(memcmp(pdu + 1, image + 1, EFR32_SEQ_SCAN_REQ_ADVA_OCTET - 1) == 0);
	for (uint32_t i = 0U; i < EFR32_SEQ_ADV_ADDR_LEN; i++) {
		LL_ASSERT(pdu[EFR32_SEQ_SCAN_REQ_ADVA_OCTET + i] ==
			  efr32_rx_fifo[(efr32_conn_rx_offset + PDU_AC_LL_HEADER_SIZE + i) %
					sizeof(efr32_rx_fifo)]);
	}
}

/* The SN and NESN octet the sequencer put on air for the reply being committed. */
static uint8_t efr32_conn_sent_seq_bits(void)
{
	uint32_t ack = (efr32_conn_outcome_word >> EFR32_SEQ_CONN_OUT_ACK_SHIFT) & 1U;
	uint32_t accept = (efr32_conn_outcome_word >> EFR32_SEQ_CONN_OUT_ACCEPT_SHIFT) & 1U;
	uint32_t sn = ((efr32_conn_flags >> EFR32_SEQ_CONN_SN_SHIFT) & 1U) ^ ack;
	uint32_t nesn = ((efr32_conn_flags >> EFR32_SEQ_CONN_NESN_SHIFT) & 1U) ^ accept;

	return (uint8_t)((sn << EFR32_SEQ_PDU_SN_BIT) | (nesn << EFR32_SEQ_PDU_NESN_BIT));
}

/* Forget every stage and outcome (radio_disable(), radio_reset(), radio_tmr_stop()). */
static void efr32_conn_stage_reset(void)
{
	*EFR32_SEQ_CONN_WORD(EFR32_SEQ_CONN_STATE) = 0U;
	efr32_conn_staged = false;
	efr32_conn_received = false;
	efr32_conn_in_flight = false;
	efr32_conn_committing = false;
	efr32_conn_outcome_word = 0U;
	efr32_conn_reply_header = -1;
	efr32_init_sent = false;
}
#else
static inline void efr32_conn_stage_reset(void)
{
}
#define efr32_conn_staged false
#define efr32_conn_in_flight false
#define efr32_conn_committing false
#endif /* CONFIG_EFR32_RADIO_CONN_STAGED */

/*
 * The FRC line is level-sensitive while IF.RXDONE/TXDONE remains set.  Mask
 * only those two sources after publishing their callback so IRQ exit does not
 * starve the scheduler job.  The job leaves the status visible to the OpenISA
 * callback, then re-enables the sources after that callback has had its
 * normal radio_status_reset() opportunity.
 */
static void efr32_radio_completion_window(bool abort, uint32_t now, int *deadline,
					  uint32_t *budget);
static void efr32_ccm_rx_complete(void);

static void efr32_radio_completion_publish(radio_isr_cb_t callback, void *param,
						   bool abort)
{
	uint32_t now = k_cycle_get_32();
	uint32_t budget;
	int deadline;

	if (atomic_get(&efr32_radio_halted) != 0) {
		return;
	}
	if (!efr32_radio_completion_dispatch_queue(&efr32_radio_completion_dispatch,
						      efr32_radio_completion_dispatch.generation)) {
		return;
	}

	efr32_radio_completion_work.callback = callback;
	efr32_radio_completion_work.param = param;
	efr32_radio_completion_work.abort = abort;
	/* The kind of completion decides what the callback serves, hence its deadline and budget. */
	efr32_radio_completion_window(abort, now, &deadline, &budget);
	k_deadline_event_post_budget(&efr32_radio_completion_event, deadline, budget);
}

static void efr32_radio_completion_job(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	for (;;) {
		radio_isr_cb_t callback = NULL;
		void *param = NULL;
		bool abort = false;
#if defined(CONFIG_EFR32_RADIO_TIMING_PROBE)
		uint8_t probe_kind = 0U;
		bool probe_conn = false;
#endif
		unsigned int key;

		k_deadline_event_wait(&efr32_radio_completion_event);
#if defined(CONFIG_EFR32_RADIO_TIMING_PROBE)
		efr32_radio_probe_job_start = k_cycle_get_32();
		efr32_radio_probe.samples++;
#endif
		key = irq_lock();
		if (efr32_radio_completion_dispatch_take(&efr32_radio_completion_dispatch)) {
			callback = efr32_radio_completion_work.callback;
			param = efr32_radio_completion_work.param;
			abort = efr32_radio_completion_work.abort;
#if defined(CONFIG_EFR32_RADIO_TIMING_PROBE)
			/*
			 * The run is recorded under the kind it was published as: an abort the
			 * callback itself raises (radio_disable() in the LLL's isr_rx) republishes
			 * the mailbox while this run is still going.
			 */
			probe_kind = efr32_radio_completion_work.kind;
			probe_conn = efr32_radio_completion_work.conn;
#endif
		}
		irq_unlock(key);

		if (!callback) {
			continue;
		}
		/* The radio completed: the timed command that started it has been acted on. */
		efr32_seq_timed_check(&efr32_radio_tx_event, "efr32 sequencer timed command");

#if defined(CONFIG_EFR32_RADIO_QEMU_TIFS)
		/*
		 * A scan prepare may have recorded a next TX before RX began.  Consume
		 * that request before the LLL clears RX completion state, but leave the
		 * callback time to stage its response PDU before the TIFS worker TXEN.
		 */
		if (!abort && efr32_packet_stage.lifecycle.completion_direction ==
			EFR32_RADIO_PACKET_RX) {
			efr32_qemu_tifs_begin_rx_to_tx();
		}
#endif
		bool rx_cb = !abort && efr32_packet_stage.lifecycle.completion_direction ==
					EFR32_RADIO_PACKET_RX;

		if (rx_cb) {
			EFR32_RX2TX_M33_STAMP(1);
			/* An encrypted receive is decrypted before the LLL reads it. */
			efr32_ccm_rx_complete();
		}
		callback(param);
		if (rx_cb) {
			EFR32_RX2TX_M33_STAMP(3);
		}
#if defined(CONFIG_EFR32_RADIO_QEMU_TIFS)
		if (!abort) {
			efr32_qemu_tifs_arm_staged_rx_to_tx();
		}
#endif
		if (abort) {
			key = irq_lock();
			efr32_radio_abort_finish(&efr32_radio_abort);
			irq_unlock(key);
		} else {
			/* A queued RXDONE/TXDONE is still visible until the LLL consumes it. */
			*radio_reg32(EFR32_FRC_BASE + EFR32_REGISTER_SET_OFFSET +
				     EFR32_FRC_IEN_OFFSET) = EFR32_FRC_IF_COMPLETION;
		}
		EFR32_PROBE_MAX(completion_run[probe_conn ? 1 : 0][probe_kind],
				k_cycle_get_32() - efr32_radio_probe_job_start);
	}
}


/* [software] lll_init() connects LL_RADIO_IRQn (FRC_IRQn) to this entry. */
#if !defined(CONFIG_EFR32_RADIO_QEMU_RX_DESCRIPTOR)
static int efr32_packet_stage_rx_complete(const uint8_t *pdu, uint16_t bytes);

/*
 * Hardware RX handoff (docs/radio-tables.md SS7.66).  Measured on silicon:
 * FRC writes a received frame into BUFC RX channel 1 as header octet, length
 * octet, then exactly that many payload octets -- WRITEOFFSET advanced by
 * length + 2 for both a 31- and a 37-octet PDU -- with no trailer; CRC status
 * is FRC.TRAILRXDATA.  Hand the frame at READOFFSET to the LLL and consume
 * it.  Each RX window starts from a CLEARed channel, so a frame never wraps
 * in practice; one that would is rejected rather than reassembled.
 */
static void efr32_radio_rx_end_capture(void);

static void efr32_hw_rx_deliver(void)
{
	uintptr_t base = EFR32_BUFC_BASE + EFR32_BUFC_RX_BASE_OFFSET;
	uint32_t read = *radio_reg32(base + EFR32_BUFC_READOFFSET_OFFSET) %
			sizeof(efr32_rx_fifo);
	uint32_t write = *radio_reg32(base + EFR32_BUFC_WRITEOFFSET) %
			 sizeof(efr32_rx_fifo);
	uint32_t available = (write + sizeof(efr32_rx_fifo) - read) %
			     sizeof(efr32_rx_fifo);
	uint32_t bytes;

	if (available < 2U || read + 2U > sizeof(efr32_rx_fifo)) {
		efr32_rx_delivery_invalid = true;
		return;
	}
	bytes = (uint32_t)efr32_rx_fifo[read + 1U] + 2U;
	if (bytes + EFR32_FRC_TRAILER_OCTETS > available ||
	    read + bytes + EFR32_FRC_TRAILER_OCTETS > sizeof(efr32_rx_fifo)) {
		efr32_rx_delivery_invalid = true;
		return;
	}
	efr32_rx_crc_ok = !!(efr32_rx_fifo[read + bytes] & EFR32_FRC_TRAILER_CRCOK_BIT);
#if defined(CONFIG_EFR32_RADIO_RX_CHANNEL_STATS)
	efr32_radio_rx_channel_stats[efr32_radio_channel_index][efr32_rx_crc_ok ? 1 : 0]++;
#endif
#if defined(CONFIG_EFR32_RADIO_BRINGUP_TRACE)
	{
		/* Diagnostic only, in spare SEQRAM: CRC-ok/CRC-bad frame counts,
		 * then the last SCAN_REQ-typed frame's first 16 octets and its
		 * TRAILRXDATA. */
		volatile uint32_t *diag = radio_reg32(0xB0004F00UL);
		uint32_t trail = *radio_reg32(EFR32_FRC_BASE +
					      EFR32_FRC_TRAILRXDATA_OFFSET);

		ARG_UNUSED(trail);
		diag[efr32_rx_crc_ok ? 0 : 1]++;
		if ((efr32_rx_fifo[read] & 0x0FU) == 0x03U) {
			for (uint32_t i = 0U; i < 4U; i++) {
				diag[2U + i] = (uint32_t)efr32_rx_fifo[read + 4U * i] |
					((uint32_t)efr32_rx_fifo[read + 4U * i + 1U] << 8) |
					((uint32_t)efr32_rx_fifo[read + 4U * i + 2U] << 16) |
					((uint32_t)efr32_rx_fifo[read + 4U * i + 3U] << 24);
			}
			diag[6] = trail;
		}
	}
#endif
	efr32_radio_rx_pdu_octets = (uint16_t)bytes;
	(void)efr32_packet_stage_rx_complete(&efr32_rx_fifo[read], (uint16_t)bytes);
	*radio_reg32(base + EFR32_BUFC_READOFFSET_OFFSET) =
		(read + bytes + EFR32_FRC_TRAILER_OCTETS) % sizeof(efr32_rx_fifo);
}
#endif

void isr_radio(void *arg)
{
	uint32_t pending;
	uint32_t expected = 0U;
	enum efr32_radio_packet_direction completion_direction = EFR32_RADIO_PACKET_IDLE;
	radio_isr_cb_t abort_callback;
	void *abort_param;

	ARG_UNUSED(arg);
	/*
	 * RXDIS/TXDIS do not raise FRC.RXDONE/TXDONE.  Deliver their explicitly
	 * requested cleanup callback without pretending an aborted PDU completed.
	 * radio_isr_set() clears this flag together with a pended FRC IRQ, so this
	 * cannot be delivered to a handler installed for a later event.
	 */
	if (efr32_radio_abort_take(&efr32_radio_abort, &abort_callback, &abort_param)) {
		/*
		 * A completion can race the RXDIS/TXDIS commands after
		 * radio_disable() has cleared the pre-abort latches.  The pending
		 * software abort is terminal for that LLL event, so acknowledge such
		 * a late completion before handing control to its cleanup callback.
		 * Otherwise the shared, level-sensitive FRC IRQ can immediately
		 * re-enter the same callback and make an aborted PDU look completed.
		 */
		*radio_reg32(EFR32_FRC_BASE + EFR32_REGISTER_CLEAR_OFFSET +
			     EFR32_FRC_IF_OFFSET) = EFR32_FRC_IF_COMPLETION;
		efr32_radio_completion_publish(abort_callback, abort_param, true);
		return;
	}

	pending = *radio_reg32(EFR32_FRC_BASE + EFR32_FRC_IF_OFFSET) &
		  EFR32_FRC_IF_COMPLETION;

	/*
	 * TXDONE and RXDONE share FRC_IRQn, but their OpenISA callbacks have
	 * different contracts. The LLL TX callbacks do not re-check radio_is_done(),
	 * so never let an RX completion advance a TX state machine (or converse).
	 * active_direction is a software routing label; it does not assign how the
	 * hardware produces either latch.
	 */
	if (efr32_packet_stage.lifecycle.active_direction == EFR32_RADIO_PACKET_TX) {
		expected = EFR32_FRC_IF_TXDONE;
		completion_direction = EFR32_RADIO_PACKET_TX;
	} else if (efr32_packet_stage.lifecycle.active_direction == EFR32_RADIO_PACKET_RX) {
		expected = EFR32_FRC_IF_RXDONE;
		completion_direction = EFR32_RADIO_PACKET_RX;
	}

#if defined(CONFIG_EFR32_RADIO_QEMU_RX_DESCRIPTOR)
	/*
	 * The opt-in QEMU seam publishes its descriptor before retaining the
	 * synthetic RXDONE latch. If BUFC is still pending, hold FRC completion
	 * masked until the scheduler worker has copied/rejected that record. This
	 * preserves the existing LLL callback ordering without assigning that
	 * ordering to real EFR32 hardware.
	 */
	if (completion_direction == EFR32_RADIO_PACKET_RX &&
	    (pending & EFR32_FRC_IF_RXDONE) &&
	    efr32_qemu_rx_descriptor_pending()) {
		*radio_reg32(EFR32_FRC_BASE + EFR32_REGISTER_CLEAR_OFFSET +
			EFR32_FRC_IEN_OFFSET) = EFR32_FRC_IF_RXDONE;
		efr32_qemu_rx_descriptor_release();
		return;
	}
#endif

	/* A stale or wrong-direction completion must not leave FRC_IRQn asserted. */
	if ((pending & ~expected) || (!efr32_radio_abort.callback && pending)) {
		*radio_reg32(EFR32_FRC_BASE + EFR32_REGISTER_CLEAR_OFFSET +
			     EFR32_FRC_IF_OFFSET) = !efr32_radio_abort.callback ? pending : pending & ~expected;
	}

	if (efr32_radio_abort.callback && (pending & expected)) {
		/*
		 * Keep the completion visible while the callback samples status.  It
		 * is retired by radio_status_reset(), the LLL's explicit status
		 * consumption boundary, rather than by this IRQ entry.
		 */
#if !defined(CONFIG_EFR32_RADIO_QEMU_RX_DESCRIPTOR)
		if (completion_direction == EFR32_RADIO_PACKET_RX) {
			EFR32_RX2TX_M33_STAMP(0);
			efr32_hw_rx_deliver();
			efr32_radio_rx_end_capture();
		}
#endif
#if defined(CONFIG_EFR32_RADIO_CONN_STAGED)
		/*
		 * A staged receive: the sequencer answers it on its own, so its reply may be on air
		 * from now until that reply's TXDONE (efr32_radio_conn_outcome() learns which).
		 */
		if (completion_direction == EFR32_RADIO_PACKET_RX && efr32_conn_staged) {
			efr32_conn_staged = false;
			efr32_conn_received = true;
			efr32_conn_in_flight = true;
		} else if (completion_direction == EFR32_RADIO_PACKET_TX) {
			efr32_conn_in_flight = false;
		}
#endif
		LL_ASSERT(efr32_packet_lifecycle_latch_completion(
			&efr32_packet_stage.lifecycle, completion_direction));
		/* Do not run an LLL state machine on the hardware interrupt stack. */
		*radio_reg32(EFR32_FRC_BASE + EFR32_REGISTER_CLEAR_OFFSET +
			     EFR32_FRC_IEN_OFFSET) = EFR32_FRC_IF_COMPLETION;
		efr32_radio_completion_publish(efr32_radio_abort.callback,
					       efr32_radio_abort.param, false);
	}
}

/*
 * The generic OpenISA LLL owns the FRC_IRQn connection and enable.  It can
 * replace the callback while it advances a radio event, so make the callback
 * and its parameter an interrupt-atomic pair and discard a completion that
 * belonged to the old handler.  This is the single-IRQ equivalent of the
 * RV32M1 HAL's IRQ disable, callback update, pending-event clear, and IRQ
 * restore sequence.
 */
void radio_isr_set(radio_isr_cb_t cb, void *param)
{
	unsigned int key = irq_lock();

	(void)efr32_radio_completion_dispatch_replace(&efr32_radio_completion_dispatch);
	efr32_radio_abort_replace(&efr32_radio_abort, cb, param);
	efr32_packet_lifecycle_forget_completion(&efr32_packet_stage.lifecycle);
	*radio_reg32(EFR32_FRC_BASE + EFR32_REGISTER_CLEAR_OFFSET +
		     EFR32_FRC_IF_OFFSET) = EFR32_FRC_IF_COMPLETION;
	/* Replacement also cancels a deferred callback that had masked this line. */
	*radio_reg32(EFR32_FRC_BASE + EFR32_REGISTER_SET_OFFSET +
		     EFR32_FRC_IEN_OFFSET) = EFR32_FRC_IF_COMPLETION;
	/* FRC's line can remain pended after its source was acknowledged. */
	k_irq_clear_pending(LL_RADIO_IRQn);

	irq_unlock(key);
}

/*
 * A reset/startup boundary has no successor LLL event.  Unlike radio_disable(),
 * it must not dispatch the preceding event's cleanup callback.  Quiesce the
 * software callback and both completion-pending paths as one IRQ-atomic
 * transaction, so a retained FRC completion cannot be delivered to it.
 */
static void efr32_radio_completion_quiesce(void)
{
	unsigned int key = irq_lock();

	efr32_radio_abort_cancel(&efr32_radio_abort);
	(void)efr32_radio_completion_dispatch_replace(&efr32_radio_completion_dispatch);
	efr32_radio_completion_work.callback = NULL;
	efr32_radio_completion_work.param = NULL;
	*radio_reg32(EFR32_FRC_BASE + EFR32_REGISTER_CLEAR_OFFSET +
		     EFR32_FRC_IF_OFFSET) = EFR32_FRC_IF_COMPLETION;
	k_irq_clear_pending(LL_RADIO_IRQn);
	#if defined(CONFIG_EFR32_RADIO_QEMU_RX_DESCRIPTOR)
	efr32_radio_qemu_rx_descriptor_quiesce();
	#endif

	irq_unlock(key);
}

/*
 * Hardware TX->RX turnaround (docs/radio-tables.md SS7.63).  On silicon the
 * RAC turns around by itself when an RX request is already pending as the
 * frame ends: stock's RAC.STATUS reads RXENS with the SWRXEN source during
 * TXWARM and TX, then passes TX2RX into RXSEARCH, where the open sequencer
 * retunes and powers the receiver.  The LLL's switch request is therefore
 * turned into SWRXEN right after TXEN, and the RX packet stage is armed once
 * the LLL has consumed the TX completion and staged its RX buffer.
 */
static bool efr32_hw_rx_after_tx_requested;
static bool efr32_hw_rx_after_tx_pending;
static bool efr32_hw_rx_turnaround_claimed;
/*
 * RX->TX (e.g. SCAN_RSP) is not implemented on hardware yet.  When the LLL
 * has asked for one and a frame was received, end the event after this bound
 * instead of leaving the LLL waiting for a TX completion that cannot come.
 */
static bool efr32_hw_tx_after_rx_requested;
/*
 * Hardware RX->TX (SS7.65).  [I] Stock raises a TX request while a frame is
 * still arriving (RAC.STATUS.TXENS during RXFRAME), so the RAC goes RX2TX at
 * frame end.  Here the M33 marks the RX window PENDING; the open sequencer
 * issues TXEN from its RXFRAME handler only then, and its RX2TX handler waits
 * for the LLL's decision: RESPOND once the response PDU is in BUFC, CANCEL
 * when the LLL disables instead.
 */
static bool efr32_hw_rx2tx_armed;

#define EFR32_RAC_STATE_RXFRAME 3U
#define EFR32_RAC_STATE_RX2TX 7U
/* RAC.STIMER ticks in @p ns, rounded up / down. */
#define EFR32_STIMER_TICKS_CEIL(ns) \
	DIV_ROUND_UP((uint32_t)(ns) * EFR32_SEQ_RAC_STIMER_TICKS_PER_10US, 10000U)
#define EFR32_STIMER_TICKS_FLOOR(ns) \
	((uint32_t)(ns) * EFR32_SEQ_RAC_STIMER_TICKS_PER_10US / 10000U)

/* RAC state, RAC.STIMER (ticks since that state began) and the scheduler clock, read together. */
static bool efr32_rac_stimer_in(uint32_t state, uint32_t *stimer, uint32_t *now)
{
	uint32_t before = (*radio_reg32(EFR32_RAC_BASE + EFR32_RAC_STATUS_OFFSET) >>
			   EFR32_RAC_STATUS_STATE_SHIFT) & EFR32_RAC_STATUS_STATE_MASK;
	uint32_t after;

	*stimer = *radio_reg32(EFR32_RAC_BASE + EFR32_RAC_STIMER_OFFSET) & EFR32_RAC_STIMER_MASK;
	*now = k_cycle_get_32();
	after = (*radio_reg32(EFR32_RAC_BASE + EFR32_RAC_STATUS_OFFSET) >>
		 EFR32_RAC_STATUS_STATE_SHIFT) & EFR32_RAC_STATUS_STATE_MASK;
	return before == state && after == state;
}

/*
 * Timestamp the frame just received. A window armed for a response (every receive in a
 * connection event, and an advertiser's CONNECT_IND) holds RX2TX, which the RAC entered
 * EFR32_RADIO_RX_EOF_TO_EXIT_NS after the frame's MODEM EOF and which RAC.STIMER counts from
 * (docs/radio-tables.md 7.68, 7.72): the EOF is that much before now. Another state means the
 * frame end is no longer visible; the timers then report none (radio_tmr_end_get()).
 */
static void efr32_radio_rx_end_capture(void)
{
	uint32_t start = k_cycle_get_32();
	uint32_t stimer;
	uint32_t now;

	/*
	 * RXDONE is raised at the EOF and the RAC leaves RXFRAME EFR32_RADIO_RX_EOF_TO_EXIT_NS
	 * later, so an interrupt taken at once can find it still in RXFRAME: wait out that
	 * measured exit, no longer.
	 */
	while (((*radio_reg32(EFR32_RAC_BASE + EFR32_RAC_STATUS_OFFSET) >>
		 EFR32_RAC_STATUS_STATE_SHIFT) & EFR32_RAC_STATUS_STATE_MASK) ==
	       EFR32_RAC_STATE_RXFRAME &&
	       k_cycle_get_32() - start <=
	       k_ns_to_cyc_ceil32(CONFIG_EFR32_RADIO_RX_EOF_TO_EXIT_NS_MAX)) {
	}
	efr32_radio_rx_eof_valid = efr32_rac_stimer_in(EFR32_RAC_STATE_RX2TX, &stimer, &now);
	if (efr32_radio_rx_eof_valid) {
		efr32_radio_rx_eof_cyc =
			now - k_ns_to_cyc_floor32(stimer * 10000U /
						  EFR32_SEQ_RAC_STIMER_TICKS_PER_10US) -
			k_ns_to_cyc_floor32(CONFIG_EFR32_RADIO_RX_EOF_TO_EXIT_NS);
	}
}
/*
 * Latest RAC.STIMER count (since RX2TX began, i.e. since the received frame's end) at which
 * RESPOND may be written: the sequencer reaches its hold the measured RESPOND-to-TX-ready time
 * after the write, and TX then starts at the hold, at our measured T_IFS; a later write delays
 * TX past it, which the active clock's 2 us allows, less the spread of our T_IFS already
 * measured on air (section 7.71).
 */
#define EFR32_RX2TX_RESPOND_BY_STIMER \
	(EFR32_SEQ_RX2TX_TX_AT_STIMER - \
	 EFR32_STIMER_TICKS_CEIL(CONFIG_EFR32_RADIO_SEQ_RESPOND_TO_TX_READY_NS) + \
	 EFR32_STIMER_TICKS_FLOOR(BT_ACTIVE_CLOCK_JITTER_US * 1000U - \
				  CONFIG_EFR32_RADIO_TIFS_SPREAD_NS))
BUILD_ASSERT(CONFIG_EFR32_RADIO_TIFS_SPREAD_NS < BT_ACTIVE_CLOCK_JITTER_US * 1000U,
	     "the measured T_IFS spread already exceeds the active clock's tolerance");

/*
 * Start-by of work whose output is the next packet of the current radio event, @p path_ns from
 * its start to that packet's TXEN or RXEN at the latest (hal/efr32/ll_deadlines.h). In a
 * connection event that work closes the event and serves the connection's next event.
 */
static int efr32_radio_next_start_by(uint32_t now, uint32_t path_ns)
{
	if (efr32_radio_conn_interval_us != 0U) {
		return efr32_dl_conn_close_start_by(efr32_radio_launch_cyc,
						    efr32_radio_conn_interval_us, path_ns);
	}
	return efr32_radio_event_tx ? efr32_dl_adv_next_start_by(efr32_radio_last_txen, path_ns) :
				      efr32_dl_scan_next_start_by(now, path_ns);
}

/*
 * Measured run of each completion kind (Kconfig): connection events run lll_conn.c's callbacks,
 * advertising and scanning their own.
 */
/* An encrypted connection's RX->TX callback also runs CCM (include/hal/efr32/ll_deadlines.h). */
#define EFR32_CONN_CCM_TX_RX_NS 0
#define EFR32_CONN_CCM_RX_TX_NS EFR32_DL_CCM_RX_TX_NS
#define EFR32_COMPLETION_NS(conn, kind) \
	((conn) ? (CONFIG_EFR32_RADIO_WCET_CONN_COMPLETION_##kind##_NS + \
		   EFR32_CONN_CCM_##kind##_NS) : \
		  CONFIG_EFR32_RADIO_WCET_COMPLETION_##kind##_NS)

/*
 * Deadline and budget of the completion published at @p now:
 * - TX whose RX follows (ADV_IND of a scannable advertiser; in a connection event, a packet
 *   with more to come): mark the RX->TX window before the earliest reply is recognised
 *   (efr32_dl_completion_tx_rx_start_by()).
 * - RX with a response armed, the RAC holding in RX2TX (SCAN_REQ; every receive in a connection
 *   event): write RESPOND by EFR32_RX2TX_RESPOND_BY_STIMER, read against RAC.STIMER, which
 *   counts from the frame's end.
 * - Every other kind (RX without response, last TX, abort) may start the event's next packet,
 *   or closes a connection event.
 * The QEMU TIFS seam starts its T_IFS delay only after the callback, so with it neither
 * turnaround's callback has a T_IFS deadline of its own: they take the last rule
 * (docs/radio-tables.md 7.77).
 */
static void efr32_radio_completion_window(bool abort, uint32_t now, int *deadline,
					  uint32_t *budget)
{
	enum efr32_radio_packet_direction direction =
		efr32_packet_stage.lifecycle.completion_direction;
	bool rx_follows = efr32_hw_rx_after_tx_pending;
	bool conn = efr32_radio_conn_interval_us != 0U;
	uint32_t wcet_ns;

	efr32_radio_completion_work.conn = conn;
	efr32_radio_completion_work.kind =
		abort ? EFR32_COMPLETION_ABORT :
		(direction == EFR32_RADIO_PACKET_TX) ?
			((rx_follows && !IS_ENABLED(CONFIG_EFR32_RADIO_QEMU_TIFS)) ?
				EFR32_COMPLETION_TX_RX : EFR32_COMPLETION_TX) :
		efr32_hw_rx2tx_armed ? EFR32_COMPLETION_RX_TX : EFR32_COMPLETION_RX;
#if defined(CONFIG_EFR32_RADIO_CONN_STAGED)
	/*
	 * Staged connection replies (7.82): the sequencer answers at T_IFS; the receive's
	 * completion commits and stages before the next exchange needs it, and a transmit's
	 * that turns to the next receive ends before that receive's completion is due.
	 */
	if (!abort && conn && direction == EFR32_RADIO_PACKET_RX && efr32_conn_received) {
		uint32_t eof = efr32_radio_rx_eof_valid ? efr32_radio_rx_eof_cyc :
			now - EFR32_DL_MEAS_CYC(CONFIG_EFR32_RADIO_FRAME_END_TO_COMPLETION_POST_NS) -
			EFR32_DL_MEAS_CYC(CONFIG_EFR32_RADIO_INTERRUPT_BLOCKING_NS);

		*budget = EFR32_DL_MEAS_CYC(EFR32_COMPLETION_NS(conn, RX_TX));
		*deadline = efr32_dl_conn_rx_staged_start_by(
			eof, CONFIG_EFR32_RADIO_CONN_COMPLETION_TO_RESPOND_NS + EFR32_DL_CCM_RX_NS,
			CONFIG_EFR32_RADIO_CONN_COMPLETION_TO_STAGED_NS + EFR32_DL_CCM_RX_TX_NS);
		return;
	}
	/*
	 * A scanner's or initiator's staged receive (lll_scan.c, 7.87): the sequencer may have
	 * answered. An active scanner's commit re-arms the RX FIFO and stages the SCAN_RSP
	 * receive before that exchange needs them; an initiator's lets the connection be set up
	 * before its first anchor. Without a reply the completion needs no more time than that.
	 */
	if (!abort && !conn && direction == EFR32_RADIO_PACKET_RX && efr32_conn_received &&
	    (efr32_conn_flags & EFR32_SEQ_CONN_SCAN) != 0U) {
		uint32_t eof = efr32_radio_rx_eof_valid ? efr32_radio_rx_eof_cyc :
			now - EFR32_DL_MEAS_CYC(CONFIG_EFR32_RADIO_FRAME_END_TO_COMPLETION_POST_NS) -
			EFR32_DL_MEAS_CYC(CONFIG_EFR32_RADIO_INTERRUPT_BLOCKING_NS);

		efr32_radio_completion_work.kind = EFR32_COMPLETION_RX_TX;
		*budget = EFR32_DL_MEAS_CYC(CONFIG_EFR32_RADIO_WCET_SCAN_COMMIT_NS);
		*deadline = ((efr32_conn_flags & EFR32_SEQ_CONN_INIT) != 0U) ?
			efr32_dl_init_commit_start_by(eof, CONFIG_EFR32_RADIO_WCET_SCAN_COMMIT_NS) :
			efr32_dl_rx_staged_start_by(eof, BT_LE_1M_SCAN_REQ_US,
						    CONFIG_EFR32_RADIO_SCAN_COMMIT_TO_RESPOND_NS,
						    CONFIG_EFR32_RADIO_SCAN_COMMIT_TO_STAGED_NS);
		return;
	}
	/*
	 * The SCAN_REQ the sequencer sent turns to the staged SCAN_RSP receive: this completion
	 * ends before that receive's completion is due.
	 */
	if (!abort && !conn && direction == EFR32_RADIO_PACKET_TX && rx_follows &&
	    efr32_conn_staged && (efr32_conn_flags & EFR32_SEQ_CONN_SCAN_REQ) != 0U) {
		*budget = EFR32_DL_MEAS_CYC(CONFIG_EFR32_RADIO_WCET_COMPLETION_TX_RX_NS);
		*deadline = efr32_dl_tx_staged_start_by(now,
							CONFIG_EFR32_RADIO_WCET_COMPLETION_TX_RX_NS,
							BT_LE_1M_SCAN_REQ_US,
							CONFIG_EFR32_RADIO_SCAN_COMMIT_TO_RESPOND_NS,
							CONFIG_EFR32_RADIO_SCAN_COMMIT_TO_STAGED_NS);
		return;
	}
	/* The CONNECT_IND the sequencer sent: the connection is set up before its first anchor. */
	if (!abort && direction == EFR32_RADIO_PACKET_TX && efr32_init_sent) {
		efr32_init_sent = false;
		*budget = EFR32_DL_MEAS_CYC(CONFIG_EFR32_RADIO_WCET_COMPLETION_TX_NS);
		*deadline = efr32_dl_init_tx_start_by(now, CONFIG_EFR32_RADIO_WCET_COMPLETION_TX_NS);
		return;
	}
	/*
	 * An advertiser's staged SCAN_REQ (lll_adv.c): the sequencer answers it; the event's next
	 * packet is what this completion still serves.
	 */
	if (!abort && !conn && direction == EFR32_RADIO_PACKET_RX && efr32_conn_received) {
		efr32_radio_completion_work.kind = EFR32_COMPLETION_RX_TX;
		*budget = EFR32_DL_MEAS_CYC(CONFIG_EFR32_RADIO_WCET_COMPLETION_RX_TX_NS);
		*deadline = efr32_radio_next_start_by(now,
						      CONFIG_EFR32_RADIO_WCET_COMPLETION_RX_TX_NS);
		return;
	}
	if (!abort && conn && direction == EFR32_RADIO_PACKET_TX && rx_follows) {
		*budget = EFR32_DL_MEAS_CYC(EFR32_COMPLETION_NS(conn, TX_RX));
		*deadline = efr32_dl_conn_tx_staged_start_by(
			now, EFR32_COMPLETION_NS(conn, TX_RX),
			CONFIG_EFR32_RADIO_CONN_COMPLETION_TO_RESPOND_NS + EFR32_DL_CCM_RX_NS,
			CONFIG_EFR32_RADIO_CONN_COMPLETION_TO_STAGED_NS + EFR32_DL_CCM_RX_TX_NS);
		return;
	}
#endif
#if defined(CONFIG_EFR32_RADIO_AMATEUR)
	/*
	 * The end of a Morse identification: no link-layer event follows inside its ticker slot,
	 * which reserves EVENT_OVERHEAD_XTAL_US + EVENT_OVERHEAD_END_US after the frame
	 * (hal/amateur.c): the radio must be back in its Bluetooth configuration by then.
	 */
	if (!abort && direction == EFR32_RADIO_PACKET_TX && efr32_cw_frame) {
		*budget = EFR32_DL_MEAS_CYC(CONFIG_EFR32_RADIO_WCET_COMPLETION_TX_NS);
		*deadline = (int)(now + k_us_to_cyc_floor32(EVENT_OVERHEAD_XTAL_US +
							   EVENT_OVERHEAD_END_US) -
				  EFR32_DL_MEAS_CYC(CONFIG_EFR32_RADIO_WCET_COMPLETION_TX_NS));
		return;
	}
#if defined(CONFIG_EFR32_RADIO_AMATEUR_ID_DATA)
	/*
	 * A data-identification PDU: its completion launches the next part of the identification
	 * (hal/amateur.c), which starts within the advertising PDU spacing of this TXEN, the bound
	 * the identification's slot is sized with (efr32_amateur_id_span_us()).
	 */
	if (!abort && direction == EFR32_RADIO_PACKET_TX && efr32_id_adv_frame) {
		/* A transmit's measured completion plus the identification's own callback. */
		*budget = EFR32_DL_MEAS_CYC(EFR32_ID_COMPLETION_NS);
		*deadline = efr32_dl_adv_next_start_by(efr32_radio_last_txen,
						       EFR32_ID_COMPLETION_NS);
		return;
	}
#endif
#endif
	if (abort) {
		wcet_ns = conn ? CONFIG_EFR32_RADIO_WCET_CONN_COMPLETION_CLOSE_NS :
				 CONFIG_EFR32_RADIO_WCET_COMPLETION_ABORT_NS;
	} else if (direction == EFR32_RADIO_PACKET_TX && rx_follows &&
		   !IS_ENABLED(CONFIG_EFR32_RADIO_QEMU_TIFS)) {
		*budget = EFR32_DL_MEAS_CYC(EFR32_COMPLETION_NS(conn, TX_RX));
		*deadline = efr32_dl_completion_tx_rx_start_by(
			now, conn ? CONFIG_EFR32_RADIO_CONN_COMPLETION_TO_PENDING_NS :
				    CONFIG_EFR32_RADIO_COMPLETION_TO_PENDING_NS);
		return;
	} else if (direction == EFR32_RADIO_PACKET_TX) {
		wcet_ns = conn ? CONFIG_EFR32_RADIO_WCET_CONN_COMPLETION_CLOSE_NS :
				 CONFIG_EFR32_RADIO_WCET_COMPLETION_TX_NS;
	} else if (efr32_hw_rx2tx_armed) {
		uint32_t stimer;
		uint32_t at;

		*budget = EFR32_DL_MEAS_CYC(EFR32_COMPLETION_NS(conn, RX_TX));
		if (efr32_rac_stimer_in(EFR32_RAC_STATE_RX2TX, &stimer, &at)) {
			int32_t ticks = (int32_t)EFR32_RX2TX_RESPOND_BY_STIMER - (int32_t)stimer;
			/* Rounded towards earlier either way. */
			uint32_t left = (ticks >= 0) ?
				k_ns_to_cyc_floor32((uint32_t)ticks * 10000U /
						    EFR32_SEQ_RAC_STIMER_TICKS_PER_10US) :
				-k_ns_to_cyc_ceil32(DIV_ROUND_UP((uint32_t)-ticks * 10000U,
						    EFR32_SEQ_RAC_STIMER_TICKS_PER_10US));

			*deadline = (int)(at + left -
					  EFR32_DL_MEAS_CYC(conn ?
						(CONFIG_EFR32_RADIO_CONN_COMPLETION_TO_RESPOND_NS +
						 EFR32_DL_CCM_RX_TX_NS) :
						CONFIG_EFR32_RADIO_COMPLETION_TO_RESPOND_NS));
			return;
		}
		/* The sequencer has left RX2TX: no response can follow this frame. */
		wcet_ns = EFR32_COMPLETION_NS(conn, RX_TX);
	} else {
		wcet_ns = conn ? CONFIG_EFR32_RADIO_WCET_CONN_COMPLETION_CLOSE_NS :
				 CONFIG_EFR32_RADIO_WCET_COMPLETION_RX_NS;
	}
	*budget = EFR32_DL_MEAS_CYC(wcet_ns);
	*deadline = efr32_radio_next_start_by(now, wcet_ns);
}
/*
 * Bounds a silent RX window: the BURTC alarm the QEMU TIFS path uses is real
 * hardware, and its expiry callback disables the radio unless a completion
 * already owns cleanup -- the same no-response rule as the QEMU job.  Each
 * arm gets a new generation so a stale expiry is ignored.  radio_disable()
 * is interrupt-safe (OpenISA calls it from LLL ISRs); RAM leaves no room for
 * a dedicated thread.
 */
static void efr32_hw_rx_fifo_reset(void);
static void efr32_bufc_rx_fifo_configure(void);

static void efr32_hw_turnaround_reset(void)
{
	efr32_hw_rx_after_tx_requested = false;
	efr32_hw_rx_after_tx_pending = false;
	efr32_hw_rx_turnaround_claimed = false;
	efr32_hw_tx_after_rx_requested = false;
	efr32_hw_rx_fifo_reset();
	/*
	 * After a staged reply the sequencer itself arms the next receive (decision word STAGED),
	 * which the M33 may never label: withdraw it too, or a later window never armed would
	 * request TX at its first frame (7.80).
	 */
	if (efr32_hw_rx2tx_armed || IS_ENABLED(CONFIG_EFR32_RADIO_CONN_STAGED)) {
		efr32_hw_rx2tx_armed = false;
		*radio_reg32(EFR32_SEQ_RX2TX_DECISION_ADDRESS) = EFR32_SEQ_RX2TX_CANCEL;
	}
	efr32_conn_stage_reset();
#if !defined(CONFIG_EFR32_RADIO_QEMU_TIFS)
	atomic_inc(&efr32_hw_hcto_generation);
	cntr_qemu_tifs_cmp_cancel();
#endif
}

/* [regs-public] Enable the eight radio blocks in the observed stock order. */
/*
 * The vendor RAIL path enables both the HCLKRADIO branch and each radio block's bus clock
 * before accessing the block. The custom LLL bypasses RAIL, so it owns that prerequisite.
 * While gated, a read of a radio window is a bus error and a write is dropped (radio-tables.md
 * §7.72), so every path that may run before radio_setup() (the safe shutdown) enables them
 * itself. The SET aliases make this idempotent and safe from any context.
 */
static void efr32_radio_bus_clocks_on(void)
{
	CMU->RADIOCLKCTRL_SET = CMU_RADIOCLKCTRL_EN;
	/* RDMAILBOX1 carries timed commands to the sequencer (hal/efr32/seq_abi.h). */
	CMU->CLKEN1_SET = CMU_CLKEN1_AGC | CMU_CLKEN1_MODEM | CMU_CLKEN1_RFCRC |
			  CMU_CLKEN1_FRC | CMU_CLKEN1_PROTIMER | CMU_CLKEN1_RAC |
			  CMU_CLKEN1_SYNTH | CMU_CLKEN1_BUFC | CMU_CLKEN1_RDMAILBOX1;
	__DSB();
}

void radio_setup(void)
{
	efr32_radio_hold_init();
	efr32_packet_stage_reset();
	efr32_rx_delivery_invalid = false;
	/* The turnaround reset writes shared SEQRAM words: radio bus clock first. */
	efr32_radio_bus_clocks_on();
	efr32_hw_turnaround_reset();
#if defined(CONFIG_EFR32_RADIO_QEMU_TIFS)
	efr32_qemu_tifs_state_init(&efr32_qemu_tifs);
	efr32_qemu_tifs_next_action = EFR32_QEMU_TIFS_NEXT_NONE;
	efr32_qemu_tifs_transition_requested = false;
	efr32_qemu_tifs_rx_to_tx_staging = false;
#endif
	/* QEMU used to expose every register with these reset-disabled, masking the omission. */
	efr32_radio_bus_clocks_on();

	/*
	 * Every address below is a rev-B2 non-secure EN register at +0x004, with
	 * the sole writable field EN.EN at bit zero.  Preserve the order captured
	 * from working stage-2 hardware: in particular, BUFC and PROTIMER are live
	 * before RAC is enabled.  The old RAC-before-dependencies order was
	 * accepted by QEMU but left silicon in SHUTDOWN.
	 */
	*radio_reg32(EFR32_AGC_BASE + EFR32_RADIO_EN_OFFSET) = 1U;
	*radio_reg32(EFR32_BUFC_BASE + EFR32_RADIO_EN_OFFSET) = 1U;
	*radio_reg32(EFR32_MODEM_BASE + EFR32_RADIO_EN_OFFSET) = 1U;
	*radio_reg32(EFR32_RFCRC_BASE + EFR32_RADIO_EN_OFFSET) = 1U;
	*radio_reg32(EFR32_FRC_BASE + EFR32_RADIO_EN_OFFSET) = 1U;
	*radio_reg32(EFR32_PROTIMER_BASE + EFR32_RADIO_EN_OFFSET) = 1U;
	*radio_reg32(EFR32_RAC_BASE + EFR32_RADIO_EN_OFFSET) = 1U;
	*radio_reg32(EFR32_SYNTH_BASE + EFR32_RADIO_EN_OFFSET) = 1U;

	/*
	 * RAIL selects active-high polarity for the RAC activity indication and
	 * the external PA/LNA enables.  These are electrical-interface choices,
	 * not an unexplained sequencer preset. radio_reset() performs the ordered
	 * private-M0+ handoff after installing the source-built image.
	 */
	*radio_reg32(EFR32_RAC_BASE + EFR32_RAC_CTRL_OFFSET) =
		EFR32_RAC_CTRL_ACTIVE_HIGH | EFR32_RAC_CTRL_PA_ENABLE_HIGH |
		EFR32_RAC_CTRL_LNA_ENABLE_HIGH;

	/* The completion registers are accessible only after FRC.EN takes effect. */
	efr32_radio_completion_quiesce();

	/*
	 * [RAIL init] The xG22 RF initializer writes RXCTRL=0x60, then clears
	 * RAC.SR0 bit 6 through its CLR alias (radio-tables.md §3.3). These
	 * are exact initialization writes; they do not assert a packet policy.
	 */
	*radio_reg32(EFR32_FRC_BASE + EFR32_FRC_RXCTRL_OFFSET) = 0x60U;
	/*
	 * [F] BLE RX policy, as stock runs it (live RXCTRL 0x262 and
	 * TRAILRXDATA 0x1B in RXSEARCH, SS7.64): keep CRC-failed frames so the
	 * LLL sees them, and append the CRC result after each frame.  Without
	 * ACCEPTCRCERRORS FRC silently discards a failed frame (no RXDONE);
	 * without TRAILRXDATA.CRCOK there is no per-frame CRC result at all --
	 * TRAILRXDATA is configuration, not status (SS7.66).
	 */
#if !defined(CONFIG_EFR32_RADIO_QEMU_RX_DESCRIPTOR)
	/* The QEMU descriptor seam keeps its own CRC contract. */
	*radio_reg32(EFR32_FRC_BASE + EFR32_REGISTER_SET_OFFSET +
		     EFR32_FRC_RXCTRL_OFFSET) = EFR32_FRC_RXCTRL_ACCEPTCRCERRORS;
	*radio_reg32(EFR32_FRC_BASE + EFR32_FRC_TRAILRXDATA_OFFSET) =
		EFR32_FRC_TRAILRXDATA_CRCOK;
#endif
	*radio_reg32(EFR32_FRC_BASE + EFR32_FRC_CMD_OFFSET) =
		EFR32_FRC_CMD_RXABORT | EFR32_FRC_CMD_CONVINIT;
	*radio_reg32(EFR32_RAC_BASE + EFR32_REGISTER_CLEAR_OFFSET +
		     EFR32_RAC_SR0_OFFSET) = EFR32_RAC_SR0_INIT_CLEAR;

	/*
	 * The LLL connects its radio ISR to FRC_IRQn.  Enable only the two
	 * completion latches it observes, rather than treating a RAC state change
	 * or an unrelated FRC condition as an OpenISA radio completion.
	 * lll_init() enables FRC_IRQn before calling radio_setup(), so discard a
	 * retained completion before exposing either source through IEN.  This is
	 * intentionally limited to TXDONE/RXDONE: other FRC pending sources are
	 * outside the OpenISA completion contract and must retain their state.
	 */
	*radio_reg32(EFR32_FRC_BASE + EFR32_REGISTER_CLEAR_OFFSET +
		     EFR32_FRC_IF_OFFSET) = EFR32_FRC_IF_COMPLETION;
	*radio_reg32(EFR32_FRC_BASE + EFR32_REGISTER_SET_OFFSET +
		     EFR32_FRC_IEN_OFFSET) = EFR32_FRC_IF_COMPLETION;

	/* Load and start the private core here, once, as stock RAIL does at
	 * initialization. Otherwise the first LLL prepare's radio_reset() pays
	 * for the image copy, verify and core start ahead of its launch. */
	radio_reset();

	/* Match OpenISA's initially available radio-clock contract. */
	atomic_set(&efr32_radio_power_ref, 1);

	/* Never leave BUFC's RX channel at its reset ADDR (0x20000000, start of
	 * RAM); see efr32_hw_rx_fifo_preconfigured. */
	efr32_bufc_rx_fifo_configure();

#if defined(CONFIG_EFR32_RADIO_QEMU_RX_DESCRIPTOR)
	/* The descriptor path is an explicit simulator-only test contract. */
	efr32_radio_qemu_rx_descriptor_setup();
#endif
}

/* [regs-vendor] Retain the source-backed reset prefix; see radio-tables.md §4.1. */
void radio_reset(void)
{
	uint32_t status;

	EFR32_RADIO_TRACE_ONCE("radio-reset");
	efr32_radio_launch_disarm();
	efr32_hw_turnaround_reset();
	/* Every prepare starts here; a connection's sets its facts after (radio_conn.h). */
	efr32_radio_conn_interval_us = 0U;
#if defined(CONFIG_EFR32_RADIO_AMATEUR_ID_DATA)
	efr32_id_adv_frame = false;
#endif
	efr32_radio_rx_window_tolerance_us = 0U;
	efr32_radio_rx_eof_valid = false;
#if defined(CONFIG_EFR32_RADIO_QEMU_TIFS)
	efr32_qemu_tifs_cancel();
	efr32_qemu_tifs_next_action = EFR32_QEMU_TIFS_NEXT_NONE;
	efr32_qemu_tifs_transition_requested = false;
#endif
	/* A hardware reset invalidates both BUFC channel configurations. */
	efr32_packet_stage_reset();
	efr32_rx_delivery_invalid = false;
#if defined(CONFIG_EFR32_RADIO_QEMU_RX_DESCRIPTOR)
	/* Do not carry a simulator descriptor or its synthetic CRC across reset. */
	efr32_qemu_rx_descriptor.descriptor_read_offset = 0U;
	efr32_qemu_rx_descriptor.descriptor_crc_valid = false;
#endif
	efr32_radio_completion_quiesce();

	/*
	 * Stock RAIL clears the three RAC scratch registers, forces the radio
	 * quiescent, seeds the sequencer-timer-compare event and installs its
	 * initial sequencer-control mode, then releases CPU wait before removing
	 * force-disable.  The open M0+ image is copied and verified below; retained
	 * SEQRAM is explicitly never trusted.
	 * This is the exact 0x12d58 initializer order, not a snapshot transplant.
	 * SR3 (RAC+0x4C) is deliberately NOT cleared here, unlike SR0..SR2: the
	 * verified 0x12d58 disassembly only clears SR0/SR1/SR2 (three `str`s to
	 * +0x40/+0x44/+0x48, no fourth to +0x4C). An earlier pass here added a
	 * full SR3 = 0 by incorrect analogy with SR0..SR2; that was wrong and is
	 * reverted. SR3 holds a fixed, nonzero, non-firmware-literal value on
	 * stock across every hardware-lockstep capture including the very first
	 * one (docs/radio-tables.md SS7.5.1) -- a full clear here would destroy
	 * it. The two specific bits stock *does* clear (14 and 30, via SR3's CLR
	 * alias, as the first store of its TX-start sequence) are handled in
	 * radio_tx_enable() instead, at the matching point in time.
	 */
	efr32_radio_sequencer_install();
	*radio_reg32(EFR32_RAC_BASE + EFR32_RAC_SR0_OFFSET) = 0U;
	*radio_reg32(EFR32_RAC_BASE + EFR32_RAC_SR1_OFFSET) = 0U;
	*radio_reg32(EFR32_RAC_BASE + EFR32_RAC_SR2_OFFSET) = 0U;
	*radio_reg32(EFR32_RAC_BASE + EFR32_REGISTER_SET_OFFSET +
		     EFR32_RAC_CTRL_OFFSET) = EFR32_RAC_CTRL_FORCEDISABLE;
	*radio_reg32(EFR32_RAC_BASE + EFR32_REGISTER_SET_OFFSET +
		     EFR32_RAC_SEQIF_OFFSET) = EFR32_RAC_SEQIF_STIMCMPEVSEQ;
	do {
		status = *radio_reg32(EFR32_RAC_BASE + EFR32_RAC_STATUS_OFFSET);
		uint32_t state = (status >> EFR32_RAC_STATUS_STATE_SHIFT) &
				 EFR32_RAC_STATUS_STATE_MASK;

		if (state == EFR32_RAC_STATE_OFF ||
		    state == EFR32_RAC_STATE_SHUTDOWN) {
			break;
		}
	} while (true);
	EFR32_RADIO_TRACE_ONCE("radio-reset quiescent-status=0x%08x", status);
	*radio_reg32(EFR32_RAC_BASE + EFR32_RAC_SEQCTRL_OFFSET) =
		EFR32_RAC_SEQCTRL_STAGE2_STARTUP;
	/* We explicitly asserted SEQRESET before copying, unlike RAIL's warm
	 * helper.  Release with a base-register replacement, as the successful
	 * silicon launcher does, so the reset request cannot remain selected while
	 * CPUWAITDIS is added through an alias. */
	*radio_reg32(EFR32_RAC_BASE + EFR32_RAC_CTRL_OFFSET) =
		EFR32_RAC_CTRL_ACTIVE_HIGH | EFR32_RAC_CTRL_PA_ENABLE_HIGH |
		EFR32_RAC_CTRL_LNA_ENABLE_HIGH | EFR32_RAC_CTRL_FORCEDISABLE |
		EFR32_RAC_CTRL_CPUWAITDIS;
	*radio_reg32(EFR32_RAC_BASE + EFR32_REGISTER_CLEAR_OFFSET +
		     EFR32_RAC_CTRL_OFFSET) = EFR32_RAC_CTRL_FORCEDISABLE;
	__DSB();
	efr32_radio_sequencer_wait_ready();
	EFR32_RADIO_TRACE_ONCE("sequencer identity=0x%08x",
			       *radio_reg32(EFR32_SEQ_IDENTITY_ADDRESS));
	EFR32_RADIO_TRACE_ONCE("radio-reset idle");

}

/*
 * Channel-dependent demodulator rates [F] (docs/radio-tables.md SS7.83).  The IF ADC is clocked
 * from the LO: it samples at f_LO / 2^9 with RAC.IFADCTRIM0.IFADCENHALFMODE (the 1M PHY) and
 * f_LO / 2^8 without (2M, Coded), where f_LO = f_RF + f_IF on the high side
 * (SYNTH.IFFREQ.LOSIDE; f_IF = IFFREQ x f_HFXO / 2^19, the unit of SYNTH.FREQ).  So the ADC rate
 * moves with the channel, 3.3 % from 2402 to 2480 MHz, and every quantity the PHY table gives
 * in ADC samples is right only on the table's base channel, 2402 MHz.  Stock RAIL recomputes two
 * per channel (captured on silicon over 23 channels, advertising and data):
 *
 * - MODEM.DIGMIXCTRL.DIGMIXFREQ, the digital mixer's IF in units of f_ADC / 2^20:
 *   round(floor(f_IF in Hz) x 2^20 / f_ADC).  The truncation of f_IF to whole hertz is stock's
 *   (it decides the rounding of 3 of the 23 captured values).
 * - MODEM.SRCCHF, the two-stage sample-rate converter from f_ADC to the demodulator's fixed
 *   rate: SRCRATIO1 / 2^7 x SRCRATIO2 / 2^14 = f_ADC / f_SRC_OUT.  SRCRATIO1 is the largest
 *   code that leaves stage 2 at or above 25/24 (the measured code boundaries bracket that limit
 *   to 1.0409-1.0417); SRCRATIO2 then rounds the remainder.  With SRCENABLE1 clear (Coded),
 *   SRCRATIO2 / 2^14 alone = f_ADC / f_SRC_OUT.
 *
 * With the 2402 MHz values on every channel the demodulator ran 1 % off rate on 2426 MHz and
 * 3.2 % on 2480 MHz: headers decoded, frames broke part way (SS7.80's channel-dependent receive).
 * f_SRC_OUT per PHY is what the PHY table's own 2402 MHz SRCCHF gives: 4 MHz for 1M (4 samples
 * per 1 Msym/s symbol), 8 MHz for 2M and Coded.
 */
#define EFR32_SYNTH_IFFREQ_OFFSET 0x038U
#define EFR32_SYNTH_IFFREQ_IFFREQ_MASK 0x000FFFFFU
#define EFR32_SYNTH_IFFREQ_LOSIDE BIT(20)
#define EFR32_RAC_IFADCTRIM0_OFFSET 0x0E4U
#define EFR32_RAC_IFADCTRIM0_IFADCENHALFMODE BIT(1)
#define EFR32_IFADC_LO_DIVIDER_LOG2_HALF_MODE 9U
#define EFR32_IFADC_LO_DIVIDER_LOG2_FULL 8U
#define EFR32_MODEM_SRCCHF_OFFSET 0x0ECU
#define EFR32_MODEM_SRCCHF_SRCRATIO1_MASK 0x000000FFU
#define EFR32_MODEM_SRCCHF_SRCENABLE1 BIT(11)
#define EFR32_MODEM_SRCCHF_SRCRATIO2_SHIFT 12U
#define EFR32_MODEM_SRCCHF_SRCRATIO2_MASK (0x7FFFU << EFR32_MODEM_SRCCHF_SRCRATIO2_SHIFT)
#define EFR32_MODEM_SRCRATIO1_FRACTION_BITS 7U
#define EFR32_MODEM_SRCRATIO2_FRACTION_BITS 14U
#define EFR32_MODEM_SRC2_MIN_RATIO_NUM 25U
#define EFR32_MODEM_SRC2_MIN_RATIO_DEN 24U
#define EFR32_MODEM_DIGMIXCTRL_OFFSET 0x10CU
#define EFR32_MODEM_DIGMIXCTRL_DIGMIXFREQ_MASK 0x000FFFFFU
#define EFR32_MODEM_DIGMIXFREQ_FRACTION_BITS 20U
#define EFR32_MODEM_SRC_OUT_HZ_1M 4000000U
#define EFR32_MODEM_SRC_OUT_HZ_2M_CODED 8000000U

/* SRC output rate of the PHY last applied by radio_phy_set(); 0 before the first. */
static uint32_t efr32_modem_src_out_hz;

static void efr32_modem_channel_rates_set(void)
{
	uint32_t freq = *radio_reg32(EFR32_SYNTH_BASE + EFR32_SYNTH_FREQ_OFFSET);
	uint32_t iffreq_reg = *radio_reg32(EFR32_SYNTH_BASE + EFR32_SYNTH_IFFREQ_OFFSET);
	uint32_t iffreq = iffreq_reg & EFR32_SYNTH_IFFREQ_IFFREQ_MASK;
	uint32_t divider_log2 = (*radio_reg32(EFR32_RAC_BASE + EFR32_RAC_IFADCTRIM0_OFFSET) &
				 EFR32_RAC_IFADCTRIM0_IFADCENHALFMODE) ?
				EFR32_IFADC_LO_DIVIDER_LOG2_HALF_MODE :
				EFR32_IFADC_LO_DIVIDER_LOG2_FULL;
	uint32_t srcchf = *radio_reg32(EFR32_MODEM_BASE + EFR32_MODEM_SRCCHF_OFFSET);
	uint32_t digmix = *radio_reg32(EFR32_MODEM_BASE + EFR32_MODEM_DIGMIXCTRL_OFFSET);
	/* f_LO in SYNTH.FREQ units (f_HFXO / 2^19). */
	uint64_t lo = (iffreq_reg & EFR32_SYNTH_IFFREQ_LOSIDE) ? (uint64_t)freq + iffreq :
							       (uint64_t)freq - iffreq;
	uint64_t if_hz = ((uint64_t)iffreq * EFR32_SYNTH_REFERENCE_HZ) >>
			 EFR32_SYNTH_FREQ_FRACTION_BITS;
	/*
	 * DIGMIXFREQ = f_IF x 2^20 / (f_LO / 2^d) = f_IF x 2^(20 + d + 19) / (lo x f_HFXO).  f_HFXO
	 * is 2^12 x 9375: cancel the power of two so the numerator stays within 64 bits.
	 */
	uint32_t ref_pow2 = __builtin_ctzll(EFR32_SYNTH_REFERENCE_HZ);
	uint64_t ref_odd = EFR32_SYNTH_REFERENCE_HZ >> ref_pow2;
	uint64_t mix_num = if_hz << (EFR32_MODEM_DIGMIXFREQ_FRACTION_BITS + divider_log2 +
				     EFR32_SYNTH_FREQ_FRACTION_BITS - ref_pow2);
	uint64_t mix_den = lo * ref_odd;
	uint32_t mix = (uint32_t)((mix_num + mix_den / 2U) / mix_den);
	/*
	 * f_ADC / f_SRC_OUT = lo x f_HFXO / (2^19 x 2^d x f_SRC_OUT); scaled by 2^(7 + 14) for the
	 * two ratio codes' fraction bits: num / den with the 2^19 folded in.
	 */
	uint64_t src_num = lo * EFR32_SYNTH_REFERENCE_HZ;
	uint64_t src_den = (uint64_t)efr32_modem_src_out_hz
			   << (divider_log2 + EFR32_SYNTH_FREQ_FRACTION_BITS -
			       EFR32_MODEM_SRCRATIO1_FRACTION_BITS - EFR32_MODEM_SRCRATIO2_FRACTION_BITS);
	uint32_t ratio1 = srcchf & EFR32_MODEM_SRCCHF_SRCRATIO1_MASK;
	uint32_t ratio2;

	BUILD_ASSERT(EFR32_SYNTH_FREQ_FRACTION_BITS >= EFR32_MODEM_SRCRATIO1_FRACTION_BITS +
		     EFR32_MODEM_SRCRATIO2_FRACTION_BITS - EFR32_IFADC_LO_DIVIDER_LOG2_FULL,
		     "SRC scaling shift must be non-negative");
	if (efr32_modem_src_out_hz == 0U || freq == 0U) {
		return;
	}
	if (srcchf & EFR32_MODEM_SRCCHF_SRCENABLE1) {
		ratio1 = (uint32_t)((src_num * EFR32_MODEM_SRC2_MIN_RATIO_DEN) /
				    (src_den * EFR32_MODEM_SRC2_MIN_RATIO_NUM
				     << EFR32_MODEM_SRCRATIO2_FRACTION_BITS));
		ratio2 = (uint32_t)((src_num + (src_den * ratio1) / 2U) / (src_den * ratio1));
	} else {
		uint64_t den = src_den << EFR32_MODEM_SRCRATIO1_FRACTION_BITS;

		ratio2 = (uint32_t)((src_num + den / 2U) / den);
	}
	LL_ASSERT(ratio1 <= EFR32_MODEM_SRCCHF_SRCRATIO1_MASK);
	LL_ASSERT(ratio2 <= (EFR32_MODEM_SRCCHF_SRCRATIO2_MASK >>
			     EFR32_MODEM_SRCCHF_SRCRATIO2_SHIFT));
	LL_ASSERT(mix <= EFR32_MODEM_DIGMIXCTRL_DIGMIXFREQ_MASK);
	srcchf &= ~(EFR32_MODEM_SRCCHF_SRCRATIO1_MASK | EFR32_MODEM_SRCCHF_SRCRATIO2_MASK);
	srcchf |= ratio1 | (ratio2 << EFR32_MODEM_SRCCHF_SRCRATIO2_SHIFT);
	digmix = (digmix & ~EFR32_MODEM_DIGMIXCTRL_DIGMIXFREQ_MASK) | mix;
	*radio_reg32(EFR32_MODEM_BASE + EFR32_MODEM_SRCCHF_OFFSET) = srcchf;
	*radio_reg32(EFR32_MODEM_BASE + EFR32_MODEM_DIGMIXCTRL_OFFSET) = digmix;
}

/* [regs-public] Public BLE tables plus a relocated, image-local PHYINFO object. */
void radio_phy_set(uint8_t phy, uint8_t flags)
{
	EFR32_RADIO_TRACE_ONCE("phy phy=%u flags=%u", phy, flags);
	ARG_UNUSED(flags);
	/*
	 * Not while a staged reply may be on air: isr_rx() commits it with the PHY the receive
	 * used (one PHY per direction pair, 1M), already in the radio.
	 */
	if (efr32_conn_in_flight) {
		return;
	}

	switch (phy) {
	case PHY_LEGACY:
	case PHY_1M:
		efr32_radio_table_apply(efr32_ble_1m_base_table);
		efr32_radio_table_apply(efr32_ble_1m_delta_table);
		efr32_ble_phy_info_publish(efr32_ble_1m_phy_info);
		efr32_modem_src_out_hz = EFR32_MODEM_SRC_OUT_HZ_1M;
		break;
	case PHY_2M:
		efr32_radio_table_apply(efr32_ble_1m_base_table);
		efr32_radio_table_apply(efr32_ble_2m_delta_table);
		efr32_modem_src_out_hz = EFR32_MODEM_SRC_OUT_HZ_2M_CODED;
		break;
	case PHY_CODED:
		efr32_radio_table_apply(efr32_ble_1m_base_table);
		if (flags == PHY_FLAGS_S8) {
			efr32_radio_table_apply(efr32_ble_coded_s8_delta_table);
		} else {
			LL_ASSERT(flags == PHY_FLAGS_S2);
			efr32_radio_table_apply(efr32_ble_coded_s2_delta_table);
		}
		efr32_modem_src_out_hz = EFR32_MODEM_SRC_OUT_HZ_2M_CODED;
		break;
	default:
		EFR32_RADIO_TODO("regs-public: unknown PHY");
		break;
	}
	/* The tables carry the 2402 MHz rates: redo them for the channel already tuned. */
	efr32_modem_channel_rates_set();
}

#if defined(CONFIG_EFR32_RADIO_TX_POWER)
/*
 * MODEM.RAMPLEV (public register model MODEM_S_register.py: offset 0x0CC, reset 0x009F9F9F;
 * MODEM_S_field.py: RAMPLEV0..2, 8 bits each at bits 0, 8 and 16): the level the PA ramps to.
 * With an FSK modulation the 6 dBm PA's power level goes into all three fields, offset by
 * EFR32_PA6_RAMPLEV_OFFSET: the reset value's 0x9F is the highest level, 127, plus the offset
 * (docs/tx-power.md 2).
 */
#define EFR32_MODEM_RAMPLEV_OFFSET 0x0CCU
#define EFR32_MODEM_RAMPLEV_FIELDS 3U
#define EFR32_MODEM_RAMPLEV_FIELD_BITS 8U
#define EFR32_PA6_RAMPLEV_OFFSET 32U

static void efr32_radio_pa_level_set(uint8_t level)
{
	uint32_t ramplev = 0U;

	__ASSERT_NO_MSG(level <= EFR32_PA6_LEVEL_MAX);
	for (uint32_t field = 0U; field < EFR32_MODEM_RAMPLEV_FIELDS; field++) {
		ramplev |= ((uint32_t)level + EFR32_PA6_RAMPLEV_OFFSET)
			   << (field * EFR32_MODEM_RAMPLEV_FIELD_BITS);
	}
	*radio_reg32(EFR32_MODEM_BASE + EFR32_MODEM_RAMPLEV_OFFSET) = ramplev;
}
#endif

/*
 * [hardware] Fixed working-board PA characterization. The power in dBm: OpenISA's
 * RADIO_TXP_DEFAULT, or the role's level with BT_CTLR_TX_PWR_DYNAMIC_CONTROL. Without
 * EFR32_RADIO_TX_POWER, MODEM.RAMPLEV keeps its reset value, the 6 dBm PA's highest level.
 */
void radio_tx_power_set(uint32_t power)
{
#if defined(CONFIG_EFR32_RADIO_TX_POWER)
	efr32_radio_pa_level_set(efr32_tx_power_level((int8_t)power));
#else
	ARG_UNUSED(power);
#endif
	*radio_reg32(EFR32_RAC_BASE + EFR32_RAC_PAENCTRL_OFFSET) =
		EFR32_RAC_STAGE2_PAENCTRL;
	*radio_reg32(EFR32_RAC_BASE + EFR32_RAC_PATRIM1_OFFSET) =
		EFR32_RAC_STAGE2_PATRIM1;
	*radio_reg32(EFR32_RAC_BASE + EFR32_RAC_PATRIM3_OFFSET) =
		EFR32_RAC_STAGE2_PATRIM3;
	*radio_reg32(EFR32_RAC_BASE + EFR32_RAC_PACTRL_OFFSET) =
		EFR32_RAC_STAGE2_PACTRL;
}

/* The highest level, or the amateur station's ceiling (efr32_tx_power_limit_set()). */
void radio_tx_power_max_set(void)
{
	radio_tx_power_set(0U);
#if defined(CONFIG_EFR32_RADIO_TX_POWER)
	efr32_radio_pa_level_set(efr32_tx_power_max_level());
#endif
}

#if defined(CONFIG_BT_CTLR_TX_PWR_DYNAMIC_CONTROL)
/* The controller's per-role levels (ll_sw/ll_tx_pwr.c), in the dBm the steps report. */
int8_t lll_radio_tx_pwr_min_get(void)
{
	return efr32_tx_power_min_dbm();
}

int8_t lll_radio_tx_pwr_max_get(void)
{
	return efr32_tx_power_max_dbm();
}

int8_t lll_radio_tx_pwr_floor(int8_t tx_pwr_lvl)
{
	return efr32_tx_power_floor_dbm(tx_pwr_lvl);
}
#endif

#if defined(CONFIG_EFR32_RADIO_AMATEUR)
/* SYNTH.FREQ for a carrier in kHz, rounded to nearest as the captured words are. */
static uint32_t efr32_synth_freq_word(uint32_t khz)
{
	uint64_t hz = (uint64_t)khz * EFR32_HZ_PER_KHZ;

	return (uint32_t)(((hz << EFR32_SYNTH_FREQ_FRACTION_BITS) + (EFR32_SYNTH_REFERENCE_HZ / 2ULL)) /
			  EFR32_SYNTH_REFERENCE_HZ);
}
#endif

/* [hardware] Tune any BLE channel: carrier, channel index, channel-dependent modem rates (T4). */
void radio_freq_chan_set(uint32_t chan)
{
	uint32_t channel_number;
	uint32_t frequency_word;

	/* Zephyr passes MHz relative to 2400: 2..80, always even. */
	LL_ASSERT((chan >= 2U) && (chan <= 80U) && ((chan & 1U) == 0U));
	channel_number = (chan - 2U) / 2U;
#if defined(CONFIG_EFR32_RADIO_AMATEUR)
	/* Amateur operation: the plan's carrier for this RF channel (hal/amateur.c). */
	frequency_word = efr32_synth_freq_word(efr32_radio_amateur_carrier_khz(
		channel_number, (EFR32_BLE_CHANNEL_BASE_MHZ + chan) * EFR32_KHZ_PER_MHZ));
#else
	{
		uint64_t hz = (uint64_t)(EFR32_BLE_CHANNEL_BASE_MHZ + chan) * 1000000ULL;

		/* Rounded to nearest, as the captured advertising-channel words are. */
		frequency_word = (uint32_t)(((hz << EFR32_SYNTH_FREQ_FRACTION_BITS) +
					     (EFR32_SYNTH_REFERENCE_HZ / 2ULL)) /
					    EFR32_SYNTH_REFERENCE_HZ);
	}
#endif
	/*
	 * A hardware trace of the working RAIL_StartTx path establishes the
	 * complete channel handoff: WCNTCMP2 and CHSP carry the zero-based 2 MHz
	 * physical-channel index while CHCTRL remains zero.  The former open HAL
	 * put the index in CHCTRL instead; QEMU accepted that invented contract and
	 * therefore hid the silicon divergence for every channel except index 0.
	 */
	*radio_reg32(EFR32_SYNTH_BASE + EFR32_SYNTH_FREQ_OFFSET) = frequency_word;
	*radio_reg32(EFR32_FRC_BASE + EFR32_FRC_WCNTCMP2_OFFSET) = channel_number;
	*radio_reg32(EFR32_SYNTH_BASE + EFR32_SYNTH_CHCTRL_OFFSET) = 0U;
	*radio_reg32(EFR32_SYNTH_BASE + EFR32_SYNTH_CHSP_OFFSET) =
		channel_number & EFR32_SYNTH_CHSP_CHSP_MASK;
	efr32_modem_channel_rates_set();
}

/* [regs-public] RAIL's BLE helper writes FRC.WHITEPOLY=0x44 and WHITEINIT=iv|0x40 (T5). */
void radio_whiten_iv_set(uint32_t iv)
{
	/* LLL supplies a Bluetooth physical channel number, 0 through 39. */
	LL_ASSERT(iv <= 39U);
#if defined(CONFIG_EFR32_RADIO_RX_CHANNEL_STATS)
	efr32_radio_channel_index = (uint8_t)iv;
#endif
	*radio_reg32(EFR32_FRC_BASE + EFR32_FRC_WHITEPOLY_OFFSET) = EFR32_FRC_BLE_WHITEPOLY;
	*radio_reg32(EFR32_FRC_BASE + EFR32_FRC_WHITEINIT_OFFSET) =
		EFR32_FRC_BLE_WHITEINIT_MSB | iv;
}

#define EFR32_MODEM_VTCORRCFG0_OFFSET 0x104U
#define EFR32_MODEM_VTCORRCFG1_OFFSET 0x110U
#define EFR32_MODEM_VTCORRCFG1_EXPECTHT_SHIFT 27U
#define EFR32_MODEM_VTCORRCFG1_EXPECTHT_MASK (0xFU << EFR32_MODEM_VTCORRCFG1_EXPECTHT_SHIFT)
#define EFR32_MODEM_VTCORRCFG1_EXPECTHT_BLE_RX (2U << EFR32_MODEM_VTCORRCFG1_EXPECTHT_SHIFT)

static uint32_t efr32_vtcorr_expected_pattern(uint32_t access_address)
{
	uint32_t on_air = __RBIT(access_address);

	return (on_air >> 1) | (on_air << 31);
}

/* [regs-public] MODEM.SYNC0 is the 32-bit BLE synchronization word register (T3). */
void radio_aa_set(const uint8_t *aa)
{
	/*
	 * OpenISA callers pass the Bluetooth access address in little-endian byte
	 * order.  Assemble it explicitly rather than relying on aa being aligned
	 * for a uint32_t load (the API only promises a byte pointer).
	 */
	uint32_t sync0 = (uint32_t)aa[0] |
			 ((uint32_t)aa[1] << 8) |
			 ((uint32_t)aa[2] << 16) |
			 ((uint32_t)aa[3] << 24);

	*radio_reg32(EFR32_MODEM_BASE + EFR32_MODEM_SYNC0_OFFSET) = sync0;

	/*
	 * [F] The Viterbi demodulator correlates against the access address in
	 * on-air (LSB-first) bit order, rotated right by one: stock RAIL rewrites
	 * VTCORRCFG0 at runtime, and for the advertising address 0x8E89BED6 its
	 * live value is ror(bitrev(aa), 1) = 0xB5BEC8B8.  The PHY table's
	 * 0x4A413747 is the exact complement, with which the demodulator never
	 * detected a frame (docs/radio-tables.md SS7.64).  RAIL also lowers
	 * VTCORRCFG1.EXPECTHT from the table's 5 to 2 at the same point
	 * (observed value; its derivation is not yet known).
	 */
	*radio_reg32(EFR32_MODEM_BASE + EFR32_MODEM_VTCORRCFG0_OFFSET) =
		efr32_vtcorr_expected_pattern(sync0);
	*radio_reg32(EFR32_MODEM_BASE + EFR32_REGISTER_CLEAR_OFFSET +
		     EFR32_MODEM_VTCORRCFG1_OFFSET) = EFR32_MODEM_VTCORRCFG1_EXPECTHT_MASK;
	*radio_reg32(EFR32_MODEM_BASE + EFR32_REGISTER_SET_OFFSET +
		     EFR32_MODEM_VTCORRCFG1_OFFSET) = EFR32_MODEM_VTCORRCFG1_EXPECTHT_BLE_RX;
}

/* [regs-partial] FRC length/header configuration remains unproven (T5;
 * docs/radio-tables.md §4.2). CRC, whitening, and access-address
 * configuration are implemented above.  Preserve the LLL's framing bounds
 * for a later evidenced RX completion path, without inferring an FRC layout.
 */
void radio_pkt_configure(uint8_t bits_len, uint8_t max_len, uint8_t flags)
{
	uint32_t descriptor;

	EFR32_RADIO_TRACE_ONCE("packet bits=%u max=%u flags=%u",
			       bits_len, max_len, flags);
	/*
	 * A new framing contract invalidates both direction-specific FIFO setups.
	 * In particular, do not let a later TXEN reuse bytes staged for an older
	 * maximum length or header representation.  OpenISA configures a packet
	 * while no packet operation is active; retain that ordering as an explicit
	 * controller contract rather than trying to reconfigure an active FRC.
	 */
	LL_ASSERT(efr32_packet_stage.lifecycle.active_direction == EFR32_RADIO_PACKET_IDLE);
	/* pkt_scratch and the TX FIFO are sized for this bound. */
	LL_ASSERT(max_len <= EFR32_PKT_PAYLOAD_MAX);
	efr32_packet_stage.rx_target = NULL;
	efr32_packet_stage.rx_fifo_configured = false;
	efr32_packet_stage.tx_source = NULL;
	efr32_packet_stage.tx_bytes = 0U;
	efr32_packet_stage.tx_fifo_configured = false;
	efr32_packet_stage.bits_len = bits_len;
	efr32_packet_stage.max_len = max_len;
	efr32_packet_stage.flags = flags;
	efr32_packet_stage.packet_configured = true;
	/* A staged reply is being framed by FRC from this configuration (it is the same one). */
	if (efr32_conn_in_flight) {
		return;
	}

	/*
	 * DFLCTRL reads the eight-bit BLE payload length from header byte one.
	 * Before that byte has been consumed, WCNTCMP0 must not terminate the
	 * frame at its public-table reset value of zero.  Static analysis of the
	 * working coprocessor shows that it writes configured frame capacity minus
	 * one here; the working BLE capture and FCD0..3.WORDS both use the 255-word
	 * sentinel.  The dynamic-length engine subsequently replaces the effective
	 * packet boundary from the header.  This is framing bootstrap state, not an
	 * invented maximum payload and not a scheduler decision.
	 */
	LL_ASSERT(bits_len == 8U);
	*radio_reg32(EFR32_FRC_BASE + EFR32_FRC_WCNTCMP0_OFFSET) =
		EFR32_FRC_DYNAMIC_LENGTH_PENDING_WORD_COUNT;

	/*
	 * The working coprocessor clears the public three-bit
	 * FECCTRL.BLOCKWHITEMODE field and selects mode 1 for this BLE framing
	 * path.  That is the active-frame complement to FCD.SKIPWHITE=0 and the
	 * per-channel BLE whitening seed; retain unrelated FEC fields through the
	 * standard CLR/SET aliases.
	 */
	*radio_reg32(EFR32_FRC_BASE + EFR32_REGISTER_CLEAR_OFFSET +
		     EFR32_FRC_FECCTRL_OFFSET) =
		EFR32_FRC_FECCTRL_BLOCKWHITEMODE_MASK;
	*radio_reg32(EFR32_FRC_BASE + EFR32_REGISTER_SET_OFFSET +
		     EFR32_FRC_FECCTRL_OFFSET) =
		EFR32_FRC_BLE_BLOCKWHITEMODE;

	/*
	 * The public BLE PHY table is deliberately protocol-neutral at this
	 * boundary and leaves FCD0..3.SKIPWHITE asserted.  Bluetooth LLL callers
	 * subsequently provide the physical-channel whitening seed through
	 * radio_whiten_iv_set(), so each frame descriptor must feed bytes through
	 * the whitener.  A breakpoint-aligned stock BLE capture confirms all four
	 * descriptors with SKIPWHITE clear (0x0cff/0x0dff rather than the public
	 * baseline's 0x4cff/0x4dff).  Use the generated register-model field name
	 * here instead of importing those complete captured descriptor values.
	 */
	for (descriptor = 0U; descriptor < EFR32_FRC_FCD_COUNT; descriptor++) {
		*radio_reg32(EFR32_FRC_BASE + EFR32_REGISTER_CLEAR_OFFSET +
			     EFR32_FRC_FCD0_OFFSET +
			     descriptor * EFR32_FRC_FCD_STRIDE) =
			EFR32_FRC_FCD_SKIPWHITE;
	}
	EFR32_RADIO_TODO("remaining FRC length/header fields");
}

/*
 * RAIL's RX FIFO helper configures channel 1 in this exact order: CLEAR,
 * capacity encoding, backing address, and threshold control. Unlike TX,
 * its helper does not issue PREFETCH.
 */
static void efr32_bufc_rx_fifo_configure(void)
{
	uintptr_t base = EFR32_BUFC_BASE + EFR32_BUFC_RX_BASE_OFFSET;

	*radio_reg32(base + EFR32_BUFC_CMD_OFFSET) = EFR32_BUFC_CMD_CLEAR;
	*radio_reg32(base + EFR32_BUFC_CFG_OFFSET) = EFR32_BUFC_FIFO_CONFIG(sizeof(efr32_rx_fifo));
	*radio_reg32(base + EFR32_BUFC_ADDR_OFFSET) = (uint32_t)(uintptr_t)efr32_rx_fifo;
	*radio_reg32(base + EFR32_BUFC_THRESHOLD_OFFSET) = EFR32_BUFC_RX_THRESHOLD;
#if defined(CONFIG_EFR32_RADIO_CONN_STAGED)
	/* A staged receive's frame now starts at the FIFO's start (a Central's anchor CLEARs). */
	efr32_conn_rx_offset = 0U;
	*EFR32_SEQ_CONN_WORD(EFR32_SEQ_CONN_RX_OFFSET) = efr32_conn_rx_offset;
#endif
}

/*
 * Set when radio_tx_enable() has already pointed BUFC's RX channel at
 * efr32_rx_fifo for a hardware TX->RX turnaround.  The receiver is live from
 * the end of TX, before the LLL stages its RX target, and channel 1's reset
 * ADDR is 0x20000000: an unconfigured channel let the first received frame
 * overwrite the start of RAM (bt_dev) on hardware (SS7.64).  It must not be
 * cleared again then, or a frame already arriving would be discarded.
 */
static bool efr32_hw_rx_fifo_preconfigured;

static void efr32_hw_rx_fifo_reset(void)
{
	efr32_hw_rx_fifo_preconfigured = false;
}

/*
 * [regs-partial] Configure only the source-backed BUFC RX staging FIFO.
 * FRC's delivery of a received packet to this FIFO remains unproven (T5).
 */
void radio_pkt_rx_set(void *rx_packet)
{

	LL_ASSERT(efr32_packet_stage.packet_configured);
	/* The current OpenISA callers use the one-octet BLE length field. */
	LL_ASSERT(efr32_packet_stage.bits_len == 8U);
	/*
	 * The controller hands this target to the HAL before arming RX.  Replacing
	 * it after RAC is armed could redirect a future, normalized completion to
	 * a buffer belonging to another LLL event, so make that ordering an
	 * explicit HAL invariant.
	 */
	LL_ASSERT(efr32_packet_stage.lifecycle.active_direction == EFR32_RADIO_PACKET_IDLE);
	LL_ASSERT(rx_packet != NULL);
	LL_ASSERT(((uintptr_t)efr32_rx_fifo & 3U) == 0U);

	/* Keep the caller buffer untouched until the unproven FRC packet
	 * handoff can be implemented. */
	if (efr32_hw_rx_fifo_preconfigured) {
		efr32_hw_rx_fifo_preconfigured = false;
	} else {
		efr32_bufc_rx_fifo_configure();
	}

	/* Retain the controller's target for the eventual proven RX copy-out. */
	efr32_packet_stage.rx_target = rx_packet;
	efr32_packet_stage.rx_fifo_configured = true;
	efr32_radio_rx_pdu_delivery_reset(&efr32_packet_stage.rx_delivery);
}

/*
 * This is deliberately not connected to an FRC interrupt yet.  Its input is
 * a normalized Bluetooth Link Layer PDU: header octet, length octet, and
 * exactly that many payload octets.  A future caller must first prove how to
 * obtain this representation from BUFC/FRC; it must not pass raw BUFC memory
 * here or infer a frame boundary from an FRC completion interrupt.
 *
 * Keeping the controller-facing copy and its bounds check separate from that
 * unresolved hardware handoff makes the eventual delivery path auditable.
 */
static int efr32_packet_stage_rx_complete(const uint8_t *pdu,
						  uint16_t bytes)
{
	int err;

	if (!efr32_packet_stage.rx_target ||
	    !efr32_packet_stage.packet_configured ||
	    !efr32_packet_stage.rx_fifo_configured ||
	    efr32_packet_stage.lifecycle.active_direction != EFR32_RADIO_PACKET_RX) {
		efr32_rx_delivery_invalid = true;
		return -EINVAL;
	}

	err = efr32_radio_rx_pdu_deliver_once(&efr32_packet_stage.rx_delivery,
						     efr32_packet_stage.rx_target, pdu, bytes,
						     efr32_packet_stage.bits_len,
						     efr32_packet_stage.max_len);
	if (err) {
		efr32_rx_delivery_invalid = true;
		return err;
	}

#if defined(CONFIG_BT_CTLR_PRIVACY)
	/* OpenISA resolves the completed, normalized advertising PDU before LLL. */
	efr32_radio_ar_execute(pdu, bytes);
#endif

	/*
	 * Do not release this stage here: a producer runs before RXDONE, while
	 * isr_radio() still needs the active direction to attribute that latch.
	 * radio_status_reset() retires the stage after the LLL callback sampled
	 * the resulting completion and CRC state.
	 */

	return 0;
}

#if defined(CONFIG_EFR32_RADIO_QEMU_RX_DESCRIPTOR)
/*
 * This is an explicitly synthetic receive boundary.  The worker is the only
 * code that reads descriptor or FIFO bytes; the BUFC IRQ merely masks the
 * source and wakes it.  That keeps packet copying and channel-1 release out
 * of interrupt context and lets the existing FRC completion job remain the
 * sole caller of the OpenISA LLL callback.
 */
static inline volatile uint8_t *efr32_qemu_rx_descriptor_ring(void)
{
	uint32_t address = *radio_reg32(EFR32_BUFC_BASE +
		EFR32_BUFC_RX_DESCRIPTOR_BASE_OFFSET + EFR32_BUFC_ADDR_OFFSET);

	if (address == 0U) {
		return NULL;
	}

	return (volatile uint8_t *)(uintptr_t)address;
}

static inline uint8_t efr32_qemu_rx_descriptor_ring_byte(
	volatile uint8_t *ring, uint16_t offset)
{
	return ring[efr32_radio_rx_descriptor_ring_storage_offset(offset)];
}

static inline bool efr32_qemu_rx_descriptor_pending(void)
{
	return !!(*radio_reg32(EFR32_BUFC_BASE + EFR32_BUFC_IF_OFFSET) &
		EFR32_BUFC_DESCRIPTOR_PENDING);
}

/*
 * The descriptor ring is a QEMU-only circular FIFO. Keep its logical
 * consumer pointer visible to the model as each record is consumed; without
 * this acknowledgement, the model reports a full 512-byte ring after 64
 * eight-byte records even though the scheduler has delivered them all.
 */
static inline void efr32_qemu_rx_descriptor_ack(uint16_t offset)
{
	*radio_reg32(EFR32_BUFC_BASE +
		EFR32_BUFC_RX_DESCRIPTOR_BASE_OFFSET + EFR32_BUFC_READOFFSET_OFFSET) =
		offset & EFR32_RADIO_RX_DESCRIPTOR_ENDPOINT_MASK;
}

static void efr32_radio_qemu_rx_descriptor_job(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	for (;;) {
		volatile uint8_t *ring;
		uint32_t fifo_address;
		uint16_t write_offset;
		bool drained_one = false;
		bool sliced = false;

		k_deadline_event_wait(&efr32_qemu_rx_descriptor_event);
		ring = efr32_qemu_rx_descriptor_ring();
		fifo_address = *radio_reg32(EFR32_BUFC_BASE +
			EFR32_BUFC_RX_BASE_OFFSET + EFR32_BUFC_ADDR_OFFSET);
		if (!ring || fifo_address != (uint32_t)(uintptr_t)efr32_rx_fifo ||
		    efr32_packet_stage.lifecycle.active_direction != EFR32_RADIO_PACKET_RX) {
			/* A stale synthetic event is not a packet. */
			*radio_reg32(EFR32_BUFC_BASE + EFR32_REGISTER_CLEAR_OFFSET +
				EFR32_BUFC_IF_OFFSET) = EFR32_BUFC_DESCRIPTOR_PENDING;
			continue;
		}

		write_offset = *radio_reg32(EFR32_BUFC_BASE +
			EFR32_BUFC_RX_DESCRIPTOR_BASE_OFFSET + EFR32_BUFC_WRITEOFFSET) &
			EFR32_RADIO_RX_DESCRIPTOR_ENDPOINT_MASK;
		/*
		 * One record per run: its budget (EFR32_RADIO_WCET_QEMU_RX_DRAIN_NS) is
		 * one record's work, and the ULL worker at this priority, ordered by
		 * deadline, is never held behind more than that.
		 */
		while (efr32_qemu_rx_descriptor.descriptor_read_offset != write_offset) {
			if (drained_one) {
				/* Continue under a fresh deadline, after any job due sooner. */
				efr32_qemu_rx_descriptor_release();
				sliced = true;
				break;
			}
			drained_one = true;
			uint8_t raw[EFR32_RADIO_RX_DESCRIPTOR_BYTES];
			struct efr32_radio_rx_descriptor descriptor;
			uint8_t pdu[PDU_AC_LL_SIZE_MAX];
			uint16_t pdu_bytes = 0U;
			uint16_t descriptor_offset =
				efr32_qemu_rx_descriptor.descriptor_read_offset;
			int parse_err;
			int pdu_err;

			for (uint8_t index = 0U;
			     index < EFR32_RADIO_RX_DESCRIPTOR_BYTES; index++) {
				raw[index] = efr32_qemu_rx_descriptor_ring_byte(
					ring, descriptor_offset + index);
			}
			efr32_qemu_rx_descriptor.descriptor_read_offset =
				efr32_radio_rx_descriptor_ring_advance(descriptor_offset);
			efr32_qemu_rx_descriptor_ack(
				efr32_qemu_rx_descriptor.descriptor_read_offset);

			parse_err = efr32_radio_rx_descriptor_parse(raw, &descriptor);
			if (parse_err) {
				/* Non-received classes are queue events, never PDUs. */
				continue;
			}
			/*
			 * A single OpenISA RX stage owns exactly one PDU until its
			 * completion callback calls radio_status_reset().  The synthetic
			 * QEMU producer can queue descriptor records faster than this
			 * scheduler-owned consumer runs.  Retire any surplus successful
			 * record and its source FIFO interval, but never let its expected
			 * -EALREADY overwrite the first frame's CRC-valid outcome.
			 */
			if (efr32_radio_rx_pdu_delivery_is_complete(
				    &efr32_packet_stage.rx_delivery)) {
				*radio_reg32(EFR32_BUFC_BASE + EFR32_BUFC_RX_BASE_OFFSET +
					EFR32_BUFC_READOFFSET_OFFSET) = descriptor.endpoint;
				continue;
			}

			pdu_err = efr32_radio_rx_descriptor_normalize_pdu(
				pdu, efr32_rx_fifo,
				(uint16_t)*radio_reg32(EFR32_BUFC_BASE +
					EFR32_BUFC_RX_BASE_OFFSET + EFR32_BUFC_READOFFSET_OFFSET),
				descriptor.endpoint, efr32_packet_stage.max_len, &pdu_bytes);
			if (pdu_err || efr32_packet_stage_rx_complete(pdu, pdu_bytes)) {
				efr32_rx_delivery_invalid = true;
				continue;
			}
			/*
			 * Class 6/7 is the synthetic QEMU success result. It stands in
			 * for the missing hardware trailer/CRC producer and is never
			 * enabled in a hardware configuration.
			 */
			efr32_qemu_rx_descriptor.descriptor_crc_valid = true;

			/* The endpoint is the source-backed channel-1 release point. */
			*radio_reg32(EFR32_BUFC_BASE + EFR32_BUFC_RX_BASE_OFFSET +
				EFR32_BUFC_READOFFSET_OFFSET) = descriptor.endpoint;
		}
		if (sliced) {
			continue;
		}

		*radio_reg32(EFR32_BUFC_BASE + EFR32_REGISTER_CLEAR_OFFSET +
			EFR32_BUFC_IF_OFFSET) = EFR32_BUFC_DESCRIPTOR_PENDING;
		*radio_reg32(EFR32_BUFC_BASE + EFR32_REGISTER_SET_OFFSET +
			EFR32_BUFC_IEN_OFFSET) = EFR32_BUFC_DESCRIPTOR_PENDING;
		/* Only now may a retained FRC.RXDONE wake the completion ISR. */
		*radio_reg32(EFR32_FRC_BASE + EFR32_REGISTER_SET_OFFSET +
			EFR32_FRC_IEN_OFFSET) = EFR32_FRC_IF_COMPLETION;
	}
}

/*
 * The drain is bulk copying, below the radio jobs: a TIFS turn-around or launch due meanwhile
 * preempts it, and it shares the ULL worker's priority, ordered with it by deadline.
 */
K_THREAD_DEFINE(efr32_qemu_rx_descriptor_thread,
		CONFIG_EFR32_RADIO_COMPLETION_STACK_SIZE,
		efr32_radio_qemu_rx_descriptor_job, NULL, NULL, NULL,
		EFR32_ULL_THREAD_PRIO, 0, 0);

/* BUFC IRQ work is scheduler-owned; this ISR only masks and wakes. */
static void efr32_radio_qemu_rx_descriptor_isr(void *arg)
{
	uint32_t pending;

	ARG_UNUSED(arg);
	pending = *radio_reg32(EFR32_BUFC_BASE + EFR32_BUFC_IF_OFFSET) &
		*radio_reg32(EFR32_BUFC_BASE + EFR32_BUFC_IEN_OFFSET);
	if (!(pending & EFR32_BUFC_DESCRIPTOR_PENDING)) {
		return;
	}

	*radio_reg32(EFR32_BUFC_BASE + EFR32_REGISTER_CLEAR_OFFSET +
		EFR32_BUFC_IEN_OFFSET) = EFR32_BUFC_DESCRIPTOR_PENDING;
	efr32_qemu_rx_descriptor_release();
}

static void efr32_radio_qemu_rx_descriptor_setup(void)
{
	unsigned int key = irq_lock();
	uintptr_t descriptor_start =
		(uintptr_t)efr32_qemu_rx_descriptor_ring_storage;
	uintptr_t descriptor_end = descriptor_start +
		EFR32_RADIO_QEMU_RX_DESCRIPTOR_RING_BYTES;
	uintptr_t fifo_start = (uintptr_t)efr32_rx_fifo;
	uintptr_t fifo_end = fifo_start + sizeof(efr32_rx_fifo);

	/* Catch a linker/layout regression before QEMU can DMA across regions. */
	LL_ASSERT(descriptor_end <= fifo_start || fifo_end <= descriptor_start);

	efr32_qemu_rx_descriptor.descriptor_read_offset = 0U;
	efr32_qemu_rx_descriptor.descriptor_crc_valid = false;
	k_deadline_event_reset(&efr32_qemu_rx_descriptor_event);
	*radio_reg32(EFR32_BUFC_BASE + EFR32_REGISTER_CLEAR_OFFSET +
		EFR32_BUFC_IF_OFFSET) = EFR32_BUFC_DESCRIPTOR_PENDING;
	*radio_reg32(EFR32_BUFC_BASE + EFR32_BUFC_RX_DESCRIPTOR_BASE_OFFSET +
		EFR32_BUFC_CMD_OFFSET) = EFR32_BUFC_CMD_CLEAR;
	*radio_reg32(EFR32_BUFC_BASE + EFR32_BUFC_RX_DESCRIPTOR_BASE_OFFSET +
		EFR32_BUFC_CFG_OFFSET) =
		EFR32_BUFC_FIFO_CONFIG(EFR32_RADIO_QEMU_RX_DESCRIPTOR_RING_BYTES);
	*radio_reg32(EFR32_BUFC_BASE + EFR32_BUFC_RX_DESCRIPTOR_BASE_OFFSET +
		EFR32_BUFC_ADDR_OFFSET) =
		(uint32_t)(uintptr_t)efr32_qemu_rx_descriptor_ring_storage;
	*radio_reg32(EFR32_BUFC_BASE + EFR32_BUFC_RX_DESCRIPTOR_BASE_OFFSET +
		EFR32_BUFC_WRITEOFFSET) = 0U;
	efr32_qemu_rx_descriptor_ack(0U);
	*radio_reg32(EFR32_BUFC_BASE + EFR32_BUFC_RX_DESCRIPTOR_BASE_OFFSET +
		EFR32_BUFC_THRESHOLD_OFFSET) = EFR32_BUFC_DESCRIPTOR_THRESHOLD;
	*radio_reg32(EFR32_BUFC_BASE + EFR32_REGISTER_SET_OFFSET +
		EFR32_BUFC_IEN_OFFSET) = EFR32_BUFC_DESCRIPTOR_PENDING;
	IRQ_CONNECT(BUFC_IRQn, CONFIG_BT_CTLR_LLL_PRIO,
			efr32_radio_qemu_rx_descriptor_isr, NULL, 0);
	irq_enable(BUFC_IRQn);
	irq_unlock(key);
}

static void efr32_radio_qemu_rx_descriptor_quiesce(void)
{
	*radio_reg32(EFR32_BUFC_BASE + EFR32_REGISTER_CLEAR_OFFSET +
		EFR32_BUFC_IEN_OFFSET) = EFR32_BUFC_DESCRIPTOR_PENDING;
	*radio_reg32(EFR32_BUFC_BASE + EFR32_REGISTER_CLEAR_OFFSET +
		EFR32_BUFC_IF_OFFSET) = EFR32_BUFC_DESCRIPTOR_PENDING;
	k_irq_clear_pending(BUFC_IRQn);
}
#endif /* CONFIG_EFR32_RADIO_QEMU_RX_DESCRIPTOR */

/*
 * [regs-partial] Configure only the source-backed BUFC TX staging FIFO.
 * FRC's consumption of that FIFO and sequencer handoff remain unproven (T5).
 */
void radio_pkt_tx_set(void *tx_packet)
{
	const uint8_t *pdu = tx_packet;
	uint32_t bytes;

	LL_ASSERT(efr32_packet_stage.packet_configured);
	/* All current OpenISA TX callers use the octet-sized LL length field. */
	LL_ASSERT(efr32_packet_stage.bits_len == 8U);
	/*
	 * BUFC owns the staging FIFO after TX is armed.  Reconfiguring it while
	 * RAC is active could make that operation consume a mixture of two LLL
	 * events, so retain the same setup-before-arm contract as the RX path.
	 */
	LL_ASSERT(efr32_packet_stage.lifecycle.active_direction == EFR32_RADIO_PACKET_IDLE);
	LL_ASSERT(pdu != NULL);
#if defined(CONFIG_EFR32_RADIO_AMATEUR)
	/* Every link-layer transmission passes here: the identification schedule counts it. */
	efr32_radio_amateur_tx_notify();
#endif
	bytes = (uint32_t)pdu[1] + 2U;
	EFR32_RADIO_TRACE_ONCE("tx-fifo bytes=%u pdu0=0x%02x len=%u",
			       bytes, pdu[0], pdu[1]);
	/* OpenISA PDUs are header byte, payload-length byte, then payload. */
	LL_ASSERT(pdu[1] <= efr32_packet_stage.max_len);
	LL_ASSERT(bytes <= sizeof(efr32_tx_fifo));
	LL_ASSERT(((uintptr_t)efr32_tx_fifo & 3U) == 0U);
#if defined(CONFIG_EFR32_RADIO_CONN_STAGED)
	/* An advertiser's isr_rx() answers a SCAN_REQ here: the sequencer's outcome first. */
	if (efr32_conn_received) {
		uint32_t outcome;

		int staged = efr32_radio_conn_outcome(&outcome);

		/* The link layer answers exactly when the sequencer did. */
		LL_ASSERT(staged <= 0 || (outcome & EFR32_SEQ_CONN_OUT_REPLY) != 0U);
	}
	if (efr32_conn_committing) {
		/*
		 * isr_rx() commits the reply the sequencer already sent (lll/lll_conn_stage.c, the
		 * advertiser's SCAN_RSP): bookkeeping only. A data PDU's SN and NESN must be those
		 * on air.
		 */
		uint8_t header = efr32_conn_reply_header >= 0 ? (uint8_t)efr32_conn_reply_header :
								 pdu[0];
		uint8_t ack = (efr32_conn_outcome_word & EFR32_SEQ_CONN_OUT_ACK) != 0U;
		uint8_t seq_bits = BIT(EFR32_SEQ_PDU_SN_BIT) | BIT(EFR32_SEQ_PDU_NESN_BIT);

		if ((efr32_conn_flags & EFR32_SEQ_CONN_SCAN_REQ) != 0U) {
			efr32_scan_req_check(pdu);
		} else if ((efr32_conn_flags & (EFR32_SEQ_CONN_ADV | EFR32_SEQ_CONN_INIT)) != 0U) {
			LL_ASSERT(memcmp(pdu, efr32_conn_image[0], bytes) == 0);
		} else {
			LL_ASSERT((header & seq_bits) == efr32_conn_sent_seq_bits());
		}
		LL_ASSERT(bytes == efr32_conn_image_octets[ack]);
		efr32_init_sent = (efr32_conn_flags & EFR32_SEQ_CONN_INIT) != 0U;
		efr32_conn_committing = false;
		efr32_conn_reply_header = -1;
		efr32_packet_stage.tx_source = tx_packet;
		efr32_packet_stage.tx_bytes = bytes;
		efr32_packet_stage.tx_fifo_configured = true;
		/* Its TXDONE may be latched already: arm without clearing it. */
		efr32_rx_delivery_invalid = false;
		efr32_packet_lifecycle_arm(&efr32_packet_stage.lifecycle, EFR32_RADIO_PACKET_TX);
		efr32_hw_rx2tx_armed = false;
		atomic_inc(&efr32_hw_hcto_generation);
		/* The sequencer raised SWRXEN exactly when the event continues. */
		LL_ASSERT(efr32_hw_rx_after_tx_requested == efr32_radio_conn_continues());
		if (efr32_hw_rx_after_tx_requested) {
			/* The frame just received is consumed; the next starts at offset 0. */
			efr32_bufc_rx_fifo_configure();
			efr32_hw_rx_fifo_preconfigured = true;
		}
		efr32_hw_rx_after_tx_pending = efr32_hw_rx_after_tx_requested;
		efr32_hw_rx_after_tx_requested = false;
#if defined(CONFIG_EFR32_RADIO_TIMING_PROBE)
		EFR32_PROBE_PATH(to_respond);
#endif
		return;
	}
#endif
	/* Own the PDU until the scheduled TX job reaches the hardware boundary. */
	memcpy(efr32_tx_fifo, pdu, bytes);

	/* Keep source and length for a future FRC/BUFC completion diagnostic. */
	efr32_packet_stage.tx_source = tx_packet;
	efr32_packet_stage.tx_bytes = bytes;
	efr32_packet_stage.tx_fifo_configured = true;

	if (efr32_hw_rx2tx_armed &&
	    efr32_packet_stage.lifecycle.active_direction == EFR32_RADIO_PACKET_IDLE) {
		/* A response to the frame just received: the RAC is already in
		 * RX2TX.  Publish it, label the completion, retire the unserved-
		 * response guard, then release the sequencer's IRQ7. */
		efr32_hw_rx2tx_armed = false;
		atomic_inc(&efr32_hw_hcto_generation);
		efr32_tx_fifo_publish();
		efr32_packet_stage_arm(EFR32_RADIO_PACKET_TX);
		/*
		 * A response the LLL wants answered in turn (a connection event that continues)
		 * turns to RX at its end as a launched TX does (efr32_radio_tx_start()): the RX
		 * FIFO is set up and SWRXEN raised while the response is under way, so the RAC
		 * passes TX2RX at the frame's end.
		 */
		if (efr32_hw_rx_after_tx_requested) {
			efr32_bufc_rx_fifo_configure();
			efr32_hw_rx_fifo_preconfigured = true;
		}
		__DSB();
		*radio_reg32(EFR32_SEQ_RX2TX_DECISION_ADDRESS) = EFR32_SEQ_RX2TX_RESPOND;
		EFR32_PROBE_PATH(to_respond);
		EFR32_RX2TX_M33_STAMP(2);
		efr32_hw_rx_after_tx_pending = efr32_hw_rx_after_tx_requested;
		efr32_hw_rx_after_tx_requested = false;
		if (efr32_hw_rx_after_tx_pending) {
			*radio_reg32(EFR32_RAC_BASE + EFR32_REGISTER_SET_OFFSET +
				     EFR32_RAC_RXENSRCEN_OFFSET) = EFR32_RAC_RXENSRCEN_SWRXEN;
		}
	}
}

/*
 * TXEN to the first preamble bit (Kconfig EFR32_RADIO_TX_READY_NS, measured; LE 1M only). The
 * Central's ULL schedules its anchor's TXEN this much before the anchor point.
 */
uint32_t radio_tx_ready_delay_get(uint8_t phy, uint8_t flags)
{
	ARG_UNUSED(phy);
	ARG_UNUSED(flags);
	return DIV_ROUND_UP(CONFIG_EFR32_RADIO_TX_READY_NS, NSEC_PER_USEC);
}

/* [software] software (no register access needed); not implemented yet */
uint32_t radio_tx_chain_delay_get(uint8_t phy, uint8_t flags)
{
	ARG_UNUSED(phy);
	ARG_UNUSED(flags);
	EFR32_RADIO_TODO("software");
	return 0;
}

/*
 * SWRXEN to RXSEARCH, the receiver searching (Kconfig EFR32_RADIO_RX_READY_NS, measured; LE 1M
 * only). The Peripheral's ULL opens its receive windows this much earlier.
 */
uint32_t radio_rx_ready_delay_get(uint8_t phy, uint8_t flags)
{
	ARG_UNUSED(phy);
	ARG_UNUSED(flags);
	return DIV_ROUND_UP(CONFIG_EFR32_RADIO_RX_READY_NS, NSEC_PER_USEC);
}

/* [software] software (no register access needed); not implemented yet */
uint32_t radio_rx_chain_delay_get(uint8_t phy, uint8_t flags)
{
	ARG_UNUSED(phy);
	ARG_UNUSED(flags);
	EFR32_RADIO_TODO("software");
	return 0;
}

/*
 * [regs-vendor] RAIL StartRx writes RXENSRCEN.SET.SWRXEN (bit 1). The
 * archived primitive is at librail_efr32xg22_gcc_release.a(rail_rf_hal.o),
 * sli_rail_dadf1c52f26190429dcacf7151424c68; see radio-tables.md §4.1.
 */
static void efr32_radio_rx_start(bool hold, uint32_t instant, uint32_t limit)
{
	while (!radio_is_idle()) {
	}

	/* Validate and label a completion before arming the hardware source. */
	efr32_packet_stage_arm(EFR32_RADIO_PACKET_RX);
#if !defined(CONFIG_EFR32_RADIO_QEMU_TIFS)
	/*
	 * A window the LLL wants answered (a Peripheral's anchor, an initiator's or active
	 * scanner's window) is marked PENDING before the receiver starts, as a TX->RX turnaround's
	 * is in radio_tmr_hcto_configure(): the sequencer then requests TX at RXFRAME and holds
	 * RX2TX for the LLL's decision (SS7.65).
	 */
	if (efr32_hw_tx_after_rx_requested) {
		/* A connection receive has its reply staged (lll/lll_conn_stage.c). */
		*radio_reg32(EFR32_SEQ_RX2TX_DECISION_ADDRESS) =
			efr32_conn_staged ? EFR32_SEQ_RX2TX_STAGED : EFR32_SEQ_RX2TX_PENDING;
		efr32_hw_rx2tx_armed = true;
	}
#endif
	/* A Peripheral's window hands SWRXEN to the sequencer with its instant (SS7.82). */
	if (hold && efr32_seq_timed_post(EFR32_SEQ_TIMED_OP_SWRXEN, instant, limit)) {
		efr32_seq_timed_release = instant;
		efr32_seq_timed_deadline = (int)limit;
#if defined(CONFIG_EFR32_RADIO_TIMING_PROBE)
		EFR32_PROBE_MAX(launch_pre[0], k_cycle_get_32() - efr32_radio_probe_job_start);
#endif
		return;
	}
	if (hold) {
		efr32_radio_hold_until(instant);
	}
	*radio_reg32(EFR32_RAC_BASE + EFR32_REGISTER_SET_OFFSET +
		     EFR32_RAC_RXENSRCEN_OFFSET) = EFR32_RAC_RXENSRCEN_SWRXEN;
}

void radio_rx_enable(void)
{
	efr32_radio_rx_start(false, 0U, 0U);
}

/*
 * [regs-vendor] The generic RAIL TX helper ends by writing one to RAC.CMD.
 * Its preceding packet/FIFO setup is separate and remains unimplemented;
 * see docs/radio-tables.md §4.1.
 */
void radio_tx_enable(void)
{
	efr32_radio_tx_start(false, 0U, 0U);
}

/* TX start: with @p hold, TXEN is held to @p instant (interrupts locked by the caller). */
static void efr32_radio_tx_start(bool hold, uint32_t instant, uint32_t limit)
{
	uint32_t status_before;
	uint32_t sequencer_flags;

	/* Match the OpenISA radio contract: do not start a new TX while active. */
	while (!radio_is_idle()) {
	}
	/*
	 * The very first store of stock's own TX-start sequence (see
	 * EFR32_RAC_SR3_OFFSET's comment): clear SR3 bits 14/30 via its CLR
	 * alias, before anything else below.  NOT a full-register write --
	 * SR3's other bits hold a fixed, nonzero value this driver has not
	 * identified the origin of and must not disturb.
	 */
	*radio_reg32(EFR32_RAC_BASE + EFR32_REGISTER_CLEAR_OFFSET +
		     EFR32_RAC_SR3_OFFSET) = EFR32_RAC_SR3_TX_START_CLEAR_MASK;
	status_before = *radio_reg32(EFR32_RAC_BASE + EFR32_RAC_STATUS_OFFSET);
	efr32_radio_trace_previous_seq_launch();

	/*
	 * A watchpoint on the working controller shows per-packet WRITEDATA stores
	 * at the scheduled launch boundary, immediately before TXEN.  Keep this in
	 * the scheduler-owned job, not the timer ISR.
	 */
	efr32_tx_fifo_publish();
	/* Validate and label a completion before arming the hardware source. */
	efr32_packet_stage_arm(EFR32_RADIO_PACKET_TX);
	/*
	 * Reproduce the complete immediate pre-TX handshake recovered from the
	 * proprietary primitive at 0xe35c: set SR0 bit 7, clear SR0 bit 3, then
	 * clear the sequencer's RX-after-TX inhibit before issuing TXEN.  The M0+
	 * consumer at image offset 0x9c8 tests this bit and, when clear, writes
	 * SWRXEN after its bounded SR0 gate pulse.  An isolated replay did not make
	 * RF output observable, but that says only that this sequence is not
	 * sufficient; static control flow proves it is part of every launch.
	 */
	*radio_reg32(EFR32_RAC_BASE + EFR32_REGISTER_SET_OFFSET +
		     EFR32_RAC_SR0_OFFSET) = EFR32_RAC_SR0_TX_PREPARE_SET;
	*radio_reg32(EFR32_RAC_BASE + EFR32_REGISTER_CLEAR_OFFSET +
		     EFR32_RAC_SR0_OFFSET) = EFR32_RAC_SR0_TX_PREPARE_CLEAR;
	sequencer_flags = *radio_reg32(EFR32_SEQ_TX_PREPARE_FLAGS_ADDRESS);
	sequencer_flags &= ~EFR32_SEQ_RX_AFTER_TX_INHIBIT;
	*radio_reg32(EFR32_SEQ_TX_PREPARE_FLAGS_ADDRESS) = sequencer_flags;
	/* See EFR32_SEQ_UNNAMED_FLAG_3EEC_ADDRESS: untested hardware hypothesis. */
	*radio_reg32(EFR32_SEQ_UNNAMED_FLAG_3EEC_ADDRESS) =
		EFR32_SEQ_UNNAMED_FLAG_3EEC_OBSERVED_VALUE;
#if !defined(CONFIG_EFR32_RADIO_QEMU_TIFS)
	if (efr32_hw_rx_after_tx_requested) {
		efr32_bufc_rx_fifo_configure();
		efr32_hw_rx_fifo_preconfigured = true;
	}
#endif
#if !defined(CONFIG_EFR32_RADIO_QEMU_TIFS)
	bool rx_after = efr32_hw_rx_after_tx_requested;
#else
	bool rx_after = false;
#endif
	/*
	 * A Central's anchor turns to a receive whose reply is staged: the window is armed from
	 * the start, so no M33 work stands between the anchor's end and the reply.
	 */
	if (rx_after && efr32_conn_staged) {
		*radio_reg32(EFR32_SEQ_RX2TX_DECISION_ADDRESS) = EFR32_SEQ_RX2TX_STAGED;
	}
	/*
	 * A launch hands TXEN, and the RX request of a TX that turns to RX, to the sequencer with
	 * its instant (SS7.82): no M33 interrupt can then delay it past its spec window.
	 */
	if (hold && efr32_seq_timed_post(EFR32_SEQ_TIMED_OP_TXEN |
					 (rx_after ? EFR32_SEQ_TIMED_RX_AFTER_TX : 0U),
					 instant, limit)) {
		efr32_seq_timed_release = instant;
		efr32_seq_timed_deadline = (int)limit;
		efr32_radio_last_txen = instant;
#if defined(CONFIG_EFR32_RADIO_TIMING_PROBE)
		EFR32_PROBE_MAX(launch_pre[1], k_cycle_get_32() - efr32_radio_probe_job_start);
#endif
	} else {
		if (hold) {
			efr32_radio_hold_until(instant);
		}
		*radio_reg32(EFR32_RAC_BASE + EFR32_RAC_CMD_OFFSET) = EFR32_RAC_CMD_TXEN;
		efr32_radio_last_txen = k_cycle_get_32();
		if (rx_after) {
			*radio_reg32(EFR32_RAC_BASE + EFR32_REGISTER_SET_OFFSET +
				     EFR32_RAC_RXENSRCEN_OFFSET) = EFR32_RAC_RXENSRCEN_SWRXEN;
		}
	}
#if !defined(CONFIG_EFR32_RADIO_QEMU_TIFS)
	efr32_hw_rx_after_tx_pending = efr32_hw_rx_after_tx_requested;
	efr32_hw_rx_after_tx_requested = false;
#endif
	EFR32_RADIO_TRACE_ONCE("txen status-before=0x%08x status-after=0x%08x",
			       status_before,
			       *radio_reg32(EFR32_RAC_BASE + EFR32_RAC_STATUS_OFFSET));
}

/* [regs-public] RAC.CMD TXDIS/RXDIS are write-only command bits (T2). */
void radio_disable(void)
{
	unsigned int key;

#if defined(CONFIG_EFR32_RADIO_CONN_STAGED)
	/*
	 * An advertiser's, scanner's or initiator's isr_rx() that answers nothing ends the
	 * exchange here: the sequencer must have answered nothing either (a mismatch is a bug in
	 * its rules, seq_abi.h).
	 */
	if ((efr32_conn_flags & (EFR32_SEQ_CONN_ADV | EFR32_SEQ_CONN_SCAN)) != 0U) {
		uint32_t outcome;

		LL_ASSERT(!efr32_conn_received || efr32_radio_conn_outcome(&outcome) <= 0 ||
			  (outcome & EFR32_SEQ_CONN_OUT_REPLY) == 0U);
		LL_ASSERT(!efr32_conn_committing);
	}
#endif
	efr32_radio_launch_disarm();
	efr32_hw_turnaround_reset();
#if defined(CONFIG_EFR32_RADIO_QEMU_TIFS)
	efr32_qemu_tifs_cancel();
	efr32_qemu_tifs_next_action = EFR32_QEMU_TIFS_NEXT_NONE;
	efr32_qemu_tifs_transition_requested = false;
#endif

	/*
	 * Keep independent command writes: their combined hardware semantics are
	 * unverified. The retained stock trace issues RXDIS before TXDIS
	 * (docs/radio-tables.md §4.1), so preserve that observed order.
	 */
	key = irq_lock();
	efr32_packet_lifecycle_reset(&efr32_packet_stage.lifecycle);
	/* An aborted PDU cannot be delivered by a later receive completion. */
	efr32_packet_stage.rx_target = NULL;
	efr32_packet_stage.rx_fifo_configured = false;
	efr32_packet_stage.tx_source = NULL;
	efr32_packet_stage.tx_bytes = 0U;
	efr32_packet_stage.tx_fifo_configured = false;
	/* An abort is not a TXDONE or RXDONE; discard any pre-abort completion. */
	*radio_reg32(EFR32_FRC_BASE + EFR32_REGISTER_CLEAR_OFFSET +
		     EFR32_FRC_IF_OFFSET) = EFR32_FRC_IF_COMPLETION;
	/*
	 * [I] Withdraw the software RX source first: on hardware, RXDIS with
	 * RXENSRCEN.SWRXEN still asserted left the RAC in RXSEARCH, and the next
	 * radio_tx_enable() then waited for idle forever (SS7.63).
	 */
	*radio_reg32(EFR32_RAC_BASE + EFR32_REGISTER_CLEAR_OFFSET +
		     EFR32_RAC_RXENSRCEN_OFFSET) = EFR32_RAC_RXENSRCEN_SWRXEN;
	*radio_reg32(EFR32_RAC_BASE + EFR32_RAC_CMD_OFFSET) = EFR32_RAC_CMD_RXDIS;
	*radio_reg32(EFR32_RAC_BASE + EFR32_RAC_CMD_OFFSET) = EFR32_RAC_CMD_TXDIS;
	/*
	 * OpenISA relies on radio_disable() to enter the installed abort/cleanup
	 * callback.  FRC has no disable-complete latch, so pend its already-owned
	 * IRQ as a software event and make isr_radio() distinguish it from a PDU
	 * completion above.
	 */
	if (efr32_radio_abort_request(&efr32_radio_abort)) {
		k_irq_set_pending(LL_RADIO_IRQn);
	}
	irq_unlock(key);
}

/* [regs-public] FRC.IF has named TXDONE and RXDONE completion latches. */
void radio_status_reset(void)
{
	bool completed;
	enum efr32_radio_packet_direction completed_direction;

	/* A staged reply's TXDONE may already be latched: it is its own completion. */
	*radio_reg32(EFR32_FRC_BASE + EFR32_REGISTER_CLEAR_OFFSET +
		     EFR32_FRC_IF_OFFSET) = efr32_conn_in_flight ? EFR32_FRC_IF_RXDONE :
							       EFR32_FRC_IF_COMPLETION;
	/* The LLL has sampled completion/CRC state before asking to clear it. */
	completed_direction = efr32_packet_stage.lifecycle.completion_direction;
	completed = efr32_packet_lifecycle_status_reset(&efr32_packet_stage.lifecycle);
#if defined(CONFIG_EFR32_RADIO_QEMU_TIFS)
	/*
	 * A TX-completion callback prepares the RX packet and timeout after it
	 * clears status. Claim the RX prearm now, before that callback records a
	 * later RX-to-TX request. The HCTO call made by the callback consumes this
	 * flag and arms the bounded BURTC RX job.
	 */
	if (completed && completed_direction == EFR32_RADIO_PACKET_TX &&
	    efr32_qemu_tifs_next_action_claim(&efr32_qemu_tifs_next_action,
					       EFR32_QEMU_TIFS_NEXT_RX)) {
		efr32_qemu_tifs_transition_requested = true;
	}
#endif
#if !defined(CONFIG_EFR32_RADIO_QEMU_TIFS)
	if (completed && completed_direction == EFR32_RADIO_PACKET_TX &&
	    efr32_hw_rx_after_tx_pending) {
		efr32_hw_rx_after_tx_pending = false;
		efr32_hw_rx_turnaround_claimed = true;
	}
	/*
	 * No unserved-response guard here any more: the LLL either stages a
	 * response or calls radio_disable(), and the sequencer bounds its own
	 * wait.  Arming a BURTC compare on this path cost ~119 us of the T_IFS
	 * budget (LF-domain synchronisation; SS7.68).
	 */
	if (completed && completed_direction == EFR32_RADIO_PACKET_RX) {
		efr32_hw_tx_after_rx_requested = false;
	}
#endif
	efr32_rx_delivery_invalid = false;
#if defined(CONFIG_EFR32_RADIO_QEMU_RX_DESCRIPTOR)
	efr32_qemu_rx_descriptor.descriptor_crc_valid = false;
#endif
}

/*
 * [software] OpenISA requires this to remain false while it stages a packet.
 * The reference RV32M1 HAL deliberately returns false regardless of its
 * hardware state; RAC state must not be substituted for that API contract.
 */
uint32_t radio_is_ready(void)
{
	return 0;
}

/* [regs-public] FRC.IF.TXDONE/RXDONE are the completion latches (T2). */
uint32_t radio_is_done(void)
{
	return !!(*radio_reg32(EFR32_FRC_BASE + EFR32_FRC_IF_OFFSET) &
		 EFR32_FRC_IF_COMPLETION);
}

/* [software] Unused by the OpenISA LLL; matches the reference HAL. */
uint32_t radio_has_disabled(void)
{
	return 0;
}

/* [hardware] OFF and retained SHUTDOWN are both quiescent launch states. */
uint32_t radio_is_idle(void)
{
	uint32_t state = (*radio_reg32(EFR32_RAC_BASE + EFR32_RAC_STATUS_OFFSET) >>
			  EFR32_RAC_STATUS_STATE_SHIFT) & EFR32_RAC_STATUS_STATE_MASK;

	return state == EFR32_RAC_STATE_OFF || state == EFR32_RAC_STATE_SHUTDOWN;
}

/*
 * [regs-public] RFCRC values match BLE's 24-bit CRC after bit reversal (T5).
 *
 * A hardware-lockstep diff against stock (docs/radio-tables.md SS7.43)
 * found RFCRC.DATA -- the live CRC accumulator -- reads 0 on the open
 * stack right before TXEN, while stock's reads back the just-written
 * INIT value. CMD.INITIALIZE (write-only, bit 0) is the public command
 * that loads DATA from INIT; this function configured EN/CTRL/INIT/POLY
 * but never issued it. Confirmed directly on hardware: issuing it here
 * makes DATA read INIT immediately afterward, matching stock. Does NOT
 * explain the ACTIVETXFCD stall (tested on hardware, no change to
 * RAC.STATUS/FRC.STATUS/BUFC.READOFFSET -- SS7.43), but is a genuine,
 * independently confirmed divergence from stock's real CRC-engine
 * initialization, worth fixing on its own merits.
 */
void radio_crc_configure(uint32_t polynomial, uint32_t iv)
{
	LL_ASSERT((polynomial & ~0x00FFFFFFU) == 0U);
	LL_ASSERT((iv & ~0x00FFFFFFU) == 0U);
	*radio_reg32(EFR32_RFCRC_BASE + EFR32_RFCRC_EN_OFFSET) = 1U;
	*radio_reg32(EFR32_RFCRC_BASE + EFR32_RFCRC_CTRL_OFFSET) = EFR32_RFCRC_BLE_CTRL;
	*radio_reg32(EFR32_RFCRC_BASE + EFR32_RFCRC_INIT_OFFSET) = bit_reverse24(iv);
	*radio_reg32(EFR32_RFCRC_BASE + EFR32_RFCRC_POLY_OFFSET) = bit_reverse24(polynomial);
	*radio_reg32(EFR32_RFCRC_BASE + EFR32_RFCRC_CMD_OFFSET) =
		EFR32_RFCRC_CMD_INITIALIZE;
}

/*
 * FRC.TRAILRXDATA.CRCOK is the hardware CRC result.  Gate it with the
 * private staging result so a future rejected normalized PDU follows
 * OpenISA's completed-but-invalid receive path without writing CRCOK.
 */
uint32_t radio_crc_is_valid(void)
{
	#if defined(CONFIG_EFR32_RADIO_QEMU_RX_DESCRIPTOR)
	if (efr32_qemu_rx_descriptor.descriptor_crc_valid) {
		return 1U;
	}
	#endif
#if defined(CONFIG_EFR32_RADIO_QEMU_RX_DESCRIPTOR)
	return !efr32_rx_delivery_invalid &&
	       !!(*radio_reg32(EFR32_FRC_BASE + EFR32_FRC_TRAILRXDATA_OFFSET) &
		 EFR32_FRC_TRAILRXDATA_CRCOK);
#else
	return !efr32_rx_delivery_invalid && efr32_rx_crc_ok;
#endif
}

/* [software] software (no register access needed); not implemented yet */
void *radio_pkt_empty_get(void)
{
	return pkt_empty;
}

/* [software] software (no register access needed); not implemented yet */
void *radio_pkt_scratch_get(void)
{
	return pkt_scratch;
}

/*
 * OpenISA's radio_switch_complete_and_tx() is called in two distinct
 * contexts: before radio_tmr_start() during event setup, and from the
 * packet-complete ISR for an RX/TX turn-around (see its radio.c:231-233,
 * 350-354, and 937-945).  The latter does not issue RAC.CMD immediately.
 * It records the next sequencer command and, after the packet-end capture,
 * programs T1 for end + TIFS - warmup.  A conforming EFR32 implementation
 * therefore needs all of these state inputs before it can arm a switch:
 *
 *   - a source-backed packet-end timestamp;
 *   - the requested TIFS interval; and
 *   - PHY-specific RX/TX warm-up/chain-delay values and a proven
 *     PROTIMER-to-RAC command path.
 *
 * This port currently has only the BURTC event-start alarm.  The end-capture
 * API records a request (radio_tmr_end_capture()), but no captured timestamp
 * is available, and QEMU models no autonomous TIFS sequencer.  Do not turn
 * this into an immediate RAC command: that would change the OpenISA
 * packet-complete contract and would not be evidence-consistent.
 */
void radio_switch_complete_and_rx(uint8_t phy_rx)
{
	ARG_UNUSED(phy_rx);
	efr32_hw_rx_after_tx_requested = true;
	efr32_hw_tx_after_rx_requested = false;
#if defined(CONFIG_EFR32_RADIO_QEMU_TIFS)
	/*
	 * This is an instruction for the operation following the one currently
	 * being prepared. For example, advertising records RX before its initial
	 * TX; an active scanner records RX before it stages its SCAN_REQ. Do not
	 * tie that instruction to a completion marker: the LLL intentionally
	 * replaces it while handling the preceding completion.
	 */
	efr32_qemu_tifs_next_action_set(&efr32_qemu_tifs_next_action,
					EFR32_QEMU_TIFS_NEXT_RX);
#endif
}

/* See radio_switch_complete_and_rx() for the bounded implementation contract. */
void radio_switch_complete_and_tx(uint8_t phy_rx, uint8_t flags_rx, uint8_t phy_tx,
				  uint8_t flags_tx)
{
	ARG_UNUSED(phy_rx);
	ARG_UNUSED(flags_rx);
	ARG_UNUSED(phy_tx);
	ARG_UNUSED(flags_tx);
	efr32_hw_rx_after_tx_requested = false;
	efr32_hw_tx_after_rx_requested = true;
#if defined(CONFIG_EFR32_RADIO_QEMU_TIFS)
	/*
	 * A scanner or advertiser may record this before RX begins. A real RXDONE
	 * claims it before invoking the LLL callback, gives that callback time to
	 * stage its response PDU, then lets the bounded QEMU TIFS job issue TXEN.
	 */
	efr32_qemu_tifs_next_action_set(&efr32_qemu_tifs_next_action,
					EFR32_QEMU_TIFS_NEXT_TX);
#endif
}

/* [regs] software TIFS on top of PROTIMER compare + RAC commands (T2, T8) */
void radio_switch_complete_and_disable(void)
{
	efr32_hw_rx_after_tx_requested = false;
	efr32_hw_tx_after_rx_requested = false;
#if defined(CONFIG_EFR32_RADIO_QEMU_TIFS)
	/*
	 * This clears the sequencer command that has not been consumed yet. It
	 * must not cancel an RXDONE-claimed reservation: lll_adv uses this call
	 * immediately before it stages the SCAN_RSP that owns that reservation.
	 */
	efr32_qemu_tifs_next_action_clear(&efr32_qemu_tifs_next_action);
#endif
}

/* [regs-public] AGC.CMD.RSSISTART requests a live RSSI measurement (T9). */
void radio_rssi_measure(void)
{
	*radio_reg32(EFR32_AGC_BASE + EFR32_AGC_CMD_OFFSET) =
		EFR32_AGC_CMD_RSSISTART;
}

/*
 * RAIL reads FRAMERSSI rather than the live RSSI register for a received
 * packet. The encoded 10-bit value is signed quarter-dB with RAIL's
 * source-backed -200 quarter-dB offset. OpenISA stores RSSI as a positive
 * magnitude, so negate the resulting signed dB value at the API boundary.
 */
uint32_t radio_rssi_get(void)
{
	int32_t code = (*radio_reg32(EFR32_AGC_BASE + EFR32_AGC_FRAMERSSI_OFFSET) >>
			EFR32_AGC_RSSI_CODE_SHIFT) & EFR32_AGC_RSSI_CODE_MASK;
	int32_t rssi_quarter_db;
	int32_t rssi_db;

	if (code & 0x200) {
		code -= 0x400;
	}
	if (code == EFR32_AGC_RSSI_INVALID) {
		return 127U;
	}

	rssi_quarter_db = code - EFR32_AGC_RSSI_OFFSET_QUARTER_DB;
	if (rssi_quarter_db < -511) {
		rssi_quarter_db = -511;
	}
	rssi_db = rssi_quarter_db / 4;

	return rssi_db < 0 ? (uint32_t)-rssi_db : 0U;
}

/* [regs-public] AGC.IF.RSSIVALID is the public RSSI-valid status latch (T9). */
void radio_rssi_status_reset(void)
{
	*radio_reg32(EFR32_AGC_BASE + EFR32_REGISTER_CLEAR_OFFSET +
		     EFR32_AGC_IF_OFFSET) = EFR32_AGC_IF_RSSIVALID;
}

/* [regs-public] AGC.IF.RSSIVALID records availability of a live RSSI result (T9). */
uint32_t radio_rssi_is_ready(void)
{
	return !!(*radio_reg32(EFR32_AGC_BASE + EFR32_AGC_IF_OFFSET) &
		 EFR32_AGC_IF_RSSIVALID);
}

/* [software] OpenISA's reference HAL intentionally leaves software filtering disabled. */
void radio_filter_configure(uint8_t bitmask_enable, uint8_t bitmask_addr_type,
			    uint8_t *bdaddr)
{
	ARG_UNUSED(bitmask_enable);
	ARG_UNUSED(bitmask_addr_type);
	ARG_UNUSED(bdaddr);
}

/* [software] No filter hardware or software matcher is enabled by this LLL path. */
void radio_filter_disable(void)
{
}

/* [software] No match latch exists while filtering is disabled. */
void radio_filter_status_reset(void)
{
}

/* [software] Matches the OpenISA reference's permanently no-match filter path. */
uint32_t radio_filter_has_match(void)
{
	return 0;
}

/* [software] No filter match index exists while filtering is disabled. */
uint32_t radio_filter_match_get(void)
{
	return 0;
}

/* [software] OpenISA's reference does not configure a bit counter here. */
void radio_bc_configure(uint32_t n)
{
	ARG_UNUSED(n);
}

/* [software] No bit-counter status is produced by this port. */
void radio_bc_status_reset(void)
{
}

/* [software] Matches the OpenISA reference's permanently no-match bit counter. */
uint32_t radio_bc_has_match(void)
{
	return 0;
}

static uint32_t efr32_radio_launch_schedule(uint8_t trx, uint32_t tick, uint32_t remainder)
{
	uint32_t generation;
	uint32_t supplied_remainder = remainder;

	if (!(remainder / 1000000UL) || (remainder & BIT(31))) {
		tick--;
		remainder += HAL_TICKER_CNTR_CLK_UNIT_FSEC / 1000UL;
	}
	remainder /= 1000000UL;
	/* Do not let an unread expiry from the replaced launch run this one. */
	efr32_radio_launch_disarm();
	do {
		generation = (uint32_t)atomic_inc(&efr32_radio_launch_generation) + 1U;
	} while (!generation);
	atomic_set(&efr32_radio_launch_active, generation);
	efr32_radio_event_tx = trx != 0U;
	efr32_radio_timer_state_start_set(&efr32_radio_timer, tick);
	/*
	 * The launch is due at the start tick plus the remainder, like OpenISA's T1 compare
	 * (EVENT_TMR counts from the tick): the remainder carries the anchor's sub-tick part, which
	 * a Central's anchor points must keep to the sleep clock's 16 us (Vol 6 Part B 4.2.2).
	 */
	efr32_radio_start_cyc = cntr_tick_cycles(tick);
	efr32_radio_launch_cyc = efr32_radio_start_cyc + k_us_to_cyc_floor32(remainder);
#if defined(CONFIG_EFR32_RADIO_CONN_STAGED)
	/* An initiator's receive staged ahead of its start: its slot check counts from here. */
	if (efr32_conn_staged && (efr32_conn_flags & EFR32_SEQ_CONN_INIT_TIMED) != 0U) {
		efr32_scan_start_write();
	}
#endif
	efr32_radio_ready_us = remainder + (trx ? radio_tx_ready_delay_get(0U, 0U) :
						  radio_rx_ready_delay_get(0U, 0U));
	efr32_radio_rx_eof_valid = false;
	EFR32_RADIO_TRACE_ONCE("schedule trx=%u tick=%u remainder=%u generation=%u",
			       trx, tick, supplied_remainder, generation);
	cntr_radio_cmp_set(trx, efr32_radio_launch_cyc,
			   trx ? 0U : efr32_radio_rx_window_tolerance_us, generation);
	efr32_radio_rx_window_tolerance_us = 0U;
	return remainder;
}

/* [software] OpenISA capture-request bookkeeping; PROTIMER capture remains unknown (T8). */
void radio_tmr_status_reset(void)
{
	efr32_radio_timer_state_status_reset(&efr32_radio_timer);
}

/* [software] Retain TIFS requested by the LLL; programming it remains unknown (T2, T8). */
void radio_tmr_tifs_set(uint32_t tifs)
{
	efr32_radio_timer_state_tifs_set(&efr32_radio_timer, tifs);
}

/* [software] Schedule the normal TX/RX enable path through the shared BURTC job bridge. */
uint32_t radio_tmr_start(uint8_t trx, uint32_t ticks_start, uint32_t remainder)
{
	return efr32_radio_launch_schedule(trx, ticks_start, remainder);
}

/* [software] Schedule a whole-tick TX/RX launch through the shared BURTC job bridge. */
uint32_t radio_tmr_start_tick(uint8_t trx, uint32_t tick)
{
	return efr32_radio_launch_schedule(trx, tick, 1000000UL);
}

/* [software] Return the retained controller tick; no hardware read is implied. */
void radio_tmr_start_us(uint8_t trx, uint32_t us)
{
	ARG_UNUSED(trx);
	ARG_UNUSED(us);
	EFR32_RADIO_AIR_TODO("PROTIMER (timebase, compare, captures) and RTC syn");
}

/* [software] Request a later AA capture; capture itself remains unknown (T8). */
uint32_t radio_tmr_start_now(uint8_t trx)
{
	ARG_UNUSED(trx);
	EFR32_RADIO_AIR_TODO("regs");
	return 0;
}

/* [software] Keep LLL-supplied AA timestamp independently of hardware capture (T8). */
uint32_t radio_tmr_start_get(void)
{
	return efr32_radio_timer.start_tick;
}

/* [software] Restore LLL-supplied AA timestamp; no hardware read is implied. */
void radio_tmr_stop(void)
{
	efr32_radio_launch_disarm();
	efr32_hw_turnaround_reset();
#if defined(CONFIG_EFR32_RADIO_QEMU_TIFS)
	efr32_qemu_tifs_cancel();
	efr32_qemu_tifs_next_action = EFR32_QEMU_TIFS_NEXT_NONE;
	efr32_qemu_tifs_transition_requested = false;
#endif
}

/*
 * OpenISA arms GENFSK.T2_CMP with an EVENT_TMR-relative value here
 * (vendor/zephyr/.../radio.c:1149-1173).  Its callers compute that value from
 * radio_tmr_tifs_base_get() and PHY/address delays (for example
 * lll_adv.c:552-557 and lll_peripheral.c:197-211).  Those inputs are not a
 * BURTC timestamp: this port's start path coarsens the fractional launch
 * remainder, and its packet-end/TIFS base remains unresolved.
 *
 * Keep the request as software bookkeeping, but do not arm the shared BURTC
 * compare or synthesize a timeout IRQ.  A correct scheduler-owned timeout
 * still needs an event epoch conversion, cancellation/generation arbitration
 * with the launch compare, and a bounded timeout completion job.  QEMU also
 * intentionally models no PROTIMER compare flag or autonomous RAC timeout.
 */
void radio_tmr_hcto_configure(uint32_t hcto)
{
	efr32_radio_timer_state_hcto_set(&efr32_radio_timer, hcto);
#if !defined(CONFIG_EFR32_RADIO_QEMU_TIFS)
	if (efr32_hw_rx_turnaround_claimed) {
		/* The RAC is already turning around; label the next completion. */
		efr32_hw_rx_turnaround_claimed = false;
		/*
		 * After a staged reply the sequencer armed this receive itself, and its frame may
		 * already have completed: keep that latch.
		 */
		efr32_packet_stage_arm_keep(EFR32_RADIO_PACKET_RX, !efr32_conn_staged);
		if (efr32_hw_tx_after_rx_requested) {
			*radio_reg32(EFR32_SEQ_RX2TX_DECISION_ADDRESS) =
				efr32_conn_staged ? EFR32_SEQ_RX2TX_STAGED : EFR32_SEQ_RX2TX_PENDING;
			efr32_hw_rx2tx_armed = true;
			EFR32_PROBE_PATH(to_pending);
		}
		/* As in the QEMU path, HCTO bounds the window from this point. */
		cntr_rx_timeout_arm_after_us(hcto,
			(uint32_t)atomic_inc(&efr32_hw_hcto_generation) + 1U);
	} else if (!efr32_radio_event_tx && atomic_get(&efr32_radio_launch_active) != 0) {
		/*
		 * The receive window of a launch being prepared (a Peripheral's): OpenISA's HCTO is
		 * on the event timer, i.e. from the start tick. lll_peripheral.c sets it to the
		 * window's end plus windowWidening plus the address time (Vol 6 Part B 4.2.4:
		 * listen until windowWidening after receiveWindowEnd); the expiry only ends a
		 * search, never a frame being received, and a timer can only fire late.
		 */
		cntr_rx_timeout_arm_at(efr32_radio_start_cyc + k_us_to_cyc_ceil32(hcto),
			(uint32_t)atomic_inc(&efr32_hw_hcto_generation) + 1U);
	}
#endif
#if defined(CONFIG_EFR32_RADIO_QEMU_TIFS)
	if (!efr32_qemu_tifs_transition_requested) {
		/* Initial event setup has no post-TXDONE transition marker. */
		return;
	}
	/*
	 * This arm is intentionally based on the TX-completion marker rather than
	 * on a guessed hardware timestamp. It is a QEMU-only scheduler contract;
	 * hardware builds retain the bookkeeping-only behavior above.
	 */
	efr32_qemu_tifs_transition_requested = false;
	efr32_radio_timer.hcto_requested = true;
	efr32_qemu_tifs_arm(hcto);
#endif
}

/* [regs] PROTIMER (timebase, compare, captures) and RTC sync: unknown (T8) */
void radio_tmr_aa_capture(void)
{
	efr32_radio_timer_state_aa_capture_request(&efr32_radio_timer);
}

/* Event-timer microseconds (from the start tick) of scheduler-clock time @p cyc. */
static uint32_t efr32_radio_event_us(uint32_t cyc)
{
	return k_cyc_to_us_floor32(cyc - efr32_radio_start_cyc);
}

/*
 * End of the access address of the frame last received: its MODEM EOF less the PDU and CRC
 * at LE 1M (Vol 6 Part B 2.1). Without a captured frame end (QEMU-configuration images: the
 * QEMU-only descriptor seam has no air time) it is 0, as before.
 */
uint32_t radio_tmr_aa_get(void)
{
	if (!efr32_radio_rx_eof_valid) {
		return 0;
	}
	return radio_tmr_end_get() -
	       (efr32_radio_rx_pdu_octets + BT_LE_CRC_OCTETS) * BT_LE_1M_US_PER_OCTET;
}

/* [regs] PROTIMER (timebase, compare, captures) and RTC sync: unknown (T8) */
void radio_tmr_aa_save(uint32_t aa)
{
	efr32_radio_timer_state_aa_save(&efr32_radio_timer, aa);
}

/* [regs] PROTIMER (timebase, compare, captures) and RTC sync: unknown (T8) */
uint32_t radio_tmr_aa_restore(void)
{
	return efr32_radio_timer.saved_aa;
}

/*
 * When the radio was planned to be ready, as OpenISA's (remainder + warm-up): a timed launch
 * holds its command to the instant, so the plan is what happened, within the launch's
 * measured hold lateness.
 */
uint32_t radio_tmr_ready_get(void)
{
	return efr32_radio_ready_us;
}

/* [regs] PROTIMER (timebase, compare, captures) and RTC sync: unknown (T8) */
void radio_tmr_end_capture(void)
{
	efr32_radio_timer_state_end_capture_request(&efr32_radio_timer);
}

/*
 * MODEM EOF of the frame last received (efr32_radio_rx_end_capture()); 0 without one. The
 * receiver's chain delay is not separated from the transmitter's (docs/radio-tables.md 7.71:
 * only d_tx + d_rx is measured), so every receive-side time here is the MODEM's: the anchor the
 * Peripheral derives from it and the window it opens are both late by the same d_rx.
 */
uint32_t radio_tmr_end_get(void)
{
	if (!efr32_radio_rx_eof_valid) {
		return 0;
	}
	return efr32_radio_event_us(efr32_radio_rx_eof_cyc);
}

void efr32_radio_conn_event_set(uint32_t interval_us)
{
	efr32_radio_conn_interval_us = interval_us;
}

void efr32_radio_rx_window_set(uint32_t tolerance_us)
{
	efr32_radio_rx_window_tolerance_us = tolerance_us;
}

/* [regs] software TIFS on top of PROTIMER compare + RAC commands (T2, T8) */
uint32_t radio_tmr_tifs_base_get(void)
{
	EFR32_RADIO_TODO("regs");
	return 0;
}

/* [regs] PROTIMER (timebase, compare, captures) and RTC sync: unknown (T8) */
void radio_tmr_sample(void)
{
	EFR32_RADIO_TODO("PROTIMER (timebase, compare, captures) and RTC syn");
}

/* [regs] PROTIMER (timebase, compare, captures) and RTC sync: unknown (T8) */
uint32_t radio_tmr_sample_get(void)
{
	EFR32_RADIO_TODO("regs");
	return 0;
}

/* [software] software (no register access needed); not implemented yet */
void radio_gpio_pa_setup(void)
{
	/* No front-end module on the target board. */
}

/* [software] software (no register access needed); not implemented yet */
void radio_gpio_lna_setup(void)
{
	/* No front-end module on the target board. */
}

/* [software] software (no register access needed); not implemented yet */
void radio_gpio_lna_on(void)
{
	/* No front-end module on the target board. */
}

/* [software] software (no register access needed); not implemented yet */
void radio_gpio_lna_off(void)
{
	/* No front-end module on the target board. */
}

/* [software] software (no register access needed); not implemented yet */
void radio_gpio_pa_lna_enable(uint32_t trx_us)
{
	ARG_UNUSED(trx_us);
	/* No front-end module on the target board. */
}

/* [software] software (no register access needed); not implemented yet */
void radio_gpio_pa_lna_disable(void)
{
	/* No front-end module on the target board. */
}

/* [hardware: RADIOAES] Synchronous BLE AES-CCM; the decrypt runs in radio_ccm_is_done(). */
void *radio_ccm_rx_pkt_set(struct ccm *ccm, uint8_t phy, void *pkt)
{
	ARG_UNUSED(phy);
	memcpy(&ccm_ctx.config, ccm, sizeof(*ccm));
	ccm_ctx.rx_in = (struct pdu_data *)pkt_scratch;
	ccm_ctx.rx_out = pkt;
	ccm_ctx.pending_rx = true;
	ccm_ctx.done = false;
	ccm_ctx.mic_valid = false;

	return pkt_scratch;
}

/* [hardware: RADIOAES] Encrypts synchronously; returned storage remains valid until the next
 * radio packet.
 */
void *radio_ccm_tx_pkt_set(struct ccm *ccm, void *pkt)
{
	struct pdu_data *in = pkt;
	size_t len = in->len;

	ccm_ctx.pending_rx = false;
	ccm_ctx.done = false;
	ccm_ctx.mic_valid = false;
#if defined(CONFIG_EFR32_RADIO_CONN_STAGED)
	/* A staged reply went out encrypted when it was staged: the image it was sent from. */
	if (efr32_conn_committing) {
		efr32_conn_reply_header = *(uint8_t *)pkt;
		ccm_ctx.done = true;
		return efr32_conn_image[(efr32_conn_outcome_word & EFR32_SEQ_CONN_OUT_ACK) != 0U];
	}
#endif
	if (len == 0U) {
		ccm_ctx.done = true;
		return pkt;
	}
	if (len + BLE_CCM_MIC_SIZE > sizeof(pkt_scratch) - PDU_DC_LL_HEADER_SIZE ||
	    ccm_run(true, ccm, pkt, pkt_scratch, len,
		    pkt_scratch + PDU_DC_LL_HEADER_SIZE + len) != 0) {
		return NULL;
	}

	pkt_scratch[0] = *(uint8_t *)pkt;
	pkt_scratch[1] = len + BLE_CCM_MIC_SIZE;
	ccm_ctx.done = true;
	return pkt_scratch;
}

/*
 * [hardware: RADIOAES] Finish an encrypted receive into the LLL's PDU: called by the completion
 * job before the LLL's RX callback runs. lll_conn.c (isr_rx_pdu()) reads the received header's
 * SN and NESN from that PDU before it asks radio_ccm_is_done(), and asks only for a non-empty
 * PDU, as a CCM that runs while the frame arrives (Nordic's) allows; the frame itself landed in
 * pkt_scratch (radio_ccm_rx_pkt_set()). An empty PDU carries no MIC (Core v6.1 Vol 6 Part E 1)
 * and is copied; a frame with a CRC error is not decrypted (the LLL does not use it); a PDU too
 * short for a MIC, or whose MIC does not verify, completes as a MIC failure with the received
 * header in place (lll_conn.c then checks for an unencrypted control PDU in pkt_scratch).
 */
static void efr32_ccm_rx_complete(void)
{
	size_t len;

	if (!ccm_ctx.pending_rx) {
		return;
	}
	ccm_ctx.pending_rx = false;
	ccm_ctx.done = true;
	ccm_ctx.mic_valid = false;
	memcpy(ccm_ctx.rx_out, ccm_ctx.rx_in, PDU_DC_LL_HEADER_SIZE);
	if (ccm_ctx.rx_in->len == 0U) {
		ccm_ctx.mic_valid = true;
		return;
	}
	if (!efr32_rx_crc_ok || ccm_ctx.rx_in->len <= BLE_CCM_MIC_SIZE) {
		return;
	}

	len = ccm_ctx.rx_in->len - BLE_CCM_MIC_SIZE;
	if (ccm_run(false, &ccm_ctx.config, (const uint8_t *)ccm_ctx.rx_in,
		    (uint8_t *)ccm_ctx.rx_out, len,
		    (uint8_t *)ccm_ctx.rx_in + PDU_DC_LL_HEADER_SIZE + len) != 0) {
		return;
	}

	ccm_ctx.rx_out->len = len;
	ccm_ctx.mic_valid = true;
}

/* [hardware: RADIOAES] The decrypt ran before the LLL callback (efr32_ccm_rx_complete()). */
uint32_t radio_ccm_is_done(void)
{
	efr32_ccm_rx_complete();
	return ccm_ctx.done;
}

/* [software] */
uint32_t radio_ccm_mic_is_valid(void)
{
	return ccm_ctx.mic_valid;
}

/*
 * [hardware: RADIOAES] lll_conn_pdu_tx_prep() sends an empty PDU (MD set) instead of data while
 * this is false: on the RV32M1 one CAU both decrypts the received PDU and encrypts the next, so
 * its HAL reports the unit free only after an empty (undecrypted) receive. RADIOAES encrypts
 * synchronously in radio_ccm_tx_pkt_set() whatever the last receive was, so it is always free.
 * Returning the RV32M1's answer here stalled every encrypted transmission that could not follow
 * an empty receive within one event, LL_START_ENC_RSP first (QEMU connection pair).
 */
uint32_t radio_ccm_is_available(void)
{
	return 1U;
}

/* [software] Snapshot AAR-order IRKs for the next normalized advertising PDU. */
void radio_ar_configure(uint32_t nirk, void *irk)
{
#if defined(CONFIG_BT_CTLR_PRIVACY)
	uint32_t count = MIN(nirk, (uint32_t)CONFIG_BT_CTLR_RL_SIZE);
	uint32_t i;

	LL_ASSERT(irk != NULL || count == 0U);
	for (i = 0U; i < count; i++) {
		sys_memcpy_swap(efr32_radio_ar.irks[i],
				(uint8_t *)irk + (i * sizeof(efr32_radio_ar.irks[i])),
				sizeof(efr32_radio_ar.irks[i]));
	}
	efr32_radio_ar.count = (uint8_t)count;
	efr32_radio_ar.match = EFR32_RADIO_AR_NO_MATCH;
	efr32_radio_ar.armed = true;
#else
	ARG_UNUSED(nirk);
	ARG_UNUSED(irk);
#endif
}

/* [software] Return the index in the last configuration snapshot. */
uint32_t radio_ar_match_get(void)
{
#if defined(CONFIG_BT_CTLR_PRIVACY)
	return efr32_radio_ar.match;
#else
	return 0U;
#endif
}

/* [software] Disarm matching and discard the last result. */
void radio_ar_status_reset(void)
{
#if defined(CONFIG_BT_CTLR_PRIVACY)
	efr32_radio_ar.armed = false;
	efr32_radio_ar.match = EFR32_RADIO_AR_NO_MATCH;
#endif
}

/* [software] A match exists only after normalized-PDU delivery ran ah(). */
uint32_t radio_ar_has_match(void)
{
#if defined(CONFIG_BT_CTLR_PRIVACY)
	return efr32_radio_ar.match != EFR32_RADIO_AR_NO_MATCH;
#else
	return 0U;
#endif
}

/* [software] Immediate reference-counted availability; hardware sleep is unknown (T1, T2). */
uint32_t radio_sleep(void)
{
	atomic_val_t previous;

	/* Do not underflow: callers use -EALREADY to diagnose unmatched sleep. */
	do {
		previous = atomic_get(&efr32_radio_power_ref);
		if (previous == 0) {
			return (uint32_t)-EALREADY;
		}
	} while (!atomic_cas(&efr32_radio_power_ref, previous, previous - 1));

	return 0;
}

/* [software] Immediate reference-counted availability; hardware wake is unknown (T1, T2). */
uint32_t radio_wake(void)
{
	atomic_inc(&efr32_radio_power_ref);
	return 0;
}

/* [software] This is deliberately not inferred from RAC STATUS.STATE. */
uint32_t radio_is_off(void)
{
	return atomic_get(&efr32_radio_power_ref) == 0;
}

#if defined(CONFIG_EFR32_RADIO_AMATEUR)
/*
 * Amateur operation (docs/amateur-radio.md): what the radio can do, and the Morse
 * identification sent by on-off keying the carrier.
 *
 * The synthesizer range is the one measured to calibrate and lock (docs/radio-tables.md
 * SS7.85); outside it the plan is refused, so the radio never transmits off frequency.
 */
/*
 * [F] SS7.85: the M0+'s VCO calibration converged, far from either end of its 11-bit code,
 * at every carrier probed from 2290 to 2500 MHz (receive only, 5 MHz steps; code 0x58A at
 * 2290 MHz to 0x1A8 at 2500 MHz), and the synthesizer reported lock throughout transmissions
 * at the carriers tested (SS7.85). The datasheet specifies only 2400-2483.5 MHz (BGM220P
 * Table 4.5, FRANGE). Outside the probed range nothing was measured.
 */
#define EFR32_AMATEUR_SYNTH_MIN_KHZ 2290000U
#define EFR32_AMATEUR_SYNTH_MAX_KHZ 2500000U
/* BGM220P datasheet Table 4.5: POUTMAX, 8 dBm output power part, typical 8.2 dBm. */
#define EFR32_AMATEUR_MAX_POWER_MDBM 8200
/*
 * Guard either side of the carrier that stays inside the band: the first offset of the Bluetooth
 * transmit spectrum mask (Core v6.1 Vol 6 Part A 3.2.2, Tables 3.3 and 3.4), where the power in
 * 1 MHz is at most -20 dBm: 2 MHz with 1 Msym/s (LE 1M, LE Coded), 4 MHz with 2 Msym/s.
 */
#define EFR32_AMATEUR_HALF_BW_KHZ_1MSYM 2000U
#define EFR32_AMATEUR_HALF_BW_KHZ_2MSYM 4000U

const struct efr32_amateur_hw efr32_radio_amateur_hw = {
	.synth_min_khz = EFR32_AMATEUR_SYNTH_MIN_KHZ,
	.synth_max_khz = EFR32_AMATEUR_SYNTH_MAX_KHZ,
	.max_power_mdbm = EFR32_AMATEUR_MAX_POWER_MDBM,
};

const uint32_t efr32_radio_amateur_half_bw_khz =
	IS_ENABLED(CONFIG_BT_CTLR_PHY_2M) ? EFR32_AMATEUR_HALF_BW_KHZ_2MSYM :
					     EFR32_AMATEUR_HALF_BW_KHZ_1MSYM;

/*
 * On-off keying (EFR32xG22 reference manual 5.1.2: "ASK / OOK" among the transmit modulation
 * modes). MODEM.CTRL0.MODFORMAT selects it (public register model MODEM_S_field.py: bits 8:6;
 * the configurator's calc_demodulator.py encodes OOK/ASK as 6). The Morse key pattern is sent as
 * OOK symbols, EFR32_CW_SYMBOLS_PER_UNIT per Morse unit, so keying edges fall on symbol
 * boundaries of a rate the modem supports (the reference manual: "below 100 Baud/s to 2
 * MBaud/s").
 */
#define EFR32_MODEM_CTRL0_OFFSET 0x020U
#define EFR32_MODEM_CTRL0_MODFORMAT_SHIFT 6U
#define EFR32_MODEM_CTRL0_MODFORMAT_MASK (0x7U << EFR32_MODEM_CTRL0_MODFORMAT_SHIFT)
#define EFR32_MODEM_CTRL0_MODFORMAT_OOKASK (6U << EFR32_MODEM_CTRL0_MODFORMAT_SHIFT)
/* MODEM.TXBR: TXBRNUM bits 15:0, TXBRDEN bits 23:16; baud = f_HFXO / (8 x NUM / DEN). */
#define EFR32_MODEM_TXBR_OFFSET 0x058U
#define EFR32_MODEM_TXBR_DEN_SHIFT 16U
#define EFR32_MODEM_TXBR_NUM_MAX 0xFFFFU
#define EFR32_MODEM_TXBR_HFXO_DIVIDER 8U
/* MODEM.PRE.TXBASES bits 31:16: preamble repetitions; none, so no keyed preamble. */
#define EFR32_MODEM_PRE_OFFSET 0x064U
#define EFR32_MODEM_PRE_TXBASES_MASK (0xFFFFU << 16)
/* MODEM.MODINDEX: MODINDEXM bits 4:0, MODINDEXE bits 9:5 (two's complement). */
#define EFR32_MODEM_MODINDEX_OFFSET 0x088U
#define EFR32_MODEM_MODINDEX_ME_MASK 0x3FFU
#define EFR32_MODEM_MODINDEXE_SHIFT 5U
#define EFR32_MODEM_MODINDEXE_FIELD_MASK 0x1FU
/* FRC.DFLCTRL.DFLMODE bits 2:0: 0 disables dynamic length (WCNTCMP0 fixes it). */
#define EFR32_FRC_DFLCTRL_OFFSET 0x00CU
#define EFR32_FRC_DFLCTRL_DFLMODE_MASK 0x7U
#define EFR32_FRC_WCNTCMP0_FRAMELENGTH_MASK 0xFFFU
/* FRC.FCDn.INCLUDECRC (10) and CALCCRC (11): no CRC is computed or sent. */
#define EFR32_FRC_FCD_INCLUDECRC BIT(10)
#define EFR32_FRC_FCD_CALCCRC BIT(11)

/* OOK symbol rate: 200 Bd, 12 symbols per 60 ms unit at 20 WPM. */
#define EFR32_CW_SYMBOL_RATE_HZ 200U
#define EFR32_CW_TXBRNUM (EFR32_SYNTH_REFERENCE_HZ / \
			  (EFR32_MODEM_TXBR_HFXO_DIVIDER * EFR32_CW_SYMBOL_RATE_HZ))
#define EFR32_CW_TXBRDEN 1U
BUILD_ASSERT(EFR32_SYNTH_REFERENCE_HZ % (EFR32_MODEM_TXBR_HFXO_DIVIDER * EFR32_CW_SYMBOL_RATE_HZ) ==
	     0U, "the CW symbol rate must be exact");
BUILD_ASSERT(EFR32_CW_TXBRNUM <= EFR32_MODEM_TXBR_NUM_MAX, "TXBRNUM is 16 bits");

/*
 * The OOK amplitude scale in MODEM.MODINDEX, as the configurator sets it for OOK/BPSK
 * (calc_modulator.py calc_modindex_value): 150 x 16 / (shaping gain x interpolation gain).
 * Shaping gain with CTRL0.SHAPING = 3 (the 1M table's) is max(COEFF0..7) = 0x53 = 83
 * (calc_shaping.py, the 1M delta table's SHAPING0/1); the interpolation gain is TXBRNUM / 128
 * for TXBRNUM >= 16384 (calc_demodulator.py) = 187.5. 2400 / (83 x 187.5) = 0.1542, whose
 * nearest MODINDEXM x 2^MODINDEXE with M <= 31 (calc_utilities.py frac2exp) is 20 x 2^-7.
 * tests/efr32-radio-contract replays the computation. [I] unmeasured on air.
 */
#define EFR32_CW_MODINDEXM 20U
#define EFR32_CW_MODINDEXE (-7)

uint32_t efr32_radio_cw_symbols_per_unit(uint32_t unit_us)
{
	return (uint32_t)(((uint64_t)EFR32_CW_SYMBOL_RATE_HZ * unit_us) / USEC_PER_SEC);
}

uint32_t efr32_radio_cw_symbol_rate_hz(void)
{
	return EFR32_CW_SYMBOL_RATE_HZ;
}

uint32_t efr32_radio_cw_max_octets(void)
{
	return MIN(sizeof(efr32_tx_fifo), EFR32_FRC_WCNTCMP0_FRAMELENGTH_MASK + 1U);
}

/* Read-modify-write through the SET/CLR aliases, as the rest of this file does. */
static void efr32_reg_field(uintptr_t reg, uint32_t mask, uint32_t value)
{
	*radio_reg32(reg + EFR32_REGISTER_CLEAR_OFFSET) = mask & ~value;
	*radio_reg32(reg + EFR32_REGISTER_SET_OFFSET) = value & mask;
}

void efr32_radio_cw_transmit(uint32_t khz, const uint8_t *bits, size_t octets,
			     radio_isr_cb_t done, void *param)
{
	/* The sync word is sent before the frame: all zero, the key stays up. */
	static const uint8_t key_up_sync[sizeof(uint32_t)];
	uint32_t channel_number = 0U;

	LL_ASSERT(octets > 0U && octets <= efr32_radio_cw_max_octets());
	radio_reset();
	radio_phy_set(PHY_1M, 0U);
	radio_tx_power_max_set();
	*radio_reg32(EFR32_SYNTH_BASE + EFR32_SYNTH_FREQ_OFFSET) = efr32_synth_freq_word(khz);
	*radio_reg32(EFR32_FRC_BASE + EFR32_FRC_WCNTCMP2_OFFSET) = channel_number;
	*radio_reg32(EFR32_SYNTH_BASE + EFR32_SYNTH_CHCTRL_OFFSET) = 0U;
	*radio_reg32(EFR32_SYNTH_BASE + EFR32_SYNTH_CHSP_OFFSET) = channel_number;
	radio_aa_set(key_up_sync);
	radio_pkt_configure(8U, EFR32_PKT_PAYLOAD_MAX, 0U);

	efr32_reg_field(EFR32_MODEM_BASE + EFR32_MODEM_CTRL0_OFFSET,
			EFR32_MODEM_CTRL0_MODFORMAT_MASK, EFR32_MODEM_CTRL0_MODFORMAT_OOKASK);
	*radio_reg32(EFR32_MODEM_BASE + EFR32_MODEM_TXBR_OFFSET) =
		(EFR32_CW_TXBRDEN << EFR32_MODEM_TXBR_DEN_SHIFT) | EFR32_CW_TXBRNUM;
	efr32_reg_field(EFR32_MODEM_BASE + EFR32_MODEM_PRE_OFFSET, EFR32_MODEM_PRE_TXBASES_MASK, 0U);
	efr32_reg_field(EFR32_MODEM_BASE + EFR32_MODEM_MODINDEX_OFFSET, EFR32_MODEM_MODINDEX_ME_MASK,
			EFR32_CW_MODINDEXM |
			(((uint32_t)EFR32_CW_MODINDEXE & EFR32_MODEM_MODINDEXE_FIELD_MASK)
			 << EFR32_MODEM_MODINDEXE_SHIFT));
	efr32_reg_field(EFR32_FRC_BASE + EFR32_FRC_DFLCTRL_OFFSET, EFR32_FRC_DFLCTRL_DFLMODE_MASK, 0U);
	*radio_reg32(EFR32_FRC_BASE + EFR32_FRC_WCNTCMP0_OFFSET) = (uint32_t)octets - 1U;
	for (uint32_t d = 0U; d < EFR32_FRC_FCD_COUNT; d++) {
		efr32_reg_field(EFR32_FRC_BASE + EFR32_FRC_FCD0_OFFSET + d * EFR32_FRC_FCD_STRIDE,
				EFR32_FRC_FCD_INCLUDECRC | EFR32_FRC_FCD_CALCCRC | EFR32_FRC_FCD_SKIPWHITE,
				EFR32_FRC_FCD_SKIPWHITE);
	}

	memcpy(efr32_tx_fifo, bits, octets);
	efr32_packet_stage.tx_source = bits;
	efr32_packet_stage.tx_bytes = (uint32_t)octets;
	efr32_packet_stage.tx_fifo_configured = true;
	efr32_cw_frame = true;
	radio_isr_set(done, param);
	radio_switch_complete_and_disable();
	radio_tx_enable();
}

void efr32_radio_cw_finish(void)
{
	efr32_cw_frame = false;
	radio_status_reset();
	/* The 1M tables restore MODEM and FRC; the next event re-tunes and re-frames. */
	radio_phy_set(PHY_1M, 0U);
}

#if defined(CONFIG_EFR32_RADIO_AMATEUR_ID_DATA)
/*
 * The data identification's advertising PDUs, outside any LLL event, in the identification's
 * reserved slot: the setup of OpenISA lll_adv.c (prepare_cb() for the first PDU, chan_prepare()
 * and isr_done() for the next), with the advertising access address and CRC (Core v6.1 Vol 6
 * Part B 2.1.2, 3.1.1) and the channel's carrier and whitening (lll_chan_set(),
 * radio_whiten_iv_set()).
 */
/* The advertising PDU's Length field: 8 bits (Core v6.1 Vol 6 Part B 2.3). */
#define EFR32_ID_ADV_LENGTH_BITS 8U

void efr32_radio_id_adv_transmit(uint8_t chan, const uint8_t *pdu, bool first,
				 radio_isr_cb_t done, void *param)
{
	LL_ASSERT(chan >= EFR32_AMATEUR_ADV_CHANNEL_FIRST && chan < EFR32_AMATEUR_CHANNELS);
	if (first) {
		uint32_t aa = sys_cpu_to_le32(PDU_AC_ACCESS_ADDR);

		radio_reset();
		radio_tx_power_max_set();
		radio_phy_set(PHY_LEGACY, PHY_FLAGS_UNUSED);
		/* An 8-bit length field, legacy PHY (OpenISA lll_adv.c prepare_cb()). */
		radio_pkt_configure(EFR32_ID_ADV_LENGTH_BITS, PDU_AC_LEG_PAYLOAD_SIZE_MAX, 0U);
		radio_aa_set((uint8_t *)&aa);
		radio_crc_configure(PDU_CRC_POLYNOMIAL, PDU_AC_CRC_IV);
	} else {
		radio_status_reset();
		radio_tmr_status_reset();
	}
	/* radio_freq_chan_set() takes MHz above 2400; it tunes to the plan's carrier. */
	radio_freq_chan_set(efr32_amateur_ble_channel_khz(chan) / EFR32_KHZ_PER_MHZ -
			    EFR32_BLE_CHANNEL_BASE_MHZ);
	radio_whiten_iv_set(chan);
	efr32_id_adv_frame = true;
	/* An advertising event: its next PDU is due within the PDU spacing of this one. */
	efr32_radio_event_tx = true;
	radio_pkt_tx_set((void *)pdu);
	radio_isr_set(done, param);
	radio_switch_complete_and_disable();
	radio_tx_enable();
}

void efr32_radio_id_adv_finish(void)
{
	efr32_id_adv_frame = false;
	radio_status_reset();
	radio_tmr_status_reset();
}

uint32_t efr32_radio_id_tx_ready_us(void)
{
	return radio_tx_ready_delay_get(PHY_1M, 0U);
}
#endif
#endif /* CONFIG_EFR32_RADIO_AMATEUR */

#if defined(CONFIG_SCHED_DEADLINE_MISS_CUSTOM)
/*
 * Safe shutdown on a missed or infeasible deadline (include/zephyr/kernel/deadline_miss.h).
 *
 * Radio state first, from whatever context detected the failure: the PA is switched off at its
 * enables, the receive source and both directions are withdrawn, the RAC is forced off and the
 * M0+ reset, so no sequencer handler can restart the PA or a frame. CTRL reads back without
 * SEQRESET on silicon and in QEMU; with the RAC forced off the restarted M0+ gets no state
 * event, and on silicon its TX counters stop. Then the controller stops: its clock events are
 * cancelled, every wake path is closed, its interrupts are masked and its threads are
 * suspended, the current one last. The rest of the system runs on; the Bluetooth host sees a
 * controller that no longer answers.
 */
#define EFR32_RAC_TX_OFFSET 0x148U
#define EFR32_RAC_PAENCTRL_PARAMP BIT(8)
/*
 * RAC.TX power-amplifier enables cleared by the M0+'s efr32_seq_pa_stop (radio_seq.S):
 * TX0DBMENPREDRV, TX0DBMENBIAS, TX6DBMENRAMPCLK, TX6DBMENPACORE, TX6DBMENPAOUT, ENPAPOWER,
 * ENPASELSLICE.
 */
#define EFR32_RAC_TX_PA_ENABLES \
	(BIT(2) | BIT(5) | BIT(22) | BIT(24) | BIT(25) | BIT(30) | BIT(31))

static void efr32_radio_pa_off(void)
{
	*radio_reg32(EFR32_RAC_BASE + EFR32_REGISTER_CLEAR_OFFSET +
		     EFR32_RAC_PAENCTRL_OFFSET) = EFR32_RAC_PAENCTRL_PARAMP;
	*radio_reg32(EFR32_RAC_BASE + EFR32_REGISTER_CLEAR_OFFSET +
		     EFR32_RAC_TX_OFFSET) = EFR32_RAC_TX_PA_ENABLES;
}

static void efr32_radio_force_safe(void)
{
	efr32_radio_pa_off();
	/* As radio_disable(): the software RX source first, then RXDIS before TXDIS. */
	*radio_reg32(EFR32_RAC_BASE + EFR32_REGISTER_CLEAR_OFFSET +
		     EFR32_RAC_RXENSRCEN_OFFSET) = EFR32_RAC_RXENSRCEN_SWRXEN;
	*radio_reg32(EFR32_RAC_BASE + EFR32_RAC_CMD_OFFSET) = EFR32_RAC_CMD_RXDIS;
	*radio_reg32(EFR32_RAC_BASE + EFR32_RAC_CMD_OFFSET) = EFR32_RAC_CMD_TXDIS;
	/* Base-register write, as radio_reset() resets the M0+ before copying its image. */
	*radio_reg32(EFR32_RAC_BASE + EFR32_RAC_CTRL_OFFSET) =
		EFR32_RAC_CTRL_ACTIVE_HIGH | EFR32_RAC_CTRL_PA_ENABLE_HIGH |
		EFR32_RAC_CTRL_LNA_ENABLE_HIGH | EFR32_RAC_CTRL_SEQRESET |
		EFR32_RAC_CTRL_FORCEDISABLE;
	__DSB();
	/* A sequencer handler may have restarted the PA before its reset took hold. */
	efr32_radio_pa_off();
}

/*
 * The report is printed from the system work queue: the failure may be detected on a radio job's
 * small stack or in an interrupt, and the rest of the system keeps running.
 */
static struct k_deadline_miss efr32_radio_miss;
static struct k_deadline_miss_run efr32_radio_miss_ran[CONFIG_SCHED_DEADLINE_MISS_HISTORY];

static void efr32_radio_miss_report(struct k_work *work)
{
	ARG_UNUSED(work);
	printk("efr32-radio: SAFE SHUTDOWN: PA off, RAC forced off, M0+ reset, "
	       "controller stopped\n");
	k_deadline_miss_print(&efr32_radio_miss);
	printk("  RAC.STATUS 0x%08x CTRL 0x%08x TX 0x%08x PAENCTRL 0x%08x\n",
	       *radio_reg32(EFR32_RAC_BASE + EFR32_RAC_STATUS_OFFSET),
	       *radio_reg32(EFR32_RAC_BASE + EFR32_RAC_CTRL_OFFSET),
	       *radio_reg32(EFR32_RAC_BASE + EFR32_RAC_TX_OFFSET),
	       *radio_reg32(EFR32_RAC_BASE + EFR32_RAC_PAENCTRL_OFFSET));
}

static K_WORK_DEFINE(efr32_radio_miss_work, efr32_radio_miss_report);

#if defined(CONFIG_EFR32_RADIO_TEST_QEMU_COUNT_LL_MISSES)
static bool efr32_radio_policy_stopping;
static atomic_t efr32_radio_test_ll_misses;

uint32_t efr32_radio_test_ll_miss_count(void)
{
	return (uint32_t)atomic_get(&efr32_radio_test_ll_misses);
}
#endif

void k_deadline_miss_handler(const struct k_deadline_miss *miss)
{
	const k_tid_t jobs[] = {
		efr32_radio_tx_job_thread,
		efr32_radio_rx_job_thread,
		efr32_radio_completion_job_thread,
#if defined(CONFIG_EFR32_RADIO_QEMU_TIFS)
		efr32_qemu_tifs_job_thread,
#endif
#if defined(CONFIG_EFR32_RADIO_QEMU_RX_DESCRIPTOR)
		efr32_qemu_rx_descriptor_thread,
#endif
	};
	bool suspend_self;
	bool first;
	unsigned int key;

#if defined(CONFIG_EFR32_RADIO_TEST_QEMU_COUNT_LL_MISSES)
	/*
	 * Test only, QEMU: a link-layer deadline the model cannot meet (connection turnarounds,
	 * docs/radio-tables.md 7.79) is counted, as the QEMU connection profiles' MISS_COUNT does;
	 * a broken rule (efr32_radio_policy_stop()) still shuts the radio down.
	 */
	if (!efr32_radio_policy_stopping) {
		atomic_inc(&efr32_radio_test_ll_misses);
		return;
	}
#endif
	key = irq_lock();
	first = atomic_cas(&efr32_radio_halted, 0, 1);
	/* A failure may be detected before radio_setup(): its RAC accesses need the clocks. */
	efr32_radio_bus_clocks_on();
	efr32_radio_force_safe();
	irq_disable(LL_RADIO_IRQn);
#if defined(CONFIG_EFR32_RADIO_QEMU_RX_DESCRIPTOR)
	irq_disable(BUFC_IRQn);
#endif
	irq_unlock(key);
	if (!first) {
		return;
	}

	cntr_shutdown();
	atomic_set(&efr32_radio_launch_active, 0);
	k_deadline_event_reset(&efr32_radio_tx_event);
	k_deadline_event_reset(&efr32_radio_rx_event);
	k_deadline_event_reset(&efr32_radio_completion_event);
	suspend_self = efr32_ll_threads_stop();
	ARRAY_FOR_EACH(jobs, i) {
		if (jobs[i] == k_current_get() && !k_is_in_isr()) {
			suspend_self = true;
		} else {
			k_thread_suspend(jobs[i]);
		}
	}

	efr32_radio_miss = *miss;
	memcpy(efr32_radio_miss_ran, miss->ran, miss->ran_count * sizeof(miss->ran[0]));
	efr32_radio_miss.ran = efr32_radio_miss_ran;
	(void)k_work_submit(&efr32_radio_miss_work);
	if (suspend_self) {
		k_thread_suspend(k_current_get());
	}
}

/*
 * A rule the radio must keep cannot be kept (for example an amateur station identification
 * not sent by its deadline): the same safe shutdown, reported as a late start of @p event.
 */
void efr32_radio_policy_stop(const struct k_deadline_event *event, const char *why)
{
	static const struct k_deadline_miss_run no_runs[1];
	struct k_deadline_miss miss = {
		.kind = K_DEADLINE_MISS_START,
		.object = event,
		.name = why,
		.now = k_cycle_get_32(),
		.ran = no_runs,
	};

	miss.release = miss.now;
	miss.deadline = (int)miss.now;
#if defined(CONFIG_EFR32_RADIO_TEST_QEMU_COUNT_LL_MISSES)
	efr32_radio_policy_stopping = true;
#endif
	k_deadline_miss_handler(&miss);
}
#endif /* CONFIG_SCHED_DEADLINE_MISS_CUSTOM */

struct k_deadline_event *efr32_radio_launch_event(uint8_t trx)
{
	return trx ? &efr32_radio_tx_event : &efr32_radio_rx_event;
}

struct k_deadline_event *efr32_radio_tifs_event(void)
{
#if defined(CONFIG_EFR32_RADIO_QEMU_TIFS)
	return &efr32_qemu_tifs_event;
#else
	return NULL;
#endif
}

#if defined(CONFIG_EFR32_RADIO_ADMISSION)
/* Execution budgets of the radio's jobs (Kconfig EFR32_RADIO_WCET_*, measured or named). */
static int efr32_radio_budgets_init(void)
{
	k_deadline_event_budget_set(&efr32_radio_tx_event,
				    k_ns_to_cyc_ceil32(CONFIG_EFR32_RADIO_WCET_TX_LAUNCH_NS));
	/* A Peripheral's held receive-window launch runs longer than a scan window's. */
	k_deadline_event_budget_set(&efr32_radio_rx_event,
				    k_ns_to_cyc_ceil32(MAX(CONFIG_EFR32_RADIO_WCET_RX_LAUNCH_NS,
				    COND_CODE_1(CONFIG_BT_PERIPHERAL,
						(CONFIG_EFR32_RADIO_WCET_RX_WINDOW_LAUNCH_NS), (0)))));
	/* The longest kind registers the event; each publication carries its own kind's budget. */
	k_deadline_event_budget_set(&efr32_radio_completion_event,
				    EFR32_DL_MEAS_CYC(EFR32_RADIO_WCET_COMPLETION_MAX_NS));
#if defined(CONFIG_EFR32_RADIO_QEMU_TIFS)
	k_deadline_event_budget_set(&efr32_qemu_tifs_event,
				    k_ns_to_cyc_ceil32(CONFIG_EFR32_RADIO_WCET_QEMU_TIFS_NS));
#endif
	/*
	 * The QEMU RX drain shares the ULL worker's priority and, like it, is not admitted: its
	 * measured run is wall time including the radio level's preemption (ll_threads.c).
	 */
	return 0;
}

SYS_INIT(efr32_radio_budgets_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
#endif
