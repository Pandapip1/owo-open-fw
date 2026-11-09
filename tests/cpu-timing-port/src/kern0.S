/*
 * Cortex-M0+ (radio sequencer core) timing kernels (docs/radio-tables.md 7.75, 7.93, 7.94).
 * A self-contained image for SEQRAM: the M33 copies it to the M0+'s address 0 (0xB0000000 on
 * the M33), holds the core in RAC.CTRL.SEQRESET for the copy and releases it with CPUWAITDIS.
 * Every address the image uses is relative to kern0_start, so it runs at 0.
 *
 * Kernels: r0 = iterations N (> 0), r1 = target; out r0 = SysTick cycles of the loop (the
 * counter counts down at the core clock, 24 bits). Each is run at N = K0_N_LO and K0_N_HI,
 * best of K0_REPS; the per-iteration cost is the difference. The "aN" variants put the loop's
 * first instruction at byte N mod 8 (the 7.93 fetch-alignment question); the padding runs
 * once, outside the loop. Then IRQ 23 (the timed-command mailbox's line, nothing else drives
 * it while RDMAILBOX1.IEN is clear) is pended through NVIC.ISPR with its enable set and, as
 * the control, clear: SysTick before the pending store, at the handler's first and last
 * instruction and after the return.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
.syntax unified
.thumb
.cpu cortex-m0plus

.equ SYST_CSR, 0xE000E010
.equ SYST_RVR, 0xE000E014
.equ SYST_CVR, 0xE000E018
/* CSR: ENABLE, CLKSOURCE = processor clock, no interrupt. */
.equ SYST_CSR_RUN, 5
.equ SYST_MAX, 0x00FFFFFF
.equ NVIC_ISER, 0xE000E100
.equ NVIC_ICER, 0xE000E180
.equ NVIC_ISPR, 0xE000E200
.equ NVIC_ICPR, 0xE000E280
.equ K0_IRQ, 23

/* M0+ addresses, shared with main.c. The sequencer's own stack top (radio_seq.S). */
.equ K0_STACK_TOP, 0x00003e00
.equ K0_RESULTS, 0x00003000
.equ K0_TARGET, 0x00003800
.equ K0_DONE_MAGIC, 0x4E44304B
.equ K0_N_LO, 10
.equ K0_N_HI, 110
.equ K0_REPS, 3
.equ K0_XREPS, 4
/*
 * Results: word 0 done, word 1 kernel count, then lo/hi per kernel; at K0_RES_X the exception
 * runs (2 x K0_XREPS, odd ones enabled, four stamps each); K0_STAMP the handler's two words.
 */
.equ K0_RES_DONE, 0
.equ K0_RES_COUNT, 4
.equ K0_RES_KERN, 8
.equ K0_RES_X, 0x200
.equ K0_STAMP, 0x00003700

.section .text.kern0, "ax", %progbits
.align 8
.global kern0_start
kern0_start:
	.word K0_STACK_TOP
	.word k0_reset - kern0_start + 1
	.rept 14
	.word k0_fault - kern0_start + 1
	.endr
	.rept K0_IRQ
	.word k0_fault - kern0_start + 1
	.endr
	.word k0_isr - kern0_start + 1
	.rept (32 - K0_IRQ - 1)
	.word k0_fault - kern0_start + 1
	.endr

k0_fault:
	b k0_fault

/* Stamps of one exception run: before the pend, handler entry, handler exit, after return. */
k0_isr:
	ldr r1, =SYST_CVR
	ldr r0, [r1]
	ldr r3, [r1]
	ldr r2, =K0_STAMP
	str r0, [r2, #4]
	str r3, [r2, #8]
	bx lr
.ltorg

.macro K0 name, align, op1, op2
.align 3
\name:
	push {r4-r7, lr}
	ldr r3, =SYST_CVR
	ldr r2, [r3]
	b 2f
.ltorg
.align 3
2:
	.rept (\align / 2)
	nop
	.endr
1:
	.rept 8
	\op1
	\op2
	.endr
	subs r0, #1
	bne 1b
	ldr r0, [r3]
	subs r0, r2, r0
	lsls r0, r0, #8
	lsrs r0, r0, #8
	pop {r4-r7, pc}
.endm

/* radio_seq.S's efr32_seq_delay pass (ldr, subs, str, taken bpl on a stack word), N passes. */
.macro K0DELAY name, align
.align 3
\name:
	push {r4-r7, lr}
	sub sp, #4
	movs r4, #30
	subs r0, #1
	muls r4, r0, r4
	str r4, [sp]
	ldr r3, =SYST_CVR
	ldr r2, [r3]
	b 2f
.ltorg
.align 3
2:
	.rept (\align / 2)
	nop
	.endr
1:
	ldr r0, [sp]
	subs r0, #30
	str r0, [sp]
	bpl 1b
	ldr r0, [r3]
	subs r0, r2, r0
	lsls r0, r0, #8
	lsrs r0, r0, #8
	add sp, #4
	pop {r4-r7, pc}
.endm

K0 k0_empty, 0, "", ""
K0 k0_nop, 0, "nop", ""
K0 k0_alu, 0, "adds r4, #1", ""
K0 k0_str_a0, 0, "str r4, [r1]", ""
K0 k0_str_a2, 2, "str r4, [r1]", ""
K0 k0_str_a4, 4, "str r4, [r1]", ""
K0 k0_str_a6, 6, "str r4, [r1]", ""
K0 k0_ldr_a0, 0, "ldr r4, [r1]", ""
K0 k0_ldr_a2, 2, "ldr r4, [r1]", ""
K0 k0_ldr_a4, 4, "ldr r4, [r1]", ""
K0 k0_ldr_a6, 6, "ldr r4, [r1]", ""
K0 k0_stralu_a0, 0, "str r4, [r1]", "adds r5, #1"
K0 k0_stralu_a2, 2, "str r4, [r1]", "adds r5, #1"
K0 k0_stralu_a4, 4, "str r4, [r1]", "adds r5, #1"
K0 k0_stralu_a6, 6, "str r4, [r1]", "adds r5, #1"
K0DELAY k0_delay_a0, 0
K0DELAY k0_delay_a2, 2
K0DELAY k0_delay_a4, 4
K0DELAY k0_delay_a6, 6

.align 2
k0_table:
	.word k0_empty - kern0_start + 1
	.word k0_nop - kern0_start + 1
	.word k0_alu - kern0_start + 1
	.word k0_str_a0 - kern0_start + 1
	.word k0_str_a2 - kern0_start + 1
	.word k0_str_a4 - kern0_start + 1
	.word k0_str_a6 - kern0_start + 1
	.word k0_ldr_a0 - kern0_start + 1
	.word k0_ldr_a2 - kern0_start + 1
	.word k0_ldr_a4 - kern0_start + 1
	.word k0_ldr_a6 - kern0_start + 1
	.word k0_stralu_a0 - kern0_start + 1
	.word k0_stralu_a2 - kern0_start + 1
	.word k0_stralu_a4 - kern0_start + 1
	.word k0_stralu_a6 - kern0_start + 1
	.word k0_delay_a0 - kern0_start + 1
	.word k0_delay_a2 - kern0_start + 1
	.word k0_delay_a4 - kern0_start + 1
	.word k0_delay_a6 - kern0_start + 1
k0_table_end:
.equ K0_KERNELS, (k0_table_end - k0_table) / 4

/* r0 = kernel, r1 = N: best of K0_REPS. */
k0_best:
	push {r4-r7, lr}
	mov r4, r0
	mov r5, r1
	ldr r6, =SYST_MAX
	movs r7, #K0_REPS
1:	mov r0, r5
	ldr r1, =K0_TARGET
	blx r4
	cmp r0, r6
	bhs 2f
	mov r6, r0
2:	subs r7, #1
	bne 1b
	mov r0, r6
	pop {r4-r7, pc}
.ltorg

/* One exception run, the IRQ enabled or not (r0); its four stamps to [r1]. */
k0_xrun:
	push {r4-r7, lr}
	mov r7, r1
	ldr r2, =NVIC_ICPR
	ldr r3, =(1 << K0_IRQ)
	str r3, [r2]
	ldr r2, =NVIC_ISER
	cmp r0, #0
	bne 1f
	ldr r2, =NVIC_ICER
1:	str r3, [r2]
	ldr r6, =K0_STAMP
	movs r4, #0
	str r4, [r6, #4]
	str r4, [r6, #8]
	ldr r1, =SYST_CVR
	ldr r2, =NVIC_ISPR
	ldr r4, [r1]
	str r3, [r2]
	dsb
	isb
	ldr r5, [r1]
	str r4, [r7, #0]
	ldr r4, [r6, #4]
	str r4, [r7, #4]
	ldr r4, [r6, #8]
	str r4, [r7, #8]
	str r5, [r7, #12]
	ldr r2, =NVIC_ICER
	str r3, [r2]
	ldr r2, =NVIC_ICPR
	str r3, [r2]
	pop {r4-r7, pc}
.ltorg

k0_reset:
	cpsid i
	ldr r0, =SYST_RVR
	ldr r1, =SYST_MAX
	str r1, [r0]
	ldr r0, =SYST_CVR
	str r1, [r0]
	ldr r0, =SYST_CSR
	movs r1, #SYST_CSR_RUN
	str r1, [r0]
	ldr r4, k0_table_at
	ldr r5, =(K0_RESULTS + K0_RES_KERN)
	movs r6, #K0_KERNELS
1:	ldr r0, [r4]
	movs r1, #K0_N_LO
	bl k0_best
	str r0, [r5, #0]
	ldr r0, [r4]
	movs r1, #K0_N_HI
	bl k0_best
	str r0, [r5, #4]
	adds r4, #4
	adds r5, #8
	subs r6, #1
	bne 1b
	cpsie i
	ldr r5, =(K0_RESULTS + K0_RES_X)
	movs r6, #(2 * K0_XREPS)
2:	movs r0, #1
	ands r0, r6
	mov r1, r5
	bl k0_xrun
	adds r5, #16
	subs r6, #1
	bne 2b
	ldr r0, =K0_RESULTS
	movs r1, #K0_KERNELS
	str r1, [r0, #K0_RES_COUNT]
	ldr r1, =K0_DONE_MAGIC
	str r1, [r0, #K0_RES_DONE]
3:	wfi
	b 3b
.ltorg
.align 2
k0_table_at:
	.word k0_table - kern0_start

.align 2
.global kern0_end
kern0_end:
