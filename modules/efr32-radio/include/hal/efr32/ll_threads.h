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

#include <zephyr/kernel.h>

BUILD_ASSERT(CONFIG_NUM_METAIRQ_PRIORITIES >= 1,
	     "the lower link layer's contexts run as meta-IRQ threads");

/* Priority of every lower-link-layer context and radio job. */
#define EFR32_LL_THREAD_PRIO K_HIGHEST_THREAD_PRIO

/* Bind the context bodies the LLL used to connect as ISRs (NULL unbinds). */
void efr32_ll_threads_bind(void (*worker)(const void *arg), void (*lll)(const void *arg));

/* Doorbell ISRs connected in place of the context bodies. */
void efr32_ll_worker_doorbell_isr(const void *arg);
void efr32_ll_lll_doorbell_isr(const void *arg);

#endif /* EFR32_RADIO_LL_THREADS_H_ */
