/*
 * Cortex-M33 timing kernels (docs/radio-tables.md 7.75, 7.93). Position independent, so the
 * same bytes run from flash and from a copy in SRAM.
 *
 * In: r0 = iterations N (> 0), r1 = target address. Out: r0 = DWT.CYCCNT cycles of the loop.
 * Every body is 8 copies of up to three instructions followed by subs/bne; the caller takes
 * the cost per iteration from two N and subtracts the empty loop. r11 holds the cycle counter's
 * address and r12 the start count; r2-r10 are free for the bodies.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
.syntax unified
.thumb
.cpu cortex-m33

.equ DWT_CYCCNT, 0xE0001004

.section .text.kern33, "ax", %progbits
.align 4
.global kern33_start
kern33_start:

.macro KERN name, pre, op1, op2, op3
.align 4
.global \name
.thumb_func
\name:
	push {r4-r11, lr}
	movs r4, #1
	mov r5, r4
	mov r6, r4
	\pre
	ldr r11, =DWT_CYCCNT
	ldr r12, [r11]
1:
	.rept 8
	\op1
	\op2
	\op3
	.endr
	subs r0, #1
	bne 1b
9:
	ldr r0, [r11]
	sub r0, r0, r12
	pop {r4-r11, pc}
.ltorg
.endm

KERN k_empty, "", "", "", ""
KERN k_alu, "", "adds r4, #1", "", ""
KERN k_alu_pair, "", "adds r4, #1", "adds r5, #1", ""
KERN k_alu_w, "", "add.w r4, r4, #1", "", ""
KERN k_mov, "", "mov r4, r5", "", ""
KERN k_ubfx, "", "ubfx r4, r5, #3, #7", "", ""
KERN k_nop, "", "nop", "", ""
KERN k_nop_w, "", "nop.w", "", ""
KERN k_ldr, "", "ldr r4, [r1]", "", ""
KERN k_ldr_w, "", "ldr.w r4, [r1, #4]", "", ""
KERN k_ldrb, "", "ldrb r4, [r1]", "", ""
KERN k_ldrh, "", "ldrh r4, [r1]", "", ""
KERN k_str, "", "str r4, [r1]", "", ""
KERN k_strb, "", "strb r4, [r1]", "", ""
KERN k_ldr_use, "", "ldr r4, [r1]", "adds r4, #1", ""
KERN k_ldr_alu, "", "ldr r4, [r1]", "adds r5, #1", ""
KERN k_str_alu, "", "str r4, [r1]", "adds r5, #1", ""
KERN k_ldr_ldr, "", "ldr r4, [r1]", "ldr r5, [r1, #4]", ""
KERN k_ldr_str_dep, "", "ldr r4, [r1]", "str r4, [r1, #4]", ""
KERN k_ldr_str_indep, "", "ldr r4, [r1]", "str r5, [r1, #4]", ""
KERN k_ldrb_strb_wb, "", "ldrb r4, [r1], #0", "strb r4, [r1, #0]!", ""
KERN k_str_pre_wb, "", "str r4, [r1, #0]!", "", ""
KERN k_str_post_wb, "", "str r4, [r1], #0", "", ""
KERN k_ldr_lit, "","ldr r4, =0x12345678", "", ""
KERN k_ldr_lit_alu, "", "ldr r4, =0x12345678", "adds r5, #1", ""
KERN k_ldrd, "", "ldrd r4, r5, [r1]", "", ""
KERN k_strd, "", "strd r4, r5, [r1]", "", ""
KERN k_ldm2, "", "ldm r1, {r4-r5}", "", ""
KERN k_ldm4, "", "ldm r1, {r4-r7}", "", ""
KERN k_ldm8, "", "ldm r1, {r2-r9}", "", ""
KERN k_stm4, "", "stm r1, {r4-r7}", "", ""
KERN k_push_pop1, "", "push {r4}", "pop {r4}", ""
KERN k_push_pop4, "", "push {r4-r7}", "pop {r4-r7}", ""
KERN k_push_pop8, "", "push {r2-r9}", "pop {r2-r9}", ""
KERN k_mul, "movs r5, #3", "muls r4, r5, r4", "", ""
KERN k_mla, "movs r5, #3", "mla r4, r5, r6, r4", "", ""
KERN k_umull, "mvn r6, #0", "umull r4, r5, r6, r6", "", ""
KERN k_umlal, "mvn r6, #0", "umlal r4, r5, r6, r6", "", ""
KERN k_umaal, "mvn r6, #0", "umaal r4, r5, r6, r6", "", ""
KERN k_umull_use, "mvn r6, #0", "umull r4, r5, r6, r6", "adds r4, #1", ""
KERN k_umull_chain, "mvn r6, #0", "umull r4, r6, r6, r6", "", ""
KERN k_udiv_7_3, "movs r5, #7; movs r6, #3", "udiv r4, r5, r6", "", ""
KERN k_udiv_max_3, "mvn r5, #0; movs r6, #3", "udiv r4, r5, r6", "", ""
KERN k_udiv_max_65536, "mvn r5, #0; mov r6, #0x10000", "udiv r4, r5, r6", "", ""
KERN k_udiv_max_max, "mvn r5, #0; mvn r6, #0", "udiv r4, r5, r6", "", ""
KERN k_sdiv_m100_7, "mvn r5, #99; movs r6, #7", "sdiv r4, r5, r6", "", ""
KERN k_b, "", "b 2f", "2:", ""
KERN k_bcc_taken, "", "bne 2f", "2:", ""
KERN k_bcc_not, "", "beq 9f", "", ""
KERN k_cbnz_taken, "", "cbnz r1, 2f", "adds r4, #1", "2:"
KERN k_cbz_not, "", "cbz r1, 9f", "", ""
.align 2
kern33_ret:
	bx lr

KERN k_bl, "", "bl kern33_ret", "", ""
KERN k_blx, "adr r9, kern33_ret; orr r9, r9, #1", "blx r9", "", ""
KERN k_it_taken, "", "it ne", "addne r4, #1", ""
KERN k_it_not, "", "it eq", "addeq r4, #1", ""
KERN k_mrs, "", "mrs r4, primask", "", ""
KERN k_msr, "movs r4, #0", "msr basepri, r4", "", ""
KERN k_dmb, "", "dmb", "", ""
KERN k_dsb, "", "dsb", "", ""
KERN k_isb, "", "isb", "", ""

/*
 * Straight-line blocks for the instruction cache: 16-bit ALU instructions over a given span,
 * run once after a cache flush and once warm (the caller brackets them).
 */
.macro ICBLOCK name, bytes
.align 4
.global \name
.thumb_func
\name:
	push {r4, r11, lr}
	movw r11, #:lower16:DWT_CYCCNT
	movt r11, #:upper16:DWT_CYCCNT
	ldr r12, [r11]
	.rept (\bytes / 2)
	adds r4, #1
	.endr
	ldr r0, [r11]
	sub r0, r0, r12
	pop {r4, r11, pc}
.ltorg
.endm

.global kern33_end
kern33_end:

.section .text.icblocks, "ax", %progbits
ICBLOCK ic_512, 512
ICBLOCK ic_1k, 1024
ICBLOCK ic_2k, 2048
ICBLOCK ic_4k, 4096
ICBLOCK ic_8k, 8192
ICBLOCK ic_16k, 16384
