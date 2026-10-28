/* SPDX-License-Identifier: Apache-2.0 */
/*
 * EFR32BG22 PROTIMER IEN sweep probe.
 *
 * Follow-up to probe.S (see that file's header and
 * docs/radio-tables.md SS7.33 for the base enable sequence and its
 * clean-negative result). PROTIMER.IEN (+0x070) reads 0 at baseline --
 * this probe additionally writes it all-ones, to check whether PROTIMER
 * ever latches an NVIC-pending IRQ36 request at all once *something* in
 * its interrupt-enable mask is set, before investing in the much larger
 * "install a real handler via a VTOR-redirected SRAM vector table" probe
 * that full IRQ-servicing testing would need.
 *
 * All-ones is a blanket sweep, not a field-decoded value: PROTIMER's
 * IF/IEN bit meanings are explicitly unresolved (docs/radio-tables.md
 * SS7.9), so this does not claim to understand or correctly configure
 * any specific named interrupt source -- it only answers "does anything
 * latch pending at all", which is checked via NVIC's pending register
 * (safe to read regardless of enable state) rather than by actually
 * unmasking the interrupt.
 *
 * Standalone, no Zephyr, no vector table -- same SRAM-injection usage as
 * probe.S (see tests/protimer-probe/README.md).
 */

.syntax unified
.cpu cortex-m33
.thumb

.equ PROTIMER_BASE,      0xb801c000  /* non-secure alias -- see probe.S */
.equ PROTIMER_EN,        PROTIMER_BASE + 0x004
.equ PROTIMER_CTRL,      PROTIMER_BASE + 0x008
.equ PROTIMER_CTRL_VAL,  0x00011102
.equ PROTIMER_CMD,       PROTIMER_BASE + 0x00c
.equ PROTIMER_CMD_START, 0x00000001
.equ PROTIMER_REG_030,   PROTIMER_BASE + 0x030
.equ PROTIMER_REG_030_VAL, 0x00001233
.equ PROTIMER_REG_038,   PROTIMER_BASE + 0x038
.equ PROTIMER_REG_038_VAL, 0x80015558
.equ PROTIMER_IEN,       PROTIMER_BASE + 0x070  /* unresolved bit meanings -- see file header */

.equ SCRATCH_MARKER,     0x200007f0
.equ SCRATCH_DONE,       0x51a7c0de

.section .text, "ax", %progbits
.global protimer_probe_ien_sweep
.thumb_func
protimer_probe_ien_sweep:
	cpsid i

	ldr	r0, =PROTIMER_EN
	movs	r1, #1
	str	r1, [r0]

	ldr	r0, =PROTIMER_CTRL
	ldr	r1, =PROTIMER_CTRL_VAL
	str	r1, [r0]

	ldr	r0, =PROTIMER_REG_030
	ldr	r1, =PROTIMER_REG_030_VAL
	str	r1, [r0]

	ldr	r0, =PROTIMER_REG_038
	ldr	r1, =PROTIMER_REG_038_VAL
	str	r1, [r0]

	ldr	r0, =PROTIMER_IEN
	movs	r1, #0
	mvns	r1, r1          /* r1 = 0xFFFFFFFF */
	str	r1, [r0]

	ldr	r0, =PROTIMER_CMD
	ldr	r1, =PROTIMER_CMD_START
	str	r1, [r0]

	/* Deliberately no NVIC enable -- see file header. */

	ldr	r0, =SCRATCH_MARKER
	ldr	r1, =SCRATCH_DONE
	str	r1, [r0]
	bkpt	#0
