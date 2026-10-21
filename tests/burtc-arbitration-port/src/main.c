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
#include <zephyr/kernel/deferred_release.h>
#include <zephyr/sys/printk.h>

static K_SEM_DEFINE(alarm_sem, 0, 1);
static K_SEM_DEFINE(first_event_sem, 0, 4);
static K_SEM_DEFINE(second_event_sem, 0, 4);
static uint8_t first_count;
static uint8_t second_count;
static volatile uint32_t alarm_fired_at;
K_CLOCK_EVENT_DEFINE(clock_event);
K_CLOCK_EVENT_DEFINE(second_clock_event);

static void alarm_callback(const struct device *dev, uint8_t chan_id,
			   uint32_t ticks, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	if (chan_id == 0U) {
		alarm_fired_at = ticks;
		k_sem_give(&alarm_sem);
	}
}

static void first_clock_event_callback(struct k_clock_event *event, uint32_t generation,
				       void *user_data)
{
	ARG_UNUSED(event);
	ARG_UNUSED(generation);
	ARG_UNUSED(user_data);
	first_count++;
	k_sem_give(&first_event_sem);
	if (first_count < 3U) {
		(void)k_clock_event_arm_abs(event, k_clock_event_now(event) + 16U);
	}
}

static void second_clock_event_callback(struct k_clock_event *event, uint32_t generation,
					void *user_data)
{
	ARG_UNUSED(event);
	ARG_UNUSED(generation);
	ARG_UNUSED(user_data);
	second_count++;
	k_sem_give(&second_event_sem);
	if (second_count < 3U) {
		(void)k_clock_event_arm_abs(event, k_clock_event_now(event) + 16U);
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
	if (counter_silabs_burtc_clock_event_init(counter, &clock_event,
					   first_clock_event_callback, NULL) != 0 ||
	    counter_silabs_burtc_clock_event_init(counter, &second_clock_event,
					   second_clock_event_callback, NULL) != 0) {
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
	alarm.ticks = now + 128U;
	if (counter_set_channel_alarm(counter, 0, &alarm) != 0) {
		printk("burtc-arbitration: alarm arm failed\n");
		return 1;
	}
	/* Independent events must coexist and fire before the later alarm/release. */
	if (k_clock_event_arm_abs(&clock_event, now + 64U) != 0 ||
	    k_clock_event_arm_abs(&second_clock_event, now + 72U) != 0 ||
	    k_clock_event_arm_abs(&second_clock_event, now + 88U) != 0) {
		printk("burtc-arbitration: two-event arm failed\n");
		return 1;
	}

	/* The two generic deadlines precede the Counter alarm on the same COMP. */
	for (uint8_t i = 0; i < 3U; i++) {
		if (k_sem_take(&first_event_sem, K_MSEC(10)) != 0 ||
		    k_sem_take(&second_event_sem, K_MSEC(10)) != 0) {
			printk("burtc-arbitration: cadence callback missing\n");
			return 1;
		}
	}
	if (k_sem_take(&alarm_sem, K_MSEC(10)) != 0) {
		printk("burtc-arbitration: alarm callback missing\n");
		return 1;
	}
	if (first_count != 3U || second_count != 3U ||
	    k_sem_take(&second_event_sem, K_NO_WAIT) == 0) {
		printk("burtc-arbitration: second event duplicated\n");
		return 1;
	}

	/*
	 * Let a clock event's COMP match latch while IRQs are locked, then cancel
	 * it. The pending COMP flag must not deliver the later Counter alarm.
	 */
	if (counter_get_value(counter, &now) != 0) {
		printk("burtc-arbitration: stale-match time read failed\n");
		return 1;
	}
	alarm.ticks = now + 64U;
	if (counter_set_channel_alarm(counter, 0, &alarm) != 0 ||
	    k_clock_event_arm_abs(&clock_event, now + 16U) != 0) {
		printk("burtc-arbitration: stale-match arm failed\n");
		return 1;
	}
	{
		unsigned int key = irq_lock();
		uint32_t cnt;

		do {
			(void)counter_get_value(counter, &cnt);
		} while ((int32_t)(cnt - (now + 18U)) < 0);
		k_clock_event_cancel(&clock_event);
		irq_unlock(key);
	}
	if (k_sem_take(&alarm_sem, K_MSEC(10)) != 0) {
		printk("burtc-arbitration: stale-match alarm missing\n");
		return 1;
	}
	if ((int32_t)(alarm_fired_at - alarm.ticks) < 0 || first_count != 3U) {
		printk("burtc-arbitration: stale COMP match delivered alarm early\n");
		return 1;
	}

	printk("burtc-arbitration: PASS\n");
	for (;;) {
		k_sleep(K_SECONDS(1));
	}
}
