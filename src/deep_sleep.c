/*
 * Deep sleep (EM2) for the product (CONFIG_OWO_EM2, docs/radio-tables.md 7.95).
 *
 * Without CONFIG_OWO_EM2 EM2 is locked out from boot: the idle thread then only enters EM1. With
 * it, what must not sleep in EM2 says so: the radio while an event is in progress
 * (modules/efr32-radio hal/radio.c), the scheduler clock while a deadline needs TIMER0, the I2C
 * and UART drivers during a transfer, and the application while a stimulation session is armed
 * (owo_deep_sleep_inhibit(), src/lib.rs): EM2 stops TIMER1 (the boost PWM), the USARTs (the DAC
 * and switch SPI) and the IADC's high-frequency clock, and the supervisor must keep its 10 ms
 * cadence (docs/watchdog.md).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdbool.h>
#include <zephyr/init.h>
#include <zephyr/pm/policy.h>

void owo_deep_sleep_inhibit(bool inhibit)
{
	if (inhibit) {
		pm_policy_state_lock_get(PM_STATE_SUSPEND_TO_IDLE, PM_ALL_SUBSTATES);
		pm_policy_state_lock_get(PM_STATE_STANDBY, PM_ALL_SUBSTATES);
	} else {
		pm_policy_state_lock_put(PM_STATE_SUSPEND_TO_IDLE, PM_ALL_SUBSTATES);
		pm_policy_state_lock_put(PM_STATE_STANDBY, PM_ALL_SUBSTATES);
	}
}

#if !defined(CONFIG_OWO_EM2)
static int owo_deep_sleep_off(void)
{
	owo_deep_sleep_inhibit(true);
	return 0;
}

SYS_INIT(owo_deep_sleep_off, PRE_KERNEL_1, 0);
#endif
