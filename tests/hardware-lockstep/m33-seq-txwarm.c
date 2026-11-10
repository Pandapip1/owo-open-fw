/* SPDX-License-Identifier: Apache-2.0 */
/* QEMU fixture: release the radio M0+, then request a TXWARM interrupt. */
#include <stdint.h>

#define REG(addr) (*(volatile uint32_t *)(addr))

#define CMU_CLKEN1_SET 0x40009068U
#define CMU_RADIOCLKCTRL_SET 0x40009280U
#define CMU_RADIO_BLOCKS 0x0000087fU
#define CMU_RADIOCLKCTRL_EN 0x00000001U
#define RAC_CMD 0xa8020010U
#define RAC_CTRL 0xa8020014U
#define RAC_CTRL_SEQRESET (1U << 24)
#define RAC_CTRL_CPUWAITDIS (1U << 26)
#define RAC_CMD_TXEN (1U << 0)

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
	REG(CMU_RADIOCLKCTRL_SET) = CMU_RADIOCLKCTRL_EN;
	REG(CMU_CLKEN1_SET) = CMU_RADIO_BLOCKS;

	REG(RAC_CTRL) = RAC_CTRL_SEQRESET;
	REG(RAC_CTRL) = RAC_CTRL_CPUWAITDIS;

	/* TXEN enters TXWARM and asserts external IRQ8 on the released M0+. */
	REG(RAC_CMD) = RAC_CMD_TXEN;
	for (;;) {
		__asm__ volatile("wfi");
	}
}
