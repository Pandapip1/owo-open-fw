/* SPDX-License-Identifier: Apache-2.0
 * Build: arm-none-eabi-gcc -nostdlib -Wl,-Ttext=0 -o j.elf jump16000.S && arm-none-eabi-objcopy -O binary j.elf j.bin;
 * then sim/scripts/mkflash.py out.bin j.bin app.bin@0x16000 (docs/crypto.md 6). */
/* Test-only stand-in for the Gecko Bootloader in QEMU: start the application at 0x16000
 * (VTOR, MSP, reset vector from its vector table). */
	.syntax unified
	.cpu cortex-m33
	.thumb
	.section .text
	.word 0x20008000
	.word reset + 1
	.thumb_func
reset:
	ldr r0, =0x16000
	ldr r1, =0xE000ED08
	str r0, [r1]
	ldr r1, [r0]
	msr msp, r1
	ldr r1, [r0, #4]
	bx r1
	.ltorg
