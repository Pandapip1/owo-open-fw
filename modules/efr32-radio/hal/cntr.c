/*
 * 32.768 kHz tick counter of the ticker (Zephyr's hal/cntr.h contract), built on the Zephyr Counter
 * API so it needs no register knowledge. SPDX-License-Identifier: Apache-2.0
 *
 * Devicetree alias `ll-counter` must point at a free-running 32-bit counter clocked at 32768 Hz that
 * is not the system clock (the board overlay: `&burtc0`, since RTCC is the sleeptimer). The compare
 * interrupt is relayed to the ticker worker through the software interrupt HAL_SWI_WORKER_IRQ
 * (hal/efr32/swi.h), because the lower link layer connects its own handler to that interrupt number.
 *
 * UNVALIDATED: not run on a counter, only compiled.
 */
#include <stdint.h>
#include <zephyr/kernel.h>
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
static uint32_t radio_compare;
static uint32_t radio_generation;
static uint8_t radio_trx;
static bool ticker_armed;
static bool radio_armed;
static void alarm_cb(const struct device *, uint8_t, uint32_t, void *);

/* Called with local IRQs locked: all CC0 client state has one owner. */
static void cmp_program_locked(void)
{
	struct counter_alarm_cfg cfg = {
		.callback = alarm_cb,
		.flags = COUNTER_ALARM_CFG_ABSOLUTE | COUNTER_ALARM_CFG_EXPIRE_WHEN_LATE,
	};

	(void)counter_cancel_channel_alarm(cntr_dev, 0);
	if (!ticker_armed && !radio_armed) {
		return;
	}
	if (!ticker_armed || (radio_armed &&
		(int32_t)(radio_compare - ticker_compare) < 0)) {
		cfg.ticks = radio_compare;
	} else {
		cfg.ticks = ticker_compare;
	}
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
	uint32_t now = cntr_cnt_get();
	bool ticker_due = ticker_armed && (int32_t)(ticker_compare - now) <= 0;
	bool radio_due = radio_armed && (int32_t)(radio_compare - now) <= 0;
	uint8_t trx = radio_trx;
	uint32_t generation = radio_generation;

	if (ticker_due) {
		ticker_armed = false;
	}
	if (radio_due) {
		radio_armed = false;
	}
	cmp_program_locked();
	if (ticker_due) {
		atomic_set(&cmp_fired, 1);
		hal_swi_worker_pend();
	}
	if (radio_due) {
		efr32_radio_launch_wake(trx, generation);
	}
}

void cntr_init(void)
{
	LL_ASSERT(device_is_ready(cntr_dev));
	/* The counter is clocked at 32768 Hz: HAL_TICKER_CNTR_CLK_UNIT_FSEC (hal/efr32/ticker.h). */
	LL_ASSERT(counter_get_frequency(cntr_dev) == 32768U);
	LL_ASSERT(counter_get_top_value(cntr_dev) == UINT32_MAX);
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
	radio_armed = false;
	cmp_program_locked();
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
	unsigned int key = irq_lock();
	radio_trx = trx;
	radio_compare = value;
	radio_generation = generation;
	radio_armed = true;
	cmp_program_locked();
	irq_unlock(key);
}
void cntr_radio_cmp_cancel(void)
{
	unsigned int key = irq_lock();
	radio_armed = false;
	cmp_program_locked();
	irq_unlock(key);
}
