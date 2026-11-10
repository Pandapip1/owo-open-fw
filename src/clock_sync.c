/*
 * The scheduler clock failed a self-check (CONFIG_CLOCK_EVENT_SCHED_SYNC_CHECK,
 * docs/zephyr-dual-clock-scheduler.md "Clock sync"): every timing guarantee is void, so this
 * does not go through the miss policy. The output stage goes to its safe state with raw register
 * writes, which depend on no clock, then the part waits for the watchdog's reset: nothing feeds
 * it again. WDOG0 runs from its own low-frequency oscillator; on the suit that is the LFRCO,
 * which also clocks the BURTC, so a stopped LFRCO stops the reset too and the part then stays
 * here, with the output safe.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/kernel/deadline_miss.h>

extern void owo_suit_force_safe_from_isr(void);

void k_clock_sync_fault_handler(const struct k_deadline_miss *miss)
{
	ARG_UNUSED(miss);
	(void)arch_irq_lock();
	owo_suit_force_safe_from_isr();
	for (;;) {
		/* Wait for the watchdog reset. */
	}
}
