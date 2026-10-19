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
#include "hal/efr32/cntr_owo.h"
#include "hal/debug.h"

#define LL_CNTR_NODE DT_ALIAS(ll_counter)
BUILD_ASSERT(DT_NODE_HAS_STATUS_OKAY(LL_CNTR_NODE), "alias ll-counter must be an enabled counter");

static const struct device *const cntr_dev = DEVICE_DT_GET(LL_CNTR_NODE);
static uint8_t refcount;
static atomic_t cmp_fired;

bool cntr_cmp_take(void)
{
	return atomic_set(&cmp_fired, 0) != 0;
}

static void alarm_cb(const struct device *dev, uint8_t chan, uint32_t ticks, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(chan);
	ARG_UNUSED(ticks);
	ARG_UNUSED(user_data);

	atomic_set(&cmp_fired, 1);
	hal_swi_worker_pend();
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
	(void)counter_cancel_channel_alarm(cntr_dev, 0);
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
	const struct counter_alarm_cfg cfg = {
		.callback = alarm_cb,
		.ticks = value,
		.flags = COUNTER_ALARM_CFG_ABSOLUTE | COUNTER_ALARM_CFG_EXPIRE_WHEN_LATE,
	};

	LL_ASSERT(cmp == 0);
	(void)counter_cancel_channel_alarm(cntr_dev, 0);
	LL_ASSERT(counter_set_channel_alarm(cntr_dev, 0, &cfg) == 0);
}
