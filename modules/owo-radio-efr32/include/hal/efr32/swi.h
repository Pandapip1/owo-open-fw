/*
 * Software interrupts of the lower link layer. SPDX-License-Identifier: Apache-2.0
 * Same contract as subsys/bluetooth/controller/ll_sw/openisa/hal/RV32M1/swi.h.
 */
#include <zephyr/irq.h>
#include "ll_irqs.h"

static inline void hal_swi_init(void)
{
	/* No platform-specific initialization required. */
}

#if defined(CONFIG_BT_LL_SW_SPLIT)
#define HAL_SWI_RADIO_IRQ  LL_SWI_RADIO_IRQn
#define HAL_SWI_WORKER_IRQ LL_RTC0_IRQn
#if !defined(CONFIG_BT_CTLR_LOW_LAT) && \
	(CONFIG_BT_CTLR_ULL_HIGH_PRIO == CONFIG_BT_CTLR_ULL_LOW_PRIO)
#define HAL_SWI_JOB_IRQ    HAL_SWI_WORKER_IRQ
#else
#define HAL_SWI_JOB_IRQ    LL_SWI_JOB_IRQn
#endif

static inline void hal_swi_lll_pend(void)
{
	NVIC_SetPendingIRQ(HAL_SWI_RADIO_IRQ);
}
#else
#error "CTRL architecture not defined"
#endif

static inline void hal_swi_worker_pend(void)
{
	NVIC_SetPendingIRQ(HAL_SWI_WORKER_IRQ);
}

static inline void hal_swi_job_pend(void)
{
	NVIC_SetPendingIRQ(HAL_SWI_JOB_IRQ);
}
