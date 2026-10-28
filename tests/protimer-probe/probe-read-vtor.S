/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Tiny diagnostic: read VTOR from the CPU's own (Non-secure) perspective
 * and report it via the usual marker mechanism, rather than trusting a
 * debugger-side read of 0xe000ed08 (which may resolve to the Secure-banked
 * copy instead of the Non-secure one this firmware actually runs under).
 * Standalone, no Zephyr. See tests/protimer-probe/README.md.
 */
.syntax unified
.cpu cortex-m33
.thumb

.equ VTOR, 0xe000ed08
.equ SCRATCH_MARKER, 0x200007f0

.section .text, "ax", %progbits
.global protimer_probe_read_vtor
.thumb_func
protimer_probe_read_vtor:
	ldr	r0, =VTOR
	ldr	r1, [r0]
	ldr	r0, =SCRATCH_MARKER
	str	r1, [r0]
	bkpt	#0
