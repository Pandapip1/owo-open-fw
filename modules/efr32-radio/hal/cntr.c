/*
 * 32.768 kHz tick counter of the ticker (Zephyr's hal/cntr.h contract), built on the Zephyr Counter
 * API so it needs no register knowledge. SPDX-License-Identifier: Apache-2.0
 *
 * Devicetree alias `ll-counter` points at the free-running 32-bit BURTC clocked at 32768 Hz. For the
 * radio configurations, that same device is Zephyr's tickless system clock; its driver is the sole
 * COMP/IRQ18 owner and arbitrates Counter clients with kernel deadlines. The Counter callback is
 * relayed to the ticker worker through the software interrupt HAL_SWI_WORKER_IRQ
 * (hal/efr32/swi.h), because the lower link layer connects its own handler to that interrupt number.
 *
 * UNVALIDATED: not run on a counter, only compiled.
 */
#include <stdint.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/kernel/clock_event.h>
#include <zephyr/drivers/counter/silabs_burtc.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/device.h>
#include <zephyr/drivers/counter.h>

#include "hal/cntr.h"
#include "hal/swi.h"
#include "hal/efr32/cntr_efr32.h"
#include "hal/debug.h"

#define LL_CNTR_NODE DT_ALIAS(ll_counter)
BUILD_ASSERT(DT_NODE_HAS_STATUS_OKAY(LL_CNTR_NODE), "alias ll-counter must be an enabled counter");

static const struct device *const cntr_dev = DEVICE_DT_GET(LL_CNTR_NODE);
static uint8_t refcount;
static atomic_t cmp_fired;
static uint32_t ticker_compare;
static bool ticker_armed;
static void alarm_cb(const struct device *, uint8_t, uint32_t, void *);

/* Independent generic clients; BURTC owns their shared COMP arbitration. */
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

static bool deadline_due(uint32_t deadline, uint32_t now)
{
	return (int32_t)(deadline - now) <= 0;
}

static void radio_launch_expired(struct k_clock_event *event, uint32_t event_generation,
				 void *user_data)
{
	uint32_t armed_generation;
	ARG_UNUSED(event);
	ARG_UNUSED(user_data);
	uint32_t generation;
	uint8_t trx;
	K_SPINLOCK(&radio_event_lock) {
		armed_generation = radio_launch.event_generation;
		generation = radio_launch.generation;
		trx = radio_launch.trx;
	}
	if (armed_generation != event_generation) {
		return;
	}
	efr32_radio_launch_wake(trx, generation);
}

static void qemu_tifs_expired(struct k_clock_event *event, uint32_t event_generation,
			      void *user_data)
{
	ARG_UNUSED(event); ARG_UNUSED(user_data);
	uint32_t armed_generation;
	uint32_t generation;
	K_SPINLOCK(&radio_event_lock) {
		armed_generation = qemu_tifs.event_generation;
		generation = qemu_tifs.generation;
	}
	if (armed_generation == event_generation) {
		efr32_radio_qemu_tifs_wake(generation);
	}
}

/* Called with local IRQs locked: all CC0 client state has one owner. */
static void cmp_program_locked(void)
{
	struct counter_alarm_cfg cfg = {
		.callback = alarm_cb,
		.flags = COUNTER_ALARM_CFG_ABSOLUTE | COUNTER_ALARM_CFG_EXPIRE_WHEN_LATE,
	};
	(void)counter_cancel_channel_alarm(cntr_dev, 0);
	if (!ticker_armed) {
		return;
	}
	cfg.ticks = ticker_compare;
	LL_ASSERT(counter_set_channel_alarm(cntr_dev, 0, &cfg) == 0);
}

bool cntr_cmp_take(void)
{
	return atomic_set(&cmp_fired, 0) != 0;
}

static void alarm_cb(const struct device *dev, uint8_t chan, uint32_t ticks, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(ticks);
	ARG_UNUSED(user_data);
	LL_ASSERT(chan == 0U);
	bool ticker_due = ticker_armed && deadline_due(ticker_compare, cntr_cnt_get());
	/* A not-yet-due callback must leave the ticker compare armed. */
	if (ticker_due) {
		ticker_armed = false;
	}
	cmp_program_locked();
	if (ticker_due) {
		atomic_set(&cmp_fired, 1);
		hal_swi_worker_pend();
	}
}

void cntr_init(void)
{
	LL_ASSERT(device_is_ready(cntr_dev));
	/* The counter is clocked at 32768 Hz: HAL_TICKER_CNTR_CLK_UNIT_FSEC (hal/efr32/ticker.h). */
	LL_ASSERT(counter_get_frequency(cntr_dev) == 32768U);
	LL_ASSERT(counter_get_top_value(cntr_dev) == UINT32_MAX);
	LL_ASSERT(counter_silabs_burtc_clock_event_init(cntr_dev, &radio_launch_event, radio_launch_expired, NULL) == 0);
	LL_ASSERT(counter_silabs_burtc_clock_event_init(cntr_dev, &qemu_tifs_event, qemu_tifs_expired, NULL) == 0);
}

uint32_t cntr_start(void)
{
	if (refcount++) {
		return 1;
	}
	LL_ASSERT(counter_start(cntr_dev) == 0);
	return 0;
}

uint32_t cntr_stop(void)
{
	LL_ASSERT(refcount);
	if (--refcount) {
		return 1;
	}
	unsigned int key = irq_lock();
	ticker_armed = false;
	cmp_program_locked();
	k_clock_event_cancel(&radio_launch_event);
	k_clock_event_cancel(&qemu_tifs_event);
	K_SPINLOCK(&radio_event_lock) {
		radio_launch.event_generation = 0U;
		qemu_tifs.event_generation = 0U;
	}
	irq_unlock(key);
	LL_ASSERT(counter_stop(cntr_dev) == 0);
	return 0;
}

uint32_t cntr_cnt_get(void)
{
	uint32_t ticks = 0;

	(void)counter_get_value(cntr_dev, &ticks);
	return ticks;
}

void cntr_cmp_set(uint8_t cmp, uint32_t value)
{
	LL_ASSERT(cmp == 0);
	unsigned int key = irq_lock();
	ticker_compare = value;
	ticker_armed = true;
	cmp_program_locked();
	irq_unlock(key);
}

void cntr_radio_cmp_set(uint8_t trx, uint32_t value, uint32_t generation)
{
	uint32_t now;
	uint32_t event_generation;
	int ret;
	unsigned int key = irq_lock();

	/* Invalidate the old IRQ before replacing the payload it would consume. */
	k_clock_event_cancel(&radio_launch_event);
	K_SPINLOCK(&radio_event_lock) { radio_launch.event_generation = 0U; }
	now = k_clock_event_now(&radio_launch_event);
	if (deadline_due(value, now) ||
	    (value - now) < k_clock_event_min_arm_cycles(&radio_launch_event)) {
		irq_unlock(key);
		/* The scheduler must run an already-unarmable release immediately. */
		efr32_radio_launch_wake(trx, generation);
		return;
	}
	ret = k_clock_event_arm_abs_with_generation(&radio_launch_event, value,
					    &event_generation);
	if (ret == 0) {
		/* IRQs remain locked: the handler cannot observe a half-published arm. */
		K_SPINLOCK(&radio_event_lock) {
			radio_launch.event_generation = event_generation;
			radio_launch.trx = trx;
			radio_launch.generation = generation;
		}
		irq_unlock(key);
		return;
	}
	irq_unlock(key);
	if (ret == -ETIME) {
		/* Backend final-lead check raced the precheck: do not lose the release. */
		efr32_radio_launch_wake(trx, generation);
		return;
	}
	LL_ASSERT(false);
	return;
}
void cntr_radio_cmp_cancel(void)
{
	unsigned int key = irq_lock();
	k_clock_event_cancel(&radio_launch_event);
	K_SPINLOCK(&radio_event_lock) { radio_launch.event_generation = 0U; }
	irq_unlock(key);
}

/*
 * A QEMU-only software TIFS delay is a third CC0 client, independent of the
 * normal LLL event launch. The BURTC ISR calls only the wake mailbox hook;
 * its persistent Zephyr job owns all RAC state changes.
 */
void cntr_qemu_tifs_cmp_set(uint32_t value, uint32_t generation)
{
	uint32_t now;
	uint32_t deadline;
	uint32_t event_generation;
	int ret = -ETIME;
	unsigned int key = irq_lock();

	k_clock_event_cancel(&qemu_tifs_event);
	K_SPINLOCK(&radio_event_lock) { qemu_tifs.event_generation = 0U; }
	/*
	 * TIFS is a physical no-earlier-than boundary, unlike an LLL launch's
	 * start-by deadline.  An arm inside the event's lead floor therefore cannot
	 * release this job immediately: preserve the lower bound by clamping it to
	 * the first armable cycle.  The provider samples its own counter, so retry
	 * once if that sample ticked past ours.
	 */
	for (uint8_t attempt = 0U; attempt < 2U && ret == -ETIME; attempt++) {
		now = k_clock_event_now(&qemu_tifs_event);
		deadline = value;
		if (deadline_due(value, now) ||
		    (value - now) < k_clock_event_min_arm_cycles(&qemu_tifs_event)) {
			deadline = now + k_clock_event_min_arm_cycles(&qemu_tifs_event);
		}
		ret = k_clock_event_arm_abs_with_generation(&qemu_tifs_event, deadline,
							    &event_generation);
	}
	if (ret == 0) {
		K_SPINLOCK(&radio_event_lock) {
			qemu_tifs.event_generation = event_generation;
			qemu_tifs.generation = generation;
		}
		irq_unlock(key);
		return;
	}
	irq_unlock(key);
	LL_ASSERT(false);
}

void cntr_qemu_tifs_cmp_cancel(void)
{
	unsigned int key = irq_lock();
	k_clock_event_cancel(&qemu_tifs_event);
	K_SPINLOCK(&radio_event_lock) { qemu_tifs.event_generation = 0U; }
	irq_unlock(key);
}
