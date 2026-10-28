/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Isolation probe: does ONLY the vector-table copy + VTOR redirect from
 * probe-irq-handler.S, with NO PROTIMER touches and NO NVIC changes at
 * all. Checks whether the abort-to-OFF result seen at two different
 * copy destinations (0x20000900, then 0x20007d00) is caused by the
 * VTOR mechanism itself rather than by corrupting whatever memory
 * happens to sit at the destination -- see docs/radio-tables.md SS7.36.
 */
.syntax unified
.cpu cortex-m33
.thumb

.equ VTOR,               0xe000ed08
.equ OLD_VECTOR_TABLE,   0x00000000
.equ NEW_VECTOR_TABLE,   0x20007d00
.equ VECTOR_WORDS,       64

.equ SCRATCH_MARKER,     0x200007f0
.equ SCRATCH_DONE,       0x51a7c0de

.section .text, "ax", %progbits
.global protimer_probe_vtor_only
.thumb_func
protimer_probe_vtor_only:
	cpsid	i

	ldr	r0, =OLD_VECTOR_TABLE
	ldr	r1, =NEW_VECTOR_TABLE
	movs	r2, #VECTOR_WORDS
copy_loop:
	ldr	r3, [r0]
	str	r3, [r1]
	adds	r0, r0, #4
	adds	r1, r1, #4
	subs	r2, r2, #1
	bne	copy_loop

	ldr	r0, =VTOR
	ldr	r1, =NEW_VECTOR_TABLE
	str	r1, [r0]
	dsb
	isb

	ldr	r0, =SCRATCH_MARKER
	ldr	r1, =SCRATCH_DONE
	str	r1, [r0]
	bkpt	#0
