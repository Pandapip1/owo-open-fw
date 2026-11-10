/*
 * The scheduler clock's self-checks against injected clock faults (run-clock-sync-qemu.sh).
 *
 * Phase "awake": EM2 locked, TIMER0 runs throughout, so the drift check judges every
 * interval. Phase "sleeping": EM2 allowed, so every sleep anchors the providers at a BURTC
 * edge (the capture). The runner injects a fault in one phase and expects the self-check's
 * report, made through k_clock_sync_fault_handler(), which here prints it instead of shutting
 * down.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/kernel/deadline_miss.h>
#include <zephyr/pm/policy.h>

static volatile int faults;

void k_clock_sync_fault_handler(const struct k_deadline_miss *miss)
{
	/* Once: a failed clock keeps failing every check after it. */
	if (faults++ == 0) {
		printk("clock sync fault: reason %u expected %u measured %u\n", miss->demand,
		       miss->release, miss->now);
	}
}

int main(void)
{
	printk("clock sync: phase awake\n");
	pm_policy_state_lock_get(PM_STATE_SUSPEND_TO_IDLE, PM_ALL_SUBSTATES);
	pm_policy_state_lock_get(PM_STATE_STANDBY, PM_ALL_SUBSTATES);
	for (int i = 0; i < 300; i++) {
		k_msleep(10);
	}
	printk("clock sync: phase sleeping, faults %d\n", faults);
	pm_policy_state_lock_put(PM_STATE_SUSPEND_TO_IDLE, PM_ALL_SUBSTATES);
	pm_policy_state_lock_put(PM_STATE_STANDBY, PM_ALL_SUBSTATES);
	for (int i = 0; i < 300; i++) {
		k_msleep(10);
	}
	printk("clock sync: done, faults %d\n", faults);
	return 0;
}
