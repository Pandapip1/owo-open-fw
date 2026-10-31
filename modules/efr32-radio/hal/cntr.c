/*
 * Tick counter of the ticker (Zephyr's hal/cntr.h contract) on the kernel's scheduler clock
 * service. SPDX-License-Identifier: Apache-2.0
 *
 * The ticker counts 32.768 kHz ticks (HAL_TICKER_CNTR_CLK_UNIT_FSEC, hal/efr32/ticker.h). They are
 * derived from the service's logical timebase, of which a tick is an exact multiple, so they stay
 * continuous across the service's switches between its active and deep-sleep clocks. The ticker
 * compare, the LLL launch and the QEMU TIFS wake are independent scheduler clock events: the
 * service, not this file, chooses which hardware compare delivers each. Their handlers only make
 * work runnable: the ticker one pends HAL_SWI_WORKER_IRQ, the others wake persistent jobs.
 *
 * UNVALIDATED ON HARDWARE.
 */
#include <stdint.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/kernel/clock_event.h>
#include <zephyr/sys/atomic.h>

#include "hal/cntr.h"
#include "hal/swi.h"
#include "hal/efr32/cntr_efr32.h"
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
/* Start-by deadline of the ticker work, for the LL thread's lateness check. */
static atomic_t ticker_deadline;

K_CLOCK_EVENT_DEFINE(ticker_event);
K_CLOCK_EVENT_DEFINE(radio_launch_event);
K_CLOCK_EVENT_DEFINE(qemu_tifs_event);
static struct k_spinlock radio_event_lock;
static struct {
	uint32_t event_generation;
	uint32_t generation;
	uint32_t deadline;
	uint8_t trx;
} radio_launch;
static struct {
	uint32_t event_generation;
	uint32_t generation;
	uint32_t deadline;
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
	hal_swi_worker_pend();
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
	uint32_t deadline;
	uint8_t trx;

	ARG_UNUSED(event);
	ARG_UNUSED(user_data);
	K_SPINLOCK(&radio_event_lock) {
		armed_generation = radio_launch.event_generation;
		generation = radio_launch.generation;
		deadline = radio_launch.deadline;
		trx = radio_launch.trx;
	}
	if (armed_generation == event_generation) {
		efr32_radio_launch_wake(trx, generation, deadline);
	}
}

static void qemu_tifs_expired(struct k_clock_event *event, uint32_t event_generation,
			      void *user_data)
{
	uint32_t armed_generation;
	uint32_t generation;
	uint32_t deadline;

	ARG_UNUSED(event);
	ARG_UNUSED(user_data);
	K_SPINLOCK(&radio_event_lock) {
		armed_generation = qemu_tifs.event_generation;
		generation = qemu_tifs.generation;
		deadline = qemu_tifs.deadline;
	}
	if (armed_generation == event_generation) {
		efr32_radio_qemu_tifs_wake(generation, deadline);
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

/* Start-by deadline of the ticker work the LL thread is about to run. */
uint32_t cntr_ticker_deadline(void)
{
	return (uint32_t)atomic_get(&ticker_deadline);
}

/* Start-by deadline of the radio launch work the LLL context is about to run. */
uint32_t cntr_launch_deadline(void)
{
	uint32_t deadline;

	K_SPINLOCK(&radio_event_lock) { deadline = radio_launch.deadline; }
	return deadline;
}

void cntr_radio_cmp_set(uint8_t trx, uint32_t value, uint32_t generation)
{
	uint64_t deadline = tick_to_logical(value);
	uint32_t event_generation;
	unsigned int key = irq_lock();

	/* Invalidate the old expiry before replacing the payload it would consume. */
	k_clock_event_cancel(&radio_launch_event);
	K_SPINLOCK(&radio_event_lock) { radio_launch.event_generation = 0U; }
	/* A launch tick is a start-by deadline: run an unarmable one at once. */
	if (too_close(&radio_launch_event, deadline) ||
	    k_clock_event_arm_abs_with_generation(&radio_launch_event, (uint32_t)deadline,
						  &event_generation) != 0) {
		irq_unlock(key);
		efr32_radio_launch_wake(trx, generation, (uint32_t)deadline);
		return;
	}
	/* IRQs remain locked: the handler cannot observe a half-published arm. */
	K_SPINLOCK(&radio_event_lock) {
		radio_launch.event_generation = event_generation;
		radio_launch.trx = trx;
		radio_launch.generation = generation;
		radio_launch.deadline = (uint32_t)deadline;
	}
	irq_unlock(key);
}

void cntr_radio_cmp_cancel(void)
{
	unsigned int key = irq_lock();

	k_clock_event_cancel(&radio_launch_event);
	K_SPINLOCK(&radio_event_lock) { radio_launch.event_generation = 0U; }
	irq_unlock(key);
}

/*
 * One software delay on the TIFS clock event, independent of the normal LLL event launch. Its
 * handler calls only the wake mailbox hook; the persistent Zephyr job owns all RAC state changes.
 *
 * With @p exact, the expiry is a physical no-earlier-than boundary, unlike an LLL launch's
 * start-by deadline: a target inside the clock event's minimum arm lead is waited out by spinning
 * on the scheduler clock and then woken, neither released early nor stretched late. Without it,
 * the expiry is a timeout that may fire late, and such a target is stretched to the lead instead
 * of spinning the caller.
 */
static void cntr_tifs_event_arm_after_us(uint32_t delay_us, uint32_t generation, bool exact)
{
	uint32_t event_generation;
	uint32_t target = 0U;
	bool wait = false;
	int ret = -ETIME;
	unsigned int key = irq_lock();

	k_clock_event_cancel(&qemu_tifs_event);
	K_SPINLOCK(&radio_event_lock) { qemu_tifs.event_generation = 0U; }
	for (uint8_t attempt = 0U; attempt < 2U && ret == -ETIME; attempt++) {
		uint64_t now = k_clock_event_sched_now();
		uint64_t delay = DIV_ROUND_UP((uint64_t)delay_us * CONFIG_CLOCK_EVENT_SCHED_FREQUENCY,
					      USEC_PER_SEC);
		uint64_t lead = (uint64_t)k_clock_event_min_arm_cycles(&qemu_tifs_event) + 1U;

		if (exact && delay < lead) {
			target = (uint32_t)(now + delay);
			wait = true;
			ret = 0;
			break;
		}
		delay = MAX(delay, lead);
		ret = k_clock_event_arm_abs_with_generation(&qemu_tifs_event, (uint32_t)(now + delay),
							    &event_generation);
		if (ret == 0) {
			K_SPINLOCK(&radio_event_lock) {
				qemu_tifs.event_generation = event_generation;
				qemu_tifs.generation = generation;
				qemu_tifs.deadline = (uint32_t)(now + delay);
			}
		}
	}
	irq_unlock(key);
	LL_ASSERT(ret == 0);
	if (wait) {
		while ((int32_t)(target - (uint32_t)k_clock_event_sched_now()) > 0) {
			arch_nop();
		}
		efr32_radio_qemu_tifs_wake(generation, target);
	}
}

void cntr_qemu_tifs_arm_after_us(uint32_t delay_us, uint32_t generation)
{
	cntr_tifs_event_arm_after_us(delay_us, generation, true);
}

void cntr_rx_timeout_arm_after_us(uint32_t delay_us, uint32_t generation)
{
	cntr_tifs_event_arm_after_us(delay_us, generation, false);
}

void cntr_qemu_tifs_cmp_cancel(void)
{
	unsigned int key = irq_lock();

	k_clock_event_cancel(&qemu_tifs_event);
	K_SPINLOCK(&radio_event_lock) { qemu_tifs.event_generation = 0U; }
	irq_unlock(key);
}
