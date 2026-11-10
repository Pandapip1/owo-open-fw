/*
 * The M0+ side of the decision harness: the RDMAILBOX1 (IRQ 23) vector of the installed image
 * is pointed here, so a mailbox flag from the M33 runs the image's own efr32_seq_conn_decide()
 * once on the stage the M33 wrote, timed on the sequencer's SysTick.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdint.h>
#include "hal/efr32/seq_abi.h"
#include "harness.h"

#define REG32(a) (*(volatile uint32_t *)(a))
#define HARNESS(o) REG32(HARNESS_BLOCK + (o))
#define SYSTICK_RVR 0xe000e014U
#define SYSTICK_CVR 0xe000e018U
/* Writes through this offset clear bits (seq_regs.h EFR32_ALIAS_CLR). */
#define ALIAS_CLR 0x2000U

__attribute__((section(".entry"), used)) void harness_irq(void)
{
	uint32_t (*decide)(void) = (uint32_t (*)(void))HARNESS(HARNESS_DECIDE);

	REG32(EFR32_SEQ_RDMAILBOX1_BASE + EFR32_SEQ_RDMAILBOX_FLAGS + ALIAS_CLR) =
		EFR32_SEQ_RDMAILBOX_TIMED_FLAG;
	uint32_t before = REG32(SYSTICK_CVR);
	uint32_t ret = decide();
	uint32_t after = REG32(SYSTICK_CVR);

	HARNESS(HARNESS_RET) = ret;
	HARNESS(HARNESS_BEFORE) = before;
	HARNESS(HARNESS_AFTER) = after;
	HARNESS(HARNESS_RELOAD) = REG32(SYSTICK_RVR);
	HARNESS(HARNESS_DONE) = HARNESS(HARNESS_DONE) + 1U;
}
