/*
 * Runtime regression for the combined BURTC Counter/system-clock owner.
 *
 * The Counter callback is intentionally scheduled before the kernel sleep.
 * Reaching PASS proves that COMP arbitration delivered the Counter callback
 * and subsequently rearmed the same peripheral for the tickless timeout.
 */
#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

static K_SEM_DEFINE(alarm_sem, 0, 1);

static void alarm_callback(const struct device *dev, uint8_t chan_id,
			   uint32_t ticks, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(ticks);
	ARG_UNUSED(user_data);

	if (chan_id == 0U) {
		k_sem_give(&alarm_sem);
	}
}

int main(void)
{
	const struct device *counter = DEVICE_DT_GET(DT_NODELABEL(burtc0));
	struct counter_alarm_cfg alarm = {
		.callback = alarm_callback,
		.flags = COUNTER_ALARM_CFG_ABSOLUTE,
	};
	uint32_t now;

	if (!device_is_ready(counter) || counter_start(counter) != 0 ||
	    counter_get_value(counter, &now) != 0) {
		printk("burtc-arbitration: setup failed\n");
		return 1;
	}

	/* 32 LF cycles leaves margin above the driver's six-cycle LFSYNC floor. */
	alarm.ticks = now + 32U;
	if (counter_set_channel_alarm(counter, 0, &alarm) != 0) {
		printk("burtc-arbitration: alarm arm failed\n");
		return 1;
	}

	/* This must be driven by sys_clock_set_timeout on the same BURTC COMP. */
	k_sleep(K_MSEC(2));
	if (k_sem_take(&alarm_sem, K_NO_WAIT) != 0) {
		printk("burtc-arbitration: callback missing\n");
		return 1;
	}

	printk("burtc-arbitration: PASS\n");
	for (;;) {
		k_sleep(K_SECONDS(1));
	}
}
