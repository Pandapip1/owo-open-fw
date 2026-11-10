/* SPDX-License-Identifier: Apache-2.0
 * Build: arm-none-eabi-gcc -mcpu=cortex-m33 -mthumb -Os -nostdlib -Wl,-T,jump16000.ld
 * -o j.elf jump16000.c && arm-none-eabi-objcopy -O binary j.elf j.bin;
 * then sim/scripts/mkflash.py out.bin j.bin app.bin@0x16000 (docs/crypto.md 6). */
/* Test-only stand-in for the Gecko Bootloader in QEMU: start the application at 0x16000
 * (VTOR, MSP, reset vector from its vector table). */
#include <stdint.h>

#define APP_BASE 0x16000U
#define VTOR 0xE000ED08U
#define INITIAL_SP 0x20008000U

void reset(void);

__attribute__((section(".vectors"), used)) static void (*const vectors[2])(void) = {
	(void (*)(void))INITIAL_SP,
	reset,
};

void reset(void)
{
	const volatile uint32_t *app = (const volatile uint32_t *)APP_BASE;

	*(volatile uint32_t *)VTOR = APP_BASE;
	/* Stays asm: once MSP moves to the application's stack, no compiled code may run. */
	__asm__ volatile("msr msp, %0\n\tbx %1" ::"r"(app[0]), "r"(app[1]) : "memory");
	__builtin_unreachable();
}
