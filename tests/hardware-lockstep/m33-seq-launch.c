/* SPDX-License-Identifier: Apache-2.0 */
/* QEMU fixture: release a preloaded radio M0+ image after machine reset. */
#include <stdint.h>

#define REG(addr) (*(volatile uint32_t *)(addr))

#define RAC_CTRL 0xa8020014U
#define RAC_CTRL_SEQRESET (1U << 24)
#define RAC_CTRL_CPUWAITDIS (1U << 26)

#define INITIAL_SP 0x20008000U
#define FIXED_EXCEPTIONS 16

void reset(void);

static void fault(void)
{
	for (;;) {
	}
}

__attribute__((section(".vectors"), used)) static void (*const vectors[FIXED_EXCEPTIONS])(void) = {
	(void (*)(void))INITIAL_SP,
	reset,
	[2 ... FIXED_EXCEPTIONS - 1] = fault,
};

void reset(void)
{
	__asm__ volatile("cpsid i" ::: "memory");
	REG(RAC_CTRL) = RAC_CTRL_SEQRESET;
	REG(RAC_CTRL) = RAC_CTRL_CPUWAITDIS;
	for (;;) {
		__asm__ volatile("wfi");
	}
}
