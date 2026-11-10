/*
 * The scheduler clock's self-checks against injected clock faults (run-clock-sync-qemu.sh).
 *
 * Phase "awake": EM2 locked, TIMER0 runs throughout, so the drift check judges every
 * interval. Phase "sleeping": EM2 allowed, so every sleep anchors the providers at a BURTC
 * edge (the capture). Each phase starts on the runner's signal, the suit board model's
 * expander /INT on PC4 (qom-set expander-ext-low), low for "awake", high again for
 * "sleeping", taken on the pin's interrupt: a timed wait would depend on the clocks under
 * test. The runner places each fault at a point fixed in the guest (inject.py), never by how
 * fast the model runs. The self-check's report, made through k_clock_sync_fault_handler(), is
 * printed here instead of shutting down.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel/deadline_miss.h>
#include <zephyr/pm/policy.h>

#define GO_PIN 4

static volatile int faults;
static const struct device *const go_port = DEVICE_DT_GET(DT_NODELABEL(gpioc));

void k_clock_sync_fault_handler(const struct k_deadline_miss *miss)
{
	/* Once: a failed clock keeps failing every check after it. */
	if (faults++ == 0) {
		printk("clock sync fault: reason %u expected %u measured %u\n", miss->demand,
		       miss->release, miss->now);
	}
}

static K_SEM_DEFINE(go_sem, 0, 1);
static struct gpio_callback go_cb;

static void go_changed(const struct device *port, struct gpio_callback *cb, uint32_t pins)
{
	k_sem_give(&go_sem);
}

/* Wait, awake and on no clock (a fault may have stopped one), for the runner's signal. */
static void wait_for_go(int level)
{
	while (gpio_pin_get_raw(go_port, GO_PIN) != level) {
		(void)k_sem_take(&go_sem, K_FOREVER);
	}
}

static void run_phase(void)
{
	for (int i = 0; i < 300; i++) {
		k_msleep(10);
	}
}

int main(void)
{
	(void)gpio_pin_configure(go_port, GO_PIN, GPIO_INPUT);
	gpio_init_callback(&go_cb, go_changed, BIT(GO_PIN));
	(void)gpio_add_callback(go_port, &go_cb);
	(void)gpio_pin_interrupt_configure(go_port, GO_PIN, GPIO_INT_EDGE_BOTH);
	pm_policy_state_lock_get(PM_STATE_SUSPEND_TO_IDLE, PM_ALL_SUBSTATES);
	pm_policy_state_lock_get(PM_STATE_STANDBY, PM_ALL_SUBSTATES);

	printk("clock sync: phase awake\n");
	wait_for_go(0);
	run_phase();
	printk("clock sync: phase sleeping, faults %d\n", faults);
	wait_for_go(1);
	pm_policy_state_lock_put(PM_STATE_SUSPEND_TO_IDLE, PM_ALL_SUBSTATES);
	pm_policy_state_lock_put(PM_STATE_STANDBY, PM_ALL_SUBSTATES);
	run_phase();
	printk("clock sync: done, faults %d\n", faults);
	return 0;
}
