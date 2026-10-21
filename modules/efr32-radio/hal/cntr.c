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

#define TICK_HZ          32768U
#define LOGICAL_PER_TICK (CONFIG_CLOCK_EVENT_SCHED_FREQUENCY / TICK_HZ)
BUILD_ASSERT(CONFIG_CLOCK_EVENT_SCHED_FREQUENCY % TICK_HZ == 0,
	     "a ticker tick must be a whole number of scheduler clock cycles");

static uint8_t refcount;
static atomic_t cmp_fired;

K_CLOCK_EVENT_DEFINE(ticker_event);
K_CLOCK_EVENT_DEFINE(radio_launch_event);
K_CLOCK_EVENT_DEFINE(qemu_tifs_event);
static struct k_spinlock radio_event_lock;
static struct {
	uint32_t event_generation;
	uint32_t generation;
	uint8_t trx;
} radio_launch;
static struct {
	uint32_t event_generation;
	uint32_t generation;
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
	uint8_t trx;

	ARG_UNUSED(event);
	ARG_UNUSED(user_data);
	K_SPINLOCK(&radio_event_lock) {
		armed_generation = radio_launch.event_generation;
		generation = radio_launch.generation;
		trx = radio_launch.trx;
	}
	if (armed_generation == event_generation) {
		efr32_radio_launch_wake(trx, generation);
	}
}

static void qemu_tifs_expired(struct k_clock_event *event, uint32_t event_generation,
			      void *user_data)
{
	uint32_t armed_generation;
	uint32_t generation;

	ARG_UNUSED(event);
	ARG_UNUSED(user_data);
	K_SPINLOCK(&radio_event_lock) {
		armed_generation = qemu_tifs.event_generation;
		generation = qemu_tifs.generation;
	}
	if (armed_generation == event_generation) {
		efr32_radio_qemu_tifs_wake(generation);
	}
}

bool cntr_cmp_take(void)
{
	return atomic_set(&cmp_fired, 0) != 0;
}

void cntr_init(void)
{
	LL_ASSERT(k_clock_event_sched_init(&ticker_event, ticker_expired, NULL) == 0);
	LL_ASSERT(k_clock_event_sched_init(&radio_launch_event, radio_launch_expired, NULL) == 0);
	LL_ASSERT(k_clock_event_sched_init(&qemu_tifs_event, qemu_tifs_expired, NULL) == 0);
}

uint32_t cntr_start(void)
{
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

	LL_ASSERT(cmp == 0);
	/* Like a Counter alarm with EXPIRE_WHEN_LATE: a late compare fires now. */
	if (too_close(&ticker_event, deadline) ||
	    k_clock_event_arm_abs(&ticker_event, (uint32_t)deadline) != 0) {
		k_clock_event_cancel(&ticker_event);
		ticker_fire();
	}
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
		efr32_radio_launch_wake(trx, generation);
		return;
	}
	/* IRQs remain locked: the handler cannot observe a half-published arm. */
	K_SPINLOCK(&radio_event_lock) {
		radio_launch.event_generation = event_generation;
		radio_launch.trx = trx;
		radio_launch.generation = generation;
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
 * A QEMU-only software TIFS delay, independent of the normal LLL event launch. Its handler calls
 * only the wake mailbox hook; the persistent Zephyr job owns all RAC state changes.
 */
void cntr_qemu_tifs_arm_after_us(uint32_t delay_us, uint32_t generation)
{
	uint32_t event_generation;
	int ret = -ETIME;
	unsigned int key = irq_lock();

	k_clock_event_cancel(&qemu_tifs_event);
	K_SPINLOCK(&radio_event_lock) { qemu_tifs.event_generation = 0U; }
	/*
	 * TIFS is a physical no-earlier-than boundary, unlike an LLL launch's start-by deadline: an
	 * interval inside the minimum lead is stretched to that lead, never released early.
	 */
	for (uint8_t attempt = 0U; attempt < 2U && ret == -ETIME; attempt++) {
		uint64_t now = k_clock_event_sched_now();
		uint64_t delay = DIV_ROUND_UP((uint64_t)delay_us * CONFIG_CLOCK_EVENT_SCHED_FREQUENCY,
					      USEC_PER_SEC);

		delay = MAX(delay, (uint64_t)k_clock_event_min_arm_cycles(&qemu_tifs_event) + 1U);
		ret = k_clock_event_arm_abs_with_generation(&qemu_tifs_event, (uint32_t)(now + delay),
							    &event_generation);
	}
	if (ret == 0) {
		K_SPINLOCK(&radio_event_lock) {
			qemu_tifs.event_generation = event_generation;
			qemu_tifs.generation = generation;
		}
	}
	irq_unlock(key);
	LL_ASSERT(ret == 0);
}

void cntr_qemu_tifs_cmp_cancel(void)
{
	unsigned int key = irq_lock();

	k_clock_event_cancel(&qemu_tifs_event);
	K_SPINLOCK(&radio_event_lock) { qemu_tifs.event_generation = 0U; }
	irq_unlock(key);
}
