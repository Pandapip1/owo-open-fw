/*
 * Execution contexts of the lower link layer. SPDX-License-Identifier: Apache-2.0
 *
 * Zephyr's split controller expects three software-interrupt contexts: LLL, and the ULL worker
 * and job (ticker and upper-layer mayflies). Here persistent Zephyr threads run them at meta-IRQ
 * priorities derived from the controller's own interrupt priorities, with the radio's persistent
 * jobs at the LLL priority:
 *
 * - CONFIG_BT_CTLR_LLL_PRIO: the LLL context and every radio job (launch, completion, TIFS).
 * - CONFIG_BT_CTLR_ULL_HIGH_PRIO (= CONFIG_BT_CTLR_ULL_LOW_PRIO): the ULL worker context, which
 *   runs the ticker worker, the ticker job and the ULL mayflies, and the QEMU-only RX-descriptor
 *   drain.
 *
 * As with interrupts, a context preempts every lower priority, ordinary threads included,
 * whether cooperative or not. A meta-IRQ thread is cooperative within its own priority
 * (kernel/include/kthread.h, should_preempt()), like interrupts of equal priority, so the LLL
 * context and the radio jobs never nest: each runs to its next wait. Among ready threads of one
 * priority the scheduler picks the earliest start-by deadline at each scheduling point. With the
 * LLL priority above the ULL one, a radio job that falls due preempts a running ULL body; the
 * controller supports that split (Nordic's default), and mayfly_prio_is_equal() reports it.
 *
 * Software interrupts remain only as doorbells that make a thread runnable, so a pend issued with
 * interrupts locked still takes effect at irq_unlock().
 */
#ifndef EFR32_RADIO_LL_THREADS_H_
#define EFR32_RADIO_LL_THREADS_H_

#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

/* Every context is a meta-IRQ thread: it must preempt cooperative threads as an ISR would. */
BUILD_ASSERT(CONFIG_BT_CTLR_ULL_HIGH_PRIO < CONFIG_NUM_METAIRQ_PRIORITIES,
	     "each lower-link-layer context needs a meta-IRQ priority");
/* ULL high and low share the worker software interrupt (hal/efr32/swi.h) and its thread. */
BUILD_ASSERT(CONFIG_BT_CTLR_ULL_HIGH_PRIO == CONFIG_BT_CTLR_ULL_LOW_PRIO,
	     "the ULL worker and job contexts share one thread");
BUILD_ASSERT(CONFIG_BT_CTLR_LLL_PRIO <= CONFIG_BT_CTLR_ULL_HIGH_PRIO,
	     "the LLL never runs below the ULL");

/* Priority of the LLL context and of every radio job. */
#define EFR32_LLL_THREAD_PRIO (K_HIGHEST_THREAD_PRIO + CONFIG_BT_CTLR_LLL_PRIO)
/* Priority of the ULL worker context (ticker worker and job, ULL mayflies). */
#define EFR32_ULL_THREAD_PRIO (K_HIGHEST_THREAD_PRIO + CONFIG_BT_CTLR_ULL_HIGH_PRIO)

/*
 * A job is late when it starts more than this after it fell due: the scheduler clock-event
 * miss tolerance (kernel/clock_event_sched.c), two cycles of the 32768 Hz deep-sleep clock.
 * Every start-by deadline below is the instant the work fell due plus this tolerance, so the
 * deadline itself is the latest acceptable start; the kernel reports a job released from idle
 * that starts later (CONFIG_SCHED_DEADLINE_MISS_POLICY).
 */
#define EFR32_LL_START_TOLERANCE_CYCLES \
	((uint32_t)DIV_ROUND_UP((uint64_t)CONFIG_CLOCK_EVENT_SCHED_LATE_TOLERANCE_NS * \
				CONFIG_CLOCK_EVENT_SCHED_FREQUENCY, NSEC_PER_SEC))

/* Latest start of work that fell due at @p due (k_cycle_get_32() units). */
static inline int efr32_ll_start_by(uint32_t due)
{
	return (int)(due + EFR32_LL_START_TOLERANCE_CYCLES);
}

/* Latest start of work that falls due now. */
static inline int efr32_ll_start_by_now(void)
{
	return efr32_ll_start_by(k_cycle_get_32());
}

/* Bind the context bodies the LLL used to connect as ISRs (NULL unbinds). */
void efr32_ll_threads_bind(void (*worker)(const void *arg), void (*lll)(const void *arg));

/* Doorbell ISRs connected in place of the context bodies. */
void efr32_ll_worker_doorbell_isr(const void *arg);
void efr32_ll_lll_doorbell_isr(const void *arg);

/* Release the worker for ticker work that fell due at @p due (ISR-safe). */
void efr32_ll_worker_release_due(uint32_t due);

/*
 * Stop both contexts for good (ISR-safe): close their gates, mask their doorbells, discard latched
 * runs and suspend their threads. Returns true when the caller is one of them and must still
 * suspend itself.
 */
bool efr32_ll_threads_stop(void);

#endif /* EFR32_RADIO_LL_THREADS_H_ */
