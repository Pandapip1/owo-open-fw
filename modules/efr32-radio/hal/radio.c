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
#include <psa/crypto.h>

#include "hal/ccm.h"
#include "hal/cntr.h"
#include "hal/efr32/cntr_efr32.h"
#include "hal/efr32/ll_threads.h"
#include "hal/efr32/ll_deadlines.h"
#include "hal/efr32/seq_abi.h"
#include "hal/efr32/ticker.h"
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
/* Instant the pending TX launch holds TXEN to. */
static atomic_t efr32_radio_tx_instant;
/*
 * Whether the current radio event began with a scheduled TX (an advertising event) rather than a
 * receive window, and the TXEN of its latest packet started by radio_tx_enable(): the work that
 * may start the next ADV_IND is due BT_ADV_PDU_SPACING_MAX_US after it.
 */
static bool efr32_radio_event_tx;
static uint32_t efr32_radio_last_txen;
static int efr32_radio_next_start_by(uint32_t now, uint32_t path_ns);

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

static inline void efr32_radio_hold_until(uint32_t instant)
{
	uint32_t count;

	if (k_clock_event_sched_active_target(instant, &count) != 0) {
		while ((int32_t)(k_cycle_get_32() - instant) < 0) {
		}
	} else if (efr32_radio_hold_direct) {
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
		    CONFIG_EFR32_RADIO_WCET_COMPLETION_RX_TX_NS), \
		MAX(CONFIG_EFR32_RADIO_WCET_COMPLETION_RX_NS, \
		    CONFIG_EFR32_RADIO_WCET_COMPLETION_TX_NS)), \
	    CONFIG_EFR32_RADIO_WCET_COMPLETION_ABORT_NS)
K_DEADLINE_EVENT_DEFINE(efr32_radio_tx_event);
K_DEADLINE_EVENT_DEFINE(efr32_radio_rx_event);
K_DEADLINE_EVENT_DEFINE(efr32_radio_completion_event);

/* The FRC vector publishes this mailbox; only the persistent job calls LLL. */
static struct {
	radio_isr_cb_t callback;
	void *param;
	bool abort;
} efr32_radio_completion_work;
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

static void efr32_radio_tx_start(bool hold, uint32_t instant);

static void efr32_radio_tx_job(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	for (;;) {
		atomic_val_t generation;

		k_deadline_event_wait(&efr32_radio_tx_event);
		generation = atomic_get(&efr32_radio_tx_pending_generation);
		if (atomic_cas(&efr32_radio_launch_active,
			    generation, 0)) {
			/* TXEN is held to the event's instant: no interrupt may come between. */
			unsigned int key = irq_lock();

			efr32_radio_tx_start(true, (uint32_t)atomic_get(&efr32_radio_tx_instant));
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
		generation = atomic_get(&efr32_radio_rx_pending_generation);
		if (atomic_cas(&efr32_radio_launch_active,
			    generation, 0)) {
			radio_rx_enable();
		}
	}
}

K_THREAD_DEFINE(efr32_radio_tx_job_thread,
		CONFIG_EFR32_RADIO_LAUNCH_STACK_SIZE,
		efr32_radio_tx_job, NULL, NULL, NULL,
		EFR32_LLL_THREAD_PRIO, 0, 0);
K_THREAD_DEFINE(efr32_radio_rx_job_thread,
		CONFIG_EFR32_RADIO_LAUNCH_STACK_SIZE,
		efr32_radio_rx_job, NULL, NULL, NULL,
		EFR32_LLL_THREAD_PRIO, 0, 0);
K_THREAD_DEFINE(efr32_radio_completion_job_thread,
		CONFIG_EFR32_RADIO_COMPLETION_STACK_SIZE,
		efr32_radio_completion_job, NULL, NULL, NULL,
		EFR32_LLL_THREAD_PRIO, 0, 0);

/* ISR-safe: publish only a generation and wake the pre-created job. */
void efr32_radio_launch_wake(uint8_t trx, uint32_t generation, uint32_t instant,
			     uint32_t not_before, int deadline)
{
	if (atomic_get(&efr32_radio_halted) != 0) {
		return;
	}
	if ((uint32_t)atomic_get(&efr32_radio_launch_active) != generation) {
		return;
	}
	if (trx) {
		atomic_set(&efr32_radio_tx_pending_generation, generation);
		atomic_set(&efr32_radio_tx_instant, (atomic_val_t)instant);
		/* Released early by the wake latency: wait out not_before, then hold TXEN. */
		k_deadline_event_post_at(&efr32_radio_tx_event, not_before, deadline);
	} else {
		ARG_UNUSED(instant);
		ARG_UNUSED(not_before);
		atomic_set(&efr32_radio_rx_pending_generation, generation);
		k_deadline_event_post(&efr32_radio_rx_event, deadline);
	}
}

/* Teardown never owns the persistent job threads. It only makes any queued
 * mailbox wake stale and releases the shared BURTC deadline. */
static void efr32_radio_launch_disarm(void)
{
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
static uint8_t MALIGN(4) efr32_tx_fifo[EFR32_BUFC_FIFO_BYTES(EFR32_PKT_SIZE_MAX)];
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
 * Hardware-watchpoint captures at the working stage-2 channel-preparation
 * boundary.  Keep these named by their Bluetooth physical use instead of
 * scattering captured words through the launch path.  The general xG22
 * frequency-word encoding still needs a source-backed derivation before data
 * channels are enabled by this open port.
 */
#define EFR32_SYNTH_BLE_ADV37_FREQ 0x01F46AABU /* 2402 MHz */
#define EFR32_SYNTH_BLE_ADV38_FREQ 0x01F96AABU /* 2426 MHz */
#define EFR32_SYNTH_BLE_ADV39_FREQ 0x0204AAABU /* 2480 MHz */

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
static void efr32_packet_stage_arm(enum efr32_radio_packet_direction direction)
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
		LL_ASSERT(efr32_packet_stage.tx_bytes <=
			  (uint32_t)efr32_packet_stage.max_len + 2U);
	}

	/* The LLL reads completion state from its ISR callback, so clear only
	 * before the operation is armed—not from the ISR itself. */
	*radio_reg32(EFR32_FRC_BASE + EFR32_REGISTER_CLEAR_OFFSET +
		     EFR32_FRC_IF_OFFSET) = EFR32_FRC_IF_COMPLETION;
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
	bool empty;
} ccm_ctx;

static void ccm_nonce_build(const struct ccm *ccm, uint8_t nonce[BLE_CCM_NONCE_SIZE])
{
	uint64_t counter = ccm->counter;

	memcpy(nonce, &counter, 5);
	nonce[4] |= ccm->direction << 7;
	memcpy(&nonce[5], ccm->iv, sizeof(ccm->iv));
}

static psa_key_id_t ccm_key_import(const struct ccm *ccm, psa_key_usage_t usage)
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_key_id_t id = 0;
	psa_algorithm_t alg = PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, BLE_CCM_MIC_SIZE);

	if (psa_crypto_init() != PSA_SUCCESS) {
		return 0;
	}
	psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
	psa_set_key_bits(&attr, 128);
	psa_set_key_usage_flags(&attr, usage);
	psa_set_key_algorithm(&attr, alg);
	if (psa_import_key(&attr, ccm->key, sizeof(ccm->key), &id) != PSA_SUCCESS) {
		return 0;
	}

	return id;
}

/*
 * The FRC line is level-sensitive while IF.RXDONE/TXDONE remains set.  Mask
 * only those two sources after publishing their callback so IRQ exit does not
 * starve the scheduler job.  The job leaves the status visible to the OpenISA
 * callback, then re-enables the sources after that callback has had its
 * normal radio_status_reset() opportunity.
 */
static void efr32_radio_completion_window(bool abort, uint32_t now, int *deadline,
					  uint32_t *budget);

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
		unsigned int key;

		k_deadline_event_wait(&efr32_radio_completion_event);
		key = irq_lock();
		if (efr32_radio_completion_dispatch_take(&efr32_radio_completion_dispatch)) {
			callback = efr32_radio_completion_work.callback;
			param = efr32_radio_completion_work.param;
			abort = efr32_radio_completion_work.abort;
		}
		irq_unlock(key);

		if (!callback) {
			continue;
		}

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
#define EFR32_SEQ_RX2TX_DECISION_ADDRESS 0xB0004FA4UL
#define EFR32_SEQ_RX2TX_PENDING 3U
#define EFR32_SEQ_RX2TX_RESPOND 1U
#define EFR32_SEQ_RX2TX_CANCEL 2U
static bool efr32_hw_rx2tx_armed;

#define EFR32_RAC_STIMER_OFFSET 0x030U
#define EFR32_RAC_STIMER_MASK 0xFFFFU
#define EFR32_RAC_STATE_RX2TX 7U
/* RAC.STIMER ticks in @p ns, rounded up / down. */
#define EFR32_STIMER_TICKS_CEIL(ns) \
	DIV_ROUND_UP((uint32_t)(ns) * EFR32_SEQ_RAC_STIMER_TICKS_PER_10US, 10000U)
#define EFR32_STIMER_TICKS_FLOOR(ns) \
	((uint32_t)(ns) * EFR32_SEQ_RAC_STIMER_TICKS_PER_10US / 10000U)
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
 * its start to that packet's TXEN or RXEN at the latest (hal/efr32/ll_deadlines.h).
 */
static int efr32_radio_next_start_by(uint32_t now, uint32_t path_ns)
{
	return efr32_radio_event_tx ? efr32_dl_adv_next_start_by(efr32_radio_last_txen, path_ns) :
				      efr32_dl_scan_next_start_by(now, path_ns);
}

/*
 * Deadline and budget of the completion published at @p now:
 * - TX whose RX follows (ADV_IND of a scannable advertiser): mark the RX->TX window before the
 *   earliest SCAN_REQ is recognised (efr32_dl_completion_tx_rx_start_by()).
 * - RX with a response armed, the RAC holding in RX2TX: write RESPOND by
 *   EFR32_RX2TX_RESPOND_BY_STIMER, read against RAC.STIMER, which counts from the frame's end.
 * - Every other kind (RX without response, last TX, abort) may start the event's next packet.
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
	uint32_t wcet_ns;

	if (abort) {
		wcet_ns = CONFIG_EFR32_RADIO_WCET_COMPLETION_ABORT_NS;
	} else if (direction == EFR32_RADIO_PACKET_TX && rx_follows &&
		   !IS_ENABLED(CONFIG_EFR32_RADIO_QEMU_TIFS)) {
		*budget = EFR32_DL_MEAS_CYC(CONFIG_EFR32_RADIO_WCET_COMPLETION_TX_RX_NS);
		*deadline = efr32_dl_completion_tx_rx_start_by(now);
		return;
	} else if (direction == EFR32_RADIO_PACKET_TX) {
		wcet_ns = CONFIG_EFR32_RADIO_WCET_COMPLETION_TX_NS;
	} else if (efr32_hw_rx2tx_armed) {
		uint32_t state = (*radio_reg32(EFR32_RAC_BASE + EFR32_RAC_STATUS_OFFSET) >>
				  EFR32_RAC_STATUS_STATE_SHIFT) & EFR32_RAC_STATUS_STATE_MASK;
		uint32_t stimer = *radio_reg32(EFR32_RAC_BASE + EFR32_RAC_STIMER_OFFSET) &
				  EFR32_RAC_STIMER_MASK;
		uint32_t state_after = (*radio_reg32(EFR32_RAC_BASE + EFR32_RAC_STATUS_OFFSET) >>
					EFR32_RAC_STATUS_STATE_SHIFT) & EFR32_RAC_STATUS_STATE_MASK;

		*budget = EFR32_DL_MEAS_CYC(CONFIG_EFR32_RADIO_WCET_COMPLETION_RX_TX_NS);
		if (state == EFR32_RAC_STATE_RX2TX && state_after == EFR32_RAC_STATE_RX2TX) {
			int32_t ticks = (int32_t)EFR32_RX2TX_RESPOND_BY_STIMER - (int32_t)stimer;
			/* Rounded towards earlier either way. */
			uint32_t left = (ticks >= 0) ?
				k_ns_to_cyc_floor32((uint32_t)ticks * 10000U /
						    EFR32_SEQ_RAC_STIMER_TICKS_PER_10US) :
				-k_ns_to_cyc_ceil32(DIV_ROUND_UP((uint32_t)-ticks * 10000U,
						    EFR32_SEQ_RAC_STIMER_TICKS_PER_10US));

			*deadline = (int)(now + left -
					  EFR32_DL_MEAS_CYC(CONFIG_EFR32_RADIO_COMPLETION_TO_RESPOND_NS));
			return;
		}
		/* The sequencer has left RX2TX: no response can follow this frame. */
		wcet_ns = CONFIG_EFR32_RADIO_WCET_COMPLETION_RX_TX_NS;
	} else {
		wcet_ns = CONFIG_EFR32_RADIO_WCET_COMPLETION_RX_NS;
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
	if (efr32_hw_rx2tx_armed) {
		efr32_hw_rx2tx_armed = false;
		*radio_reg32(EFR32_SEQ_RX2TX_DECISION_ADDRESS) = EFR32_SEQ_RX2TX_CANCEL;
	}
#if !defined(CONFIG_EFR32_RADIO_QEMU_TIFS)
	atomic_inc(&efr32_hw_hcto_generation);
	cntr_qemu_tifs_cmp_cancel();
#endif
}

/* [regs-public] Enable the eight radio blocks in the observed stock order. */
void radio_setup(void)
{
	efr32_radio_hold_init();
	const uint32_t radio_bus_clocks =
		CMU_CLKEN1_AGC | CMU_CLKEN1_MODEM | CMU_CLKEN1_RFCRC |
		CMU_CLKEN1_FRC | CMU_CLKEN1_PROTIMER | CMU_CLKEN1_RAC |
		CMU_CLKEN1_SYNTH | CMU_CLKEN1_BUFC;

	efr32_packet_stage_reset();
	efr32_rx_delivery_invalid = false;
	efr32_hw_turnaround_reset();
#if defined(CONFIG_EFR32_RADIO_QEMU_TIFS)
	efr32_qemu_tifs_state_init(&efr32_qemu_tifs);
	efr32_qemu_tifs_next_action = EFR32_QEMU_TIFS_NEXT_NONE;
	efr32_qemu_tifs_transition_requested = false;
	efr32_qemu_tifs_rx_to_tx_staging = false;
#endif
	/*
	 * The vendor RAIL path enables both the HCLKRADIO branch and each radio
	 * block's bus clock before accessing the block.  The custom LLL bypasses
	 * RAIL, so it owns that prerequisite.  QEMU used to expose every register
	 * even with these reset-disabled, masking the omission on real silicon.
	 */
	CMU->RADIOCLKCTRL_SET = CMU_RADIOCLKCTRL_EN;
	CMU->CLKEN1_SET = radio_bus_clocks;
	__DSB();

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

/* [regs-public] Public BLE tables plus a relocated, image-local PHYINFO object. */
void radio_phy_set(uint8_t phy, uint8_t flags)
{
	EFR32_RADIO_TRACE_ONCE("phy phy=%u flags=%u", phy, flags);
	ARG_UNUSED(flags);

	switch (phy) {
	case PHY_LEGACY:
	case PHY_1M:
		efr32_radio_table_apply(efr32_ble_1m_base_table);
		efr32_radio_table_apply(efr32_ble_1m_delta_table);
		efr32_ble_phy_info_publish(efr32_ble_1m_phy_info);
		break;
	case PHY_2M:
		efr32_radio_table_apply(efr32_ble_1m_base_table);
		efr32_radio_table_apply(efr32_ble_2m_delta_table);
		break;
	case PHY_CODED:
		efr32_radio_table_apply(efr32_ble_1m_base_table);
		if (flags == PHY_FLAGS_S8) {
			efr32_radio_table_apply(efr32_ble_coded_s8_delta_table);
		} else {
			LL_ASSERT(flags == PHY_FLAGS_S2);
			efr32_radio_table_apply(efr32_ble_coded_s2_delta_table);
		}
		break;
	default:
		EFR32_RADIO_TODO("regs-public: unknown PHY");
		break;
	}
}

/* [hardware] Fixed working-board PA characterization; dBm mapping remains T7. */
void radio_tx_power_set(uint32_t power)
{
	ARG_UNUSED(power);
	*radio_reg32(EFR32_RAC_BASE + EFR32_RAC_PAENCTRL_OFFSET) =
		EFR32_RAC_STAGE2_PAENCTRL;
	*radio_reg32(EFR32_RAC_BASE + EFR32_RAC_PATRIM1_OFFSET) =
		EFR32_RAC_STAGE2_PATRIM1;
	*radio_reg32(EFR32_RAC_BASE + EFR32_RAC_PATRIM3_OFFSET) =
		EFR32_RAC_STAGE2_PATRIM3;
	*radio_reg32(EFR32_RAC_BASE + EFR32_RAC_PACTRL_OFFSET) =
		EFR32_RAC_STAGE2_PACTRL;
}

/* [hardware] The only characterized preset is also the temporary maximum. */
void radio_tx_power_max_set(void)
{
	radio_tx_power_set(0U);
}

/* [hardware] Tune the three evidenced BLE advertising frequencies (T4). */
void radio_freq_chan_set(uint32_t chan)
{
	uint32_t channel_number;
	uint32_t frequency_word;

	/* Zephyr passes MHz relative to 2400: 2..80, always even. */
	LL_ASSERT((chan >= 2U) && (chan <= 80U) && ((chan & 1U) == 0U));
	channel_number = (chan - 2U) / 2U;
	switch (chan) {
	case 2U:
		frequency_word = EFR32_SYNTH_BLE_ADV37_FREQ;
		break;
	case 26U:
		frequency_word = EFR32_SYNTH_BLE_ADV38_FREQ;
		break;
	case 80U:
		frequency_word = EFR32_SYNTH_BLE_ADV39_FREQ;
		break;
	default:
		EFR32_RADIO_AIR_TODO("source-backed SYNTH frequency encoding for data channels");
		return;
	}
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
}

/* [regs-public] RAIL's BLE helper writes FRC.WHITEPOLY=0x44 and WHITEINIT=iv|0x40 (T5). */
void radio_whiten_iv_set(uint32_t iv)
{
	/* LLL supplies a Bluetooth physical channel number, 0 through 39. */
	LL_ASSERT(iv <= 39U);
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
	bytes = (uint32_t)pdu[1] + 2U;
	EFR32_RADIO_TRACE_ONCE("tx-fifo bytes=%u pdu0=0x%02x len=%u",
			       bytes, pdu[0], pdu[1]);
	/* OpenISA PDUs are header byte, payload-length byte, then payload. */
	LL_ASSERT(pdu[1] <= efr32_packet_stage.max_len);
	LL_ASSERT(bytes <= sizeof(efr32_tx_fifo));
	LL_ASSERT(((uintptr_t)efr32_tx_fifo & 3U) == 0U);
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
		__DSB();
		*radio_reg32(EFR32_SEQ_RX2TX_DECISION_ADDRESS) = EFR32_SEQ_RX2TX_RESPOND;
		EFR32_RX2TX_M33_STAMP(2);
	}
}

/* [software] software (no register access needed); not implemented yet */
uint32_t radio_tx_ready_delay_get(uint8_t phy, uint8_t flags)
{
	ARG_UNUSED(phy);
	ARG_UNUSED(flags);
	EFR32_RADIO_TODO("software");
	return 0;
}

/* [software] software (no register access needed); not implemented yet */
uint32_t radio_tx_chain_delay_get(uint8_t phy, uint8_t flags)
{
	ARG_UNUSED(phy);
	ARG_UNUSED(flags);
	EFR32_RADIO_TODO("software");
	return 0;
}

/* [software] software (no register access needed); not implemented yet */
uint32_t radio_rx_ready_delay_get(uint8_t phy, uint8_t flags)
{
	ARG_UNUSED(phy);
	ARG_UNUSED(flags);
	EFR32_RADIO_TODO("software");
	return 0;
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
void radio_rx_enable(void)
{
	while (!radio_is_idle()) {
	}

	/* Validate and label a completion before arming the hardware source. */
	efr32_packet_stage_arm(EFR32_RADIO_PACKET_RX);
	*radio_reg32(EFR32_RAC_BASE + EFR32_REGISTER_SET_OFFSET +
		     EFR32_RAC_RXENSRCEN_OFFSET) = EFR32_RAC_RXENSRCEN_SWRXEN;
}

/*
 * [regs-vendor] The generic RAIL TX helper ends by writing one to RAC.CMD.
 * Its preceding packet/FIFO setup is separate and remains unimplemented;
 * see docs/radio-tables.md §4.1.
 */
void radio_tx_enable(void)
{
	efr32_radio_tx_start(false, 0U);
}

/* TX start: with @p hold, TXEN is held to @p instant (interrupts locked by the caller). */
static void efr32_radio_tx_start(bool hold, uint32_t instant)
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
	if (hold) {
		efr32_radio_hold_until(instant);
	}
	*radio_reg32(EFR32_RAC_BASE + EFR32_RAC_CMD_OFFSET) = EFR32_RAC_CMD_TXEN;
	efr32_radio_last_txen = k_cycle_get_32();
#if !defined(CONFIG_EFR32_RADIO_QEMU_TIFS)
	efr32_hw_rx_after_tx_pending = efr32_hw_rx_after_tx_requested;
	efr32_hw_rx_after_tx_requested = false;
	if (efr32_hw_rx_after_tx_pending) {
		*radio_reg32(EFR32_RAC_BASE + EFR32_REGISTER_SET_OFFSET +
			     EFR32_RAC_RXENSRCEN_OFFSET) = EFR32_RAC_RXENSRCEN_SWRXEN;
	}
#endif
	EFR32_RADIO_TRACE_ONCE("txen status-before=0x%08x status-after=0x%08x",
			       status_before,
			       *radio_reg32(EFR32_RAC_BASE + EFR32_RAC_STATUS_OFFSET));
}

/* [regs-public] RAC.CMD TXDIS/RXDIS are write-only command bits (T2). */
void radio_disable(void)
{
	unsigned int key;

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

	*radio_reg32(EFR32_FRC_BASE + EFR32_REGISTER_CLEAR_OFFSET +
		     EFR32_FRC_IF_OFFSET) = EFR32_FRC_IF_COMPLETION;
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
	EFR32_RADIO_TRACE_ONCE("schedule trx=%u tick=%u remainder=%u generation=%u",
			       trx, tick, supplied_remainder, generation);
	cntr_radio_cmp_set(trx, tick, generation);
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
		efr32_packet_stage_arm(EFR32_RADIO_PACKET_RX);
		if (efr32_hw_tx_after_rx_requested) {
			*radio_reg32(EFR32_SEQ_RX2TX_DECISION_ADDRESS) =
				EFR32_SEQ_RX2TX_PENDING;
			efr32_hw_rx2tx_armed = true;
		}
		/* As in the QEMU path, HCTO bounds the window from this point. */
		cntr_rx_timeout_arm_after_us(hcto,
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

/* [regs] PROTIMER (timebase, compare, captures) and RTC sync: unknown (T8) */
uint32_t radio_tmr_aa_get(void)
{
	EFR32_RADIO_TODO("regs");
	return 0;
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

/* [regs] PROTIMER (timebase, compare, captures) and RTC sync: unknown (T8) */
uint32_t radio_tmr_ready_get(void)
{
	EFR32_RADIO_TODO("regs");
	return 0;
}

/* [regs] PROTIMER (timebase, compare, captures) and RTC sync: unknown (T8) */
void radio_tmr_end_capture(void)
{
	efr32_radio_timer_state_end_capture_request(&efr32_radio_timer);
}

/* [regs] PROTIMER (timebase, compare, captures) and RTC sync: unknown (T8) */
uint32_t radio_tmr_end_get(void)
{
	EFR32_RADIO_TODO("regs");
	return 0;
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

/* [software] Synchronous BLE AES-CCM through PSA. */
void *radio_ccm_rx_pkt_set(struct ccm *ccm, uint8_t phy, void *pkt)
{
	ARG_UNUSED(phy);
	memcpy(&ccm_ctx.config, ccm, sizeof(*ccm));
	ccm_ctx.rx_in = (struct pdu_data *)pkt_scratch;
	ccm_ctx.rx_out = pkt;
	ccm_ctx.pending_rx = true;
	ccm_ctx.done = false;
	ccm_ctx.mic_valid = false;
	ccm_ctx.empty = false;

	return pkt_scratch;
}

/* [software] Encrypts synchronously; returned storage remains valid until the next radio packet. */
void *radio_ccm_tx_pkt_set(struct ccm *ccm, void *pkt)
{
	struct pdu_data *in = pkt;
	uint8_t nonce[BLE_CCM_NONCE_SIZE];
	uint8_t aad = *(uint8_t *)pkt & BLE_CCM_AAD_MASK;
	psa_algorithm_t alg = PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, BLE_CCM_MIC_SIZE);
	psa_key_id_t id;
	size_t out_len;
	psa_status_t status;

	ccm_ctx.pending_rx = false;
	ccm_ctx.done = false;
	ccm_ctx.mic_valid = false;
	ccm_ctx.empty = false;
	if (in->len == 0U) {
		ccm_ctx.done = true;
		return pkt;
	}

	id = ccm_key_import(ccm, PSA_KEY_USAGE_ENCRYPT);
	if (id == 0) {
		return NULL;
	}
	ccm_nonce_build(ccm, nonce);
	status = psa_aead_encrypt(id, alg, nonce, sizeof(nonce), &aad, sizeof(aad),
				  (uint8_t *)pkt + 2, in->len, pkt_scratch + 2,
				  sizeof(pkt_scratch) - 2, &out_len);
	(void)psa_destroy_key(id);
	if (status != PSA_SUCCESS || out_len != in->len + BLE_CCM_MIC_SIZE) {
		return NULL;
	}

	pkt_scratch[0] = *(uint8_t *)pkt;
	pkt_scratch[1] = in->len + BLE_CCM_MIC_SIZE;
	ccm_ctx.done = true;
	return pkt_scratch;
}

/* [software] Completes a pending receive decrypt synchronously. */
uint32_t radio_ccm_is_done(void)
{
	uint8_t nonce[BLE_CCM_NONCE_SIZE];
	uint8_t aad;
	psa_algorithm_t alg = PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, BLE_CCM_MIC_SIZE);
	psa_key_id_t id;
	size_t out_len;
	psa_status_t status;

	if (!ccm_ctx.pending_rx) {
		return ccm_ctx.done;
	}
	ccm_ctx.pending_rx = false;
	if (ccm_ctx.rx_in->len == 0U) {
		memcpy(ccm_ctx.rx_out, ccm_ctx.rx_in, 2);
		ccm_ctx.empty = true;
		ccm_ctx.mic_valid = true;
		ccm_ctx.done = true;
		return 1;
	}
	if (ccm_ctx.rx_in->len <= BLE_CCM_MIC_SIZE) {
		return 0;
	}

	id = ccm_key_import(&ccm_ctx.config, PSA_KEY_USAGE_DECRYPT);
	if (id == 0) {
		return 0;
	}
	ccm_nonce_build(&ccm_ctx.config, nonce);
	aad = *(uint8_t *)ccm_ctx.rx_in & BLE_CCM_AAD_MASK;
	status = psa_aead_decrypt(id, alg, nonce, sizeof(nonce), &aad, sizeof(aad),
				  (uint8_t *)ccm_ctx.rx_in + 2, ccm_ctx.rx_in->len,
				  (uint8_t *)ccm_ctx.rx_out + 2,
				  ccm_ctx.rx_in->len - BLE_CCM_MIC_SIZE, &out_len);
	(void)psa_destroy_key(id);
	if (status != PSA_SUCCESS || out_len != ccm_ctx.rx_in->len - BLE_CCM_MIC_SIZE) {
		return 0;
	}

	*(uint8_t *)ccm_ctx.rx_out = *(uint8_t *)ccm_ctx.rx_in;
	ccm_ctx.rx_out->len = out_len;
	ccm_ctx.mic_valid = true;
	ccm_ctx.done = true;
	return 1;
}

/* [software] */
uint32_t radio_ccm_mic_is_valid(void)
{
	return ccm_ctx.mic_valid;
}

/* [software] OpenISA uses this to report an empty received PDU. */
uint32_t radio_ccm_is_available(void)
{
	return ccm_ctx.empty;
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
	unsigned int key = irq_lock();

	first = atomic_cas(&efr32_radio_halted, 0, 1);
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
	k_deadline_event_budget_set(&efr32_radio_rx_event,
				    k_ns_to_cyc_ceil32(CONFIG_EFR32_RADIO_WCET_RX_LAUNCH_NS));
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
