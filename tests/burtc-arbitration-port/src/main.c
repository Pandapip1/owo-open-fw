/*
 * Runtime regression for the combined BURTC COMP mux.
 *
 * The Counter callback is intentionally scheduled before the kernel sleep.
 * Reaching PASS proves that COMP arbitration delivered the Counter and generic
 * clock-event callbacks while preserving the tickless system-clock deadline.
 */
#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/drivers/counter/silabs_burtc.h>
#include <zephyr/kernel.h>
#include <zephyr/kernel/clock_event.h>
#include <zephyr/sys/printk.h>

static K_SEM_DEFINE(alarm_sem, 0, 1);
static K_SEM_DEFINE(clock_event_sem, 0, 1);
K_CLOCK_EVENT_DEFINE(clock_event);

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

static void clock_event_callback(struct k_clock_event *event, void *user_data)
{
	ARG_UNUSED(event);
	ARG_UNUSED(user_data);
	k_sem_give(&clock_event_sem);
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
	if (counter_silabs_burtc_clock_event_init(counter, &clock_event,
					   clock_event_callback, NULL) != 0) {
		printk("burtc-arbitration: clock-event setup failed\n");
		return 1;
	}
	if (k_clock_event_min_arm_cycles(&clock_event) != 6U ||
	    counter_get_value(counter, &now) != 0 ||
	    k_clock_event_arm_abs(&clock_event, now + 5U) != -ETIME) {
		printk("burtc-arbitration: clock-event min lead invalid\n");
		return 1;
	}
	if (counter_get_value(counter, &now) != 0) {
		printk("burtc-arbitration: clock-event time read failed\n");
		return 1;
	}

	/* 32 LF cycles leaves margin above the driver's six-cycle LFSYNC floor. */
	alarm.ticks = now + 32U;
	if (counter_set_channel_alarm(counter, 0, &alarm) != 0) {
		printk("burtc-arbitration: alarm arm failed\n");
		return 1;
	}
	if (k_clock_event_arm_abs(&clock_event, now + 48U) != 0) {
		printk("burtc-arbitration: clock-event arm failed\n");
		return 1;
	}

	/* This must be driven by sys_clock_set_timeout on the same BURTC COMP. */
	k_sleep(K_MSEC(2));
	if (k_sem_take(&alarm_sem, K_NO_WAIT) != 0) {
		printk("burtc-arbitration: callback missing\n");
		return 1;
	}
	if (k_sem_take(&clock_event_sem, K_NO_WAIT) != 0) {
		printk("burtc-arbitration: clock-event callback missing\n");
		return 1;
	}

	printk("burtc-arbitration: PASS\n");
	for (;;) {
		k_sleep(K_SECONDS(1));
	}
}
