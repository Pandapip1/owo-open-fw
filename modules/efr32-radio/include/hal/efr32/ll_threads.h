/*
 * Execution contexts of the lower link layer. SPDX-License-Identifier: Apache-2.0
 *
 * Zephyr's split controller expects three software-interrupt contexts: LLL, and the ULL worker
 * and job (ticker and upper-layer mayflies). Here one persistent Zephyr thread runs them all at
 * a meta-IRQ priority shared with the radio's persistent jobs: like the original equal-priority
 * IRQs they never preempt each other, and like interrupts they preempt every other thread,
 * cooperative ones included. Their software interrupts remain only as doorbells that make the
 * thread runnable, so a pend issued with interrupts locked still takes effect at irq_unlock().
 */
#ifndef EFR32_RADIO_LL_THREADS_H_
#define EFR32_RADIO_LL_THREADS_H_

#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

BUILD_ASSERT(CONFIG_NUM_METAIRQ_PRIORITIES >= 1,
	     "the lower link layer's contexts run as meta-IRQ threads");

/* Priority of every lower-link-layer context and radio job. */
#define EFR32_LL_THREAD_PRIO K_HIGHEST_THREAD_PRIO

/*
 * Deadline-miss policy for the radio's persistent jobs, matching the scheduler
 * clock-event miss policy (kernel/clock_event_sched.c): a job that runs more
 * than CLOCK_EVENT_SCHED_LATE_TOLERANCE_NS after its deadline is a missed
 * deadline; count it and panic in assert/test builds.
 */
static inline void efr32_radio_deadline_check(int deadline, const char *what)
{
	uint32_t now = k_cycle_get_32();
	uint32_t tolerance = DIV_ROUND_UP(
		(uint64_t)CONFIG_CLOCK_EVENT_SCHED_LATE_TOLERANCE_NS *
		CONFIG_CLOCK_EVENT_SCHED_FREQUENCY, NSEC_PER_SEC);

	/* A zero deadline means no real start-by bound was published yet. */
	if (deadline == 0) {
		return;
	}
	/* A start-by deadline still in the future is not late; only a passed one is. */
	if ((int32_t)((uint32_t)deadline - now) >= 0) {
		return;
	}
	if ((uint32_t)(now - (uint32_t)deadline) <= tolerance) {
		return;
	}
	if (IS_ENABLED(CONFIG_CLOCK_EVENT_SCHED_PANIC_ON_LATE)) {
		/* Print synchronously: the report must not depend on deferred logging. */
		LOG_PANIC();
		printk("radio job %s ran %u ns late (tolerance %u ns): deadline %u now %u\n",
		       what,
		       (unsigned)((uint32_t)(now - (uint32_t)deadline) * NSEC_PER_SEC /
				  CONFIG_CLOCK_EVENT_SCHED_FREQUENCY),
		       CONFIG_CLOCK_EVENT_SCHED_LATE_TOLERANCE_NS,
		       (unsigned)deadline, (unsigned)now);
		k_panic();
	}
}

/* Bind the context bodies the LLL used to connect as ISRs (NULL unbinds). */
void efr32_ll_threads_bind(void (*worker)(const void *arg), void (*lll)(const void *arg));

/* Doorbell ISRs connected in place of the context bodies. */
void efr32_ll_worker_doorbell_isr(const void *arg);
void efr32_ll_lll_doorbell_isr(const void *arg);

#endif /* EFR32_RADIO_LL_THREADS_H_ */
