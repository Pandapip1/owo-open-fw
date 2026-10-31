/*
 * Tick counter of the ticker (Zephyr's hal/cntr.h contract) on the kernel's scheduler clock
 * service. SPDX-License-Identifier: Apache-2.0
 *
 * The ticker counts 32.768 kHz ticks (HAL_TICKER_CNTR_CLK_UNIT_FSEC, hal/efr32/ticker.h). They are
 * derived from the service's logical timebase, of which a tick is an exact multiple, so they stay
 * continuous across the service's switches between its active and deep-sleep clocks. The ticker
 * compare, the LLL launch and the QEMU TIFS wake are independent scheduler clock events: the
 * service, not this file, chooses which hardware compare delivers each. Their handlers only make
 * work runnable: the ticker one releases the ULL worker, the others wake persistent jobs.
 *
 * UNVALIDATED ON HARDWARE.
 */
#include <stdint.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/kernel/clock_event.h>
#include <zephyr/kernel/deferred_release.h>
#include <zephyr/sys/atomic.h>

#include "hal/cntr.h"
#include "hal/swi.h"
#include "hal/efr32/cntr_efr32.h"
#include "hal/efr32/ll_threads.h"
#include "hal/efr32/ll_deadlines.h"
#include "hal/debug.h"

#if defined(CONFIG_EFR32_RADIO_BRINGUP_TRACE)
#define EFR32_CNTR_TRACE_ONCE(format, ...) \
	do { \
		static bool efr32_trace_once_; \
		if (!efr32_trace_once_) { \
			efr32_trace_once_ = true; \
			printk("efr32-radio trace: " format "\n", ##__VA_ARGS__); \
		} \
	} while (0)
#else
#define EFR32_CNTR_TRACE_ONCE(...) do { } while (0)
#endif

#define TICK_HZ          32768U
#define LOGICAL_PER_TICK (CONFIG_CLOCK_EVENT_SCHED_FREQUENCY / TICK_HZ)
BUILD_ASSERT(CONFIG_CLOCK_EVENT_SCHED_FREQUENCY % TICK_HZ == 0,
	     "a ticker tick must be a whole number of scheduler clock cycles");

static uint8_t refcount;
static atomic_t cmp_fired;
/* Instant the armed ticker compare falls due: the worker run it releases is due then. */
static atomic_t ticker_deadline;

K_CLOCK_EVENT_DEFINE(ticker_event);
K_CLOCK_EVENT_DEFINE(radio_launch_event);
K_CLOCK_EVENT_DEFINE(qemu_tifs_event);
static struct k_spinlock radio_event_lock;
static struct {
	uint32_t event_generation;
	uint32_t generation;
	uint32_t instant;
	uint32_t not_before;
	int deadline;
	uint8_t trx;
	bool hold;
} radio_launch;
static struct {
	uint32_t event_generation;
	uint32_t generation;
	uint32_t not_before;
	int deadline;
} qemu_tifs;

/* Logical time of the 32-bit tick @p tick nearest to now. */
static uint64_t tick_to_logical(uint32_t tick)
{
	uint64_t now = k_clock_event_sched_now() / LOGICAL_PER_TICK;

	return (now + (int64_t)(int32_t)(tick - (uint32_t)now)) * LOGICAL_PER_TICK;
}

/* Whether @p deadline can no longer be armed on @p event. */
static bool too_close(struct k_clock_event *event, uint64_t deadline)
{
	uint64_t now = k_clock_event_sched_now();

	return deadline <= now || deadline - now < k_clock_event_min_arm_cycles(event);
}

static void ticker_fire(void)
{
	EFR32_CNTR_TRACE_ONCE("ticker-fire now=%u", (uint32_t)k_clock_event_sched_now());
	atomic_set(&cmp_fired, 1);
	efr32_ll_worker_release_due((uint32_t)atomic_get(&ticker_deadline));
}

static void ticker_expired(struct k_clock_event *event, uint32_t generation, void *user_data)
{
	ARG_UNUSED(event);
	ARG_UNUSED(generation);
	ARG_UNUSED(user_data);
	ticker_fire();
}

static void radio_launch_expired(struct k_clock_event *event, uint32_t event_generation,
				 void *user_data)
{
	uint32_t armed_generation;
	uint32_t generation;
	uint32_t instant;
	uint32_t not_before;
	int deadline;
	uint8_t trx;
	bool hold;

	ARG_UNUSED(event);
	ARG_UNUSED(user_data);
	K_SPINLOCK(&radio_event_lock) {
		armed_generation = radio_launch.event_generation;
		generation = radio_launch.generation;
		instant = radio_launch.instant;
		not_before = radio_launch.not_before;
		deadline = radio_launch.deadline;
		trx = radio_launch.trx;
		hold = radio_launch.hold;
	}
	if (armed_generation == event_generation) {
		efr32_radio_launch_wake(trx, hold, generation, instant, not_before, deadline);
	}
}

static void qemu_tifs_expired(struct k_clock_event *event, uint32_t event_generation,
			      void *user_data)
{
	uint32_t armed_generation;
	uint32_t generation;
	uint32_t not_before;
	int deadline;

	ARG_UNUSED(event);
	ARG_UNUSED(user_data);
	K_SPINLOCK(&radio_event_lock) {
		armed_generation = qemu_tifs.event_generation;
		generation = qemu_tifs.generation;
		not_before = qemu_tifs.not_before;
		deadline = qemu_tifs.deadline;
	}
	if (armed_generation == event_generation) {
		efr32_radio_qemu_tifs_wake(generation, not_before, deadline);
	}
}

bool cntr_cmp_take(void)
{
	bool fired = atomic_set(&cmp_fired, 0) != 0;

	if (fired) {
		EFR32_CNTR_TRACE_ONCE("ticker-consumed");
	}
	return fired;
}

void cntr_init(void)
{
	LL_ASSERT(k_clock_event_sched_init(&ticker_event, ticker_expired, NULL) == 0);
	LL_ASSERT(k_clock_event_sched_init(&radio_launch_event, radio_launch_expired, NULL) == 0);
	LL_ASSERT(k_clock_event_sched_init(&qemu_tifs_event, qemu_tifs_expired, NULL) == 0);
	EFR32_CNTR_TRACE_ONCE("counter-init now=%u min=%u",
			      (uint32_t)k_clock_event_sched_now(),
			      k_clock_event_min_arm_cycles(&ticker_event));
}

uint32_t cntr_start(void)
{
	EFR32_CNTR_TRACE_ONCE("counter-start refcount=%u", refcount);
	/* The scheduler clock always runs; only the users are counted. */
	return refcount++ ? 1U : 0U;
}

uint32_t cntr_stop(void)
{
	LL_ASSERT(refcount);
	if (--refcount) {
		return 1;
	}
	k_clock_event_cancel(&ticker_event);
	cntr_radio_cmp_cancel();
	cntr_qemu_tifs_cmp_cancel();
	return 0;
}

uint32_t cntr_cnt_get(void)
{
	return (uint32_t)(k_clock_event_sched_now() / LOGICAL_PER_TICK);
}

void cntr_cmp_set(uint8_t cmp, uint32_t value)
{
	uint64_t deadline = tick_to_logical(value);
	EFR32_CNTR_TRACE_ONCE("counter-compare value=%u deadline=%u now=%u",
			      value, (uint32_t)deadline,
			      (uint32_t)k_clock_event_sched_now());

	LL_ASSERT(cmp == 0);
	atomic_set(&ticker_deadline, (uint32_t)deadline);
	/* Like a Counter alarm with EXPIRE_WHEN_LATE: a late compare fires now. */
	if (too_close(&ticker_event, deadline) ||
	    k_clock_event_arm_abs(&ticker_event, (uint32_t)deadline) != 0) {
		k_clock_event_cancel(&ticker_event);
		ticker_fire();
	}
}

/*
 * Admit the run of @p event that a clock event armed for @p arm_at releases: it may not start
 * before @p not_before and must start by @p deadline.
 */
static void cntr_admit(struct k_deadline_event *event, uint64_t arm_at, uint32_t not_before,
		       int deadline)
{
#if defined(CONFIG_EFR32_RADIO_ADMISSION)
	struct k_clock_event_sched_plan plan;

	if (event != NULL && k_clock_event_sched_plan(arm_at, &plan) == 0) {
		/* Infeasibility goes to the missed-deadline policy, which stops the radio. */
		(void)k_deadline_event_admit_plan(event, &plan, (uint32_t)arm_at, not_before,
						  deadline);
	}
#else
	ARG_UNUSED(event);
	ARG_UNUSED(arm_at);
	ARG_UNUSED(not_before);
	ARG_UNUSED(deadline);
#endif
}

static void cntr_admit_cancel(struct k_deadline_event *event)
{
#if defined(CONFIG_EFR32_RADIO_ADMISSION)
	if (event != NULL) {
		k_deadline_event_admit_cancel(event);
	}
#else
	ARG_UNUSED(event);
#endif
}

uint32_t cntr_tick_cycles(uint32_t tick)
{
	return (uint32_t)tick_to_logical(tick);
}

/*
 * A TX launch, and the RX launch of a Peripheral's receive window (@p tolerance_us nonzero), is
 * a timed command (hal/efr32/ll_deadlines.h): its job is released the measured wake latency
 * before its not-before bound, waits that bound out at its priority and holds TXEN or SWRXEN to
 * the instant. A scan window's RX launch has no instant to hold: it is released at the instant
 * and may start anywhere in its window. All are admitted when armed.
 */
void cntr_radio_cmp_set(uint8_t trx, uint32_t instant, uint32_t tolerance_us,
			uint32_t generation)
{
	uint64_t now = k_clock_event_sched_now();
	uint64_t target = now + (uint64_t)(int64_t)(int32_t)(instant - (uint32_t)now);
	uint64_t arm_at;
	uint32_t not_before;
	uint32_t event_generation;
	int deadline;
	bool hold = trx != 0U || tolerance_us != 0U;
	unsigned int key;

	if (trx) {
		not_before = efr32_dl_tx_launch_not_before((uint32_t)target);
		deadline = efr32_dl_tx_launch_start_by((uint32_t)target);
		arm_at = target - ((uint32_t)target - not_before) - EFR32_DL_WAKE_CYC;
		cntr_admit(efr32_radio_launch_event(trx), arm_at, not_before, deadline);
	} else if (hold) {
		not_before = efr32_dl_rx_window_not_before((uint32_t)target);
		deadline = efr32_dl_rx_window_start_by((uint32_t)target, tolerance_us);
		arm_at = target - ((uint32_t)target - not_before) - EFR32_DL_WAKE_CYC;
		cntr_admit(efr32_radio_launch_event(trx), arm_at, not_before, deadline);
	} else {
		not_before = (uint32_t)target;
		deadline = efr32_dl_rx_launch_start_by((uint32_t)target);
		arm_at = target;
		/* Released when the wake path has run: admitted that late. */
		cntr_admit(efr32_radio_launch_event(trx), arm_at,
			   (uint32_t)target + EFR32_DL_WAKE_CYC, deadline);
	}
	key = irq_lock();
	/* Invalidate the old expiry before replacing the payload it would consume. */
	k_clock_event_cancel(&radio_launch_event);
	K_SPINLOCK(&radio_event_lock) { radio_launch.event_generation = 0U; }
	/* Too close to arm: release at once; the job still holds to the instant. */
	if (too_close(&radio_launch_event, arm_at) ||
	    k_clock_event_arm_abs_with_generation(&radio_launch_event, (uint32_t)arm_at,
						  &event_generation) != 0) {
		irq_unlock(key);
		efr32_radio_launch_wake(trx, hold, generation, (uint32_t)target, not_before,
					deadline);
		return;
	}
	/* IRQs remain locked: the handler cannot observe a half-published arm. */
	K_SPINLOCK(&radio_event_lock) {
		radio_launch.event_generation = event_generation;
		radio_launch.trx = trx;
		radio_launch.hold = hold;
		radio_launch.generation = generation;
		radio_launch.instant = (uint32_t)target;
		radio_launch.not_before = not_before;
		radio_launch.deadline = deadline;
	}
	irq_unlock(key);
}

void cntr_radio_cmp_cancel(void)
{
	unsigned int key = irq_lock();

	k_clock_event_cancel(&radio_launch_event);
	K_SPINLOCK(&radio_event_lock) { radio_launch.event_generation = 0U; }
	irq_unlock(key);
	cntr_admit_cancel(efr32_radio_launch_event(0U));
	cntr_admit_cancel(efr32_radio_launch_event(1U));
}

/*
 * The TIFS clock event, independent of the LLL event launch. Its handler only wakes a job.
 *
 * cntr_qemu_tifs_arm_at() arms a timed release of the QEMU TIFS job that must start by
 * @p deadline. With @p lead (a turnaround, held to its instant by the job) it is released the
 * measured wake latency before @p not_before, at once when that is too close, and waits the
 * bound out at its priority; without (the no-response timeout) it is released at @p not_before
 * and admitted as arriving the wake latency later.
 * cntr_rx_timeout_arm_after_us() arms a hardware RX timeout, a bound that may end late: a target
 * inside the clock event's minimum arm lead is stretched to the lead.
 */
void cntr_qemu_tifs_arm_at(uint32_t not_before, int deadline, uint32_t generation, bool lead)
{
	uint32_t event_generation;
	uint64_t now = k_clock_event_sched_now();
	uint64_t arm_at = now + (uint64_t)(int32_t)(not_before - (uint32_t)now) -
			  (lead ? EFR32_DL_WAKE_CYC : 0U);
	bool release_now = false;
	unsigned int key = irq_lock();

	k_clock_event_cancel(&qemu_tifs_event);
	K_SPINLOCK(&radio_event_lock) { qemu_tifs.event_generation = 0U; }
	if (too_close(&qemu_tifs_event, arm_at) ||
	    k_clock_event_arm_abs_with_generation(&qemu_tifs_event, (uint32_t)arm_at,
						  &event_generation) != 0) {
		release_now = true;
		arm_at = now;
	} else {
		K_SPINLOCK(&radio_event_lock) {
			qemu_tifs.event_generation = event_generation;
			qemu_tifs.generation = generation;
			qemu_tifs.not_before = not_before;
			qemu_tifs.deadline = deadline;
		}
	}
	irq_unlock(key);
	cntr_admit(efr32_radio_tifs_event(), arm_at,
		   lead ? not_before : not_before + EFR32_DL_WAKE_CYC, deadline);
	if (release_now) {
		efr32_radio_qemu_tifs_wake(generation, not_before, deadline);
	}
}

void cntr_rx_timeout_arm_after_us(uint32_t delay_us, uint32_t generation)
{
	uint32_t event_generation;
	int ret = -ETIME;
	unsigned int key = irq_lock();

	k_clock_event_cancel(&qemu_tifs_event);
	K_SPINLOCK(&radio_event_lock) { qemu_tifs.event_generation = 0U; }
	for (uint8_t attempt = 0U; attempt < 2U && ret == -ETIME; attempt++) {
		uint64_t now = k_clock_event_sched_now();
		uint64_t delay = DIV_ROUND_UP((uint64_t)delay_us * CONFIG_CLOCK_EVENT_SCHED_FREQUENCY,
					      USEC_PER_SEC);
		uint64_t lead = (uint64_t)k_clock_event_min_arm_cycles(&qemu_tifs_event) + 1U;

		delay = MAX(delay, lead);
		ret = k_clock_event_arm_abs_with_generation(&qemu_tifs_event, (uint32_t)(now + delay),
							    &event_generation);
		if (ret == 0) {
			K_SPINLOCK(&radio_event_lock) {
				qemu_tifs.event_generation = event_generation;
				qemu_tifs.generation = generation;
				qemu_tifs.not_before = (uint32_t)(now + delay);
				qemu_tifs.deadline = (int)(uint32_t)(now + delay);
			}
		}
	}
	irq_unlock(key);
	LL_ASSERT(ret == 0);
}

void cntr_rx_timeout_arm_at(uint32_t at, uint32_t generation)
{
	uint64_t now = k_clock_event_sched_now();
	int32_t ahead = (int32_t)(at - (uint32_t)now);
	uint32_t delay_us = ahead > 0 ? k_cyc_to_us_ceil32((uint32_t)ahead) : 0U;

	/* The same bound, from now: it may end late, never early. */
	cntr_rx_timeout_arm_after_us(delay_us, generation);
}

void cntr_qemu_tifs_cmp_cancel(void)
{
	unsigned int key = irq_lock();

	k_clock_event_cancel(&qemu_tifs_event);
	K_SPINLOCK(&radio_event_lock) { qemu_tifs.event_generation = 0U; }
	irq_unlock(key);
	cntr_admit_cancel(efr32_radio_tifs_event());
}

void cntr_shutdown(void)
{
	unsigned int key = irq_lock();

	k_clock_event_cancel(&ticker_event);
	(void)atomic_set(&cmp_fired, 0);
	irq_unlock(key);
	cntr_radio_cmp_cancel();
	cntr_qemu_tifs_cmp_cancel();
}
