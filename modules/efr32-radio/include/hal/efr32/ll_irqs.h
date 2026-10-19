/*
 * Interrupt assignment of the lower link layer on the EFR32BG22. SPDX-License-Identifier: Apache-2.0
 *
 * The three software interrupts are the device's SW0..SW3 lines (efr32bg22c224f512im40.h,
 * IRQn_Type), meant to be pended from software. The worker (ticker) interrupt is the compare
 * interrupt of the 32.768 kHz tick counter (hal/cntr.c, BURTC: RTCC is the Silicon Labs sleeptimer
 * of the system clock, dts/arm/silabs/xg22/xg22.dtsi `silabs,sleeptimer = &rtcc0`).
 */
#include <soc.h>

#define LL_SWI_RADIO_IRQn  SW0_IRQn
#define LL_SWI_JOB_IRQn    SW1_IRQn
#define LL_RTC0_IRQn       SW2_IRQn      /* pended by the counter alarm callback (hal/cntr.c) */
/*
 * FRC owns the TXDONE/RXDONE latches used by radio_is_done().  RAC_SEQ is a
 * separate sequencer route and is not enabled by this port for completion.
 */
#define LL_RADIO_IRQn      FRC_IRQn
