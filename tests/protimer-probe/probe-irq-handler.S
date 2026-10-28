/* SPDX-License-Identifier: Apache-2.0 */
/*
 * EFR32BG22 PROTIMER real-IRQ-servicing probe.
 *
 * Follow-up to probe.S/probe-ien-sweep.S (see docs/radio-tables.md
 * SS7.32-7.34). Those ruled out "PROTIMER just needs to be clocked" and
 * "PROTIMER just needs to be pending" -- this tests the remaining
 * question: does FRC need the CPU to actually take the IRQ36 exception
 * and run real handler logic, not just have the condition latched?
 *
 * Zephyr's vector table here is flash-resident (VTOR_NS reads 0, the
 * linked address of `_vector_table`) and this build has no
 * CONFIG_DYNAMIC_INTERRUPTS (`_sw_isr_table` is also flash/const), so
 * there is no simple runtime patch target. This probe instead copies the
 * live 64-entry (256-byte) vector table into SRAM, overwrites only the
 * IRQ36 entry (index 16+36=52) to point at a trivial handler also placed
 * in SRAM, and repoints VTOR_NS at the copy -- so every other exception
 * (SysTick, UART, etc.) keeps using the exact same handlers as before,
 * unaffected.
 *
 * RAM CAVEAT, recorded honestly rather than silently assumed safe: this
 * firmware links at ~99.68% SRAM utilization (ninja's own build output).
 * There is no genuinely free scratch region. The addresses used here
 * (0x200007d0 for this probe's own code, matching every other probe in
 * this project; 0x20000900 for the copied table) fall inside the live
 * `bt_workq`/`acl_conns`/`db_hash`/`gatt_sc` globals (confirmed via
 * `nm -S`). This is the same region
 * bg22_require_sram_execution's own fixture and every prior probe in
 * this project have used throughout this entire investigation, and
 * every one of those tests reproduced the exact expected, stable
 * baseline afterward -- consistent with (not proof of) this specific
 * connectionless single-advertiser smoke test never touching its own
 * work-queue/ACL-connection state in the narrow pre-TXEN window being
 * probed, but it is a real, not fully eliminated risk, not a verified
 * absence of side effects. Do not extend this technique to a test where
 * Bluetooth connection/work-queue behavior is itself part of what is
 * being measured.
 */
.syntax unified
.cpu cortex-m33
.thumb

.equ VTOR,               0xe000ed08
.equ NVIC_ISER1,         0xe000e104
.equ NVIC_IRQ36_BIT,     (1 << 4)      /* IRQ36 - 32 = bit 4 of ISER1 */

.equ OLD_VECTOR_TABLE,   0x00000000    /* VTOR_NS, confirmed via probe-read-vtor.S */
.equ NEW_VECTOR_TABLE,   0x20000900    /* 256-byte, 0x100-aligned -- moved clear of this probe's own code (0x200007d0-0x884, confirmed via `size`), which the copy loop would otherwise overwrite mid-execution, corrupting trivial_irq36_handler before it's ever used */
.equ VECTOR_WORDS,       64
.equ IRQ36_VECTOR_INDEX, 52           /* 16 fixed exceptions + IRQ36 */

.equ PROTIMER_BASE,      0xb801c000
.equ PROTIMER_EN,        PROTIMER_BASE + 0x004
.equ PROTIMER_CTRL,      PROTIMER_BASE + 0x008
.equ PROTIMER_CTRL_VAL,  0x00011102
.equ PROTIMER_CMD,       PROTIMER_BASE + 0x00c
.equ PROTIMER_CMD_START, 0x00000001
.equ PROTIMER_REG_030,   PROTIMER_BASE + 0x030
.equ PROTIMER_REG_030_VAL, 0x00001233
.equ PROTIMER_REG_038,   PROTIMER_BASE + 0x038
.equ PROTIMER_REG_038_VAL, 0x80015558
.equ PROTIMER_IEN,       PROTIMER_BASE + 0x070
.equ PROTIMER_IF_CLR,    PROTIMER_BASE + 0x2064

.equ SCRATCH_MARKER,     0x200007f0
.equ SCRATCH_DONE,       0x51a7c0de

.section .text, "ax", %progbits
.global protimer_probe_irq_handler
.thumb_func
protimer_probe_irq_handler:
	cpsid	i

	/* Copy the live 64-word vector table into SRAM. */
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

	/* Patch index 52 (IRQ36) to point at the trivial handler below. */
	ldr	r0, =NEW_VECTOR_TABLE
	movs	r1, #IRQ36_VECTOR_INDEX
	lsls	r1, r1, #2
	adds	r0, r0, r1
	ldr	r1, =trivial_irq36_handler  /* real runtime address: this probe is linked at its own injection address, see link-loaded-at-0x200007d0.ld */
	str	r1, [r0]

	/* Repoint VTOR_NS at the SRAM copy. */
	ldr	r0, =VTOR
	ldr	r1, =NEW_VECTOR_TABLE
	str	r1, [r0]
	dsb
	isb

	/* PROTIMER enable sequence (same values as probe.S/probe-ien-sweep.S). */
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
	mvns	r1, r1               /* 0xFFFFFFFF, same blanket sweep as probe-ien-sweep.S */
	str	r1, [r0]
	ldr	r0, =PROTIMER_CMD
	ldr	r1, =PROTIMER_CMD_START
	str	r1, [r0]

	/* Now safe to unmask IRQ36: a real handler is installed above. */
	ldr	r0, =NVIC_ISER1
	ldr	r1, =NVIC_IRQ36_BIT
	str	r1, [r0]

	ldr	r0, =SCRATCH_MARKER
	ldr	r1, =SCRATCH_DONE
	str	r1, [r0]
	bkpt	#0

.align 2
.thumb_func
trivial_irq36_handler:
	/* Ack everything pending (blanket, same rationale as the CLR-alias
	 * test in SS7.34 -- bit meanings are unresolved) and return. */
	ldr	r0, =PROTIMER_IF_CLR
	movs	r1, #0
	mvns	r1, r1
	str	r1, [r0]
	bx	lr
