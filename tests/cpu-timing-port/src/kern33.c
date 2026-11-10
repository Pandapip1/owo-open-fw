/*
 * Cortex-M33 timing kernels (docs/radio-tables.md 7.75, 7.93). Position independent, so the
 * same bytes run from flash and from a copy in SRAM.
 *
 * In: r0 = iterations N (> 0), r1 = target address. Out: r0 = DWT.CYCCNT cycles of the loop.
 * Every body is 8 copies of up to three instructions followed by subs/bne; the caller takes
 * the cost per iteration from two N and subtracts the empty loop. r11 holds the cycle counter's
 * address and r12 the start count; r2-r10 are free for the bodies.
 *
 * Every kernel stays asm: its exact instruction sequence and placement is what is measured.
 * Each is a naked function, so the compiler adds nothing around it; this file is built with
 * -fno-toplevel-reorder (CMakeLists.txt), so they keep their order between kern33_start and
 * kern33_end, the region main.c copies to SRAM.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdint.h>

#define DWT_CYCCNT "0xE0001004"

/* The start of the copied region: a bare label, no code. */
__asm__(".pushsection .text.kern33, \"ax\", %progbits\n"
	".align 4\n"
	".global kern33_start\n"
	"kern33_start:\n"
	".popsection\n");

#define KERN(name, pre, op1, op2, op3)                                                             \
	__attribute__((naked, aligned(16), section(".text.kern33"))) uint32_t name(            \
		__attribute__((unused)) uint32_t n, __attribute__((unused)) uintptr_t target)      \
	{                                                                                          \
		__asm__ volatile("push {r4-r11, lr}\n"                                             \
				 "movs r4, #1\n"                                                   \
				 "mov r5, r4\n"                                                    \
				 "mov r6, r4\n" pre "\n"                                           \
				 "ldr r11, =" DWT_CYCCNT "\n"                                      \
				 "ldr r12, [r11]\n"                                                \
				 "1:\n"                                                            \
				 ".rept 8\n" op1 "\n" op2 "\n" op3 "\n"                            \
				 ".endr\n"                                                         \
				 "subs r0, #1\n"                                                   \
				 "bne 1b\n"                                                        \
				 "9:\n"                                                            \
				 "ldr r0, [r11]\n"                                                 \
				 "sub r0, r0, r12\n"                                               \
				 "pop {r4-r11, pc}\n"                                              \
				 ".ltorg\n");                                                      \
	}

KERN(k_empty, "", "", "", "")
KERN(k_alu, "", "adds r4, #1", "", "")
KERN(k_alu_pair, "", "adds r4, #1", "adds r5, #1", "")
KERN(k_alu_w, "", "add.w r4, r4, #1", "", "")
KERN(k_mov, "", "mov r4, r5", "", "")
KERN(k_ubfx, "", "ubfx r4, r5, #3, #7", "", "")
KERN(k_nop, "", "nop", "", "")
KERN(k_nop_w, "", "nop.w", "", "")
KERN(k_ldr, "", "ldr r4, [r1]", "", "")
KERN(k_ldr_w, "", "ldr.w r4, [r1, #4]", "", "")
KERN(k_ldrb, "", "ldrb r4, [r1]", "", "")
KERN(k_ldrh, "", "ldrh r4, [r1]", "", "")
KERN(k_str, "", "str r4, [r1]", "", "")
KERN(k_strb, "", "strb r4, [r1]", "", "")
KERN(k_ldr_use, "", "ldr r4, [r1]", "adds r4, #1", "")
KERN(k_ldr_alu, "", "ldr r4, [r1]", "adds r5, #1", "")
KERN(k_str_alu, "", "str r4, [r1]", "adds r5, #1", "")
KERN(k_ldr_ldr, "", "ldr r4, [r1]", "ldr r5, [r1, #4]", "")
KERN(k_ldr_str_dep, "", "ldr r4, [r1]", "str r4, [r1, #4]", "")
KERN(k_ldr_str_indep, "", "ldr r4, [r1]", "str r5, [r1, #4]", "")
KERN(k_ldrb_strb_wb, "", "ldrb r4, [r1], #0", "strb r4, [r1, #0]!", "")
KERN(k_str_pre_wb, "", "str r4, [r1, #0]!", "", "")
KERN(k_str_post_wb, "", "str r4, [r1], #0", "", "")
KERN(k_ldr_lit, "", "ldr r4, =0x12345678", "", "")
KERN(k_ldr_lit_alu, "", "ldr r4, =0x12345678", "adds r5, #1", "")
KERN(k_ldrd, "", "ldrd r4, r5, [r1]", "", "")
KERN(k_strd, "", "strd r4, r5, [r1]", "", "")
KERN(k_ldm2, "", "ldm r1, {r4-r5}", "", "")
KERN(k_ldm4, "", "ldm r1, {r4-r7}", "", "")
KERN(k_ldm8, "", "ldm r1, {r2-r9}", "", "")
KERN(k_stm4, "", "stm r1, {r4-r7}", "", "")
KERN(k_push_pop1, "", "push {r4}", "pop {r4}", "")
KERN(k_push_pop4, "", "push {r4-r7}", "pop {r4-r7}", "")
KERN(k_push_pop8, "", "push {r2-r9}", "pop {r2-r9}", "")
KERN(k_mul, "movs r5, #3", "muls r4, r5, r4", "", "")
KERN(k_mla, "movs r5, #3", "mla r4, r5, r6, r4", "", "")
KERN(k_umull, "mvn r6, #0", "umull r4, r5, r6, r6", "", "")
KERN(k_umlal, "mvn r6, #0", "umlal r4, r5, r6, r6", "", "")
KERN(k_umaal, "mvn r6, #0", "umaal r4, r5, r6, r6", "", "")
KERN(k_umull_use, "mvn r6, #0", "umull r4, r5, r6, r6", "adds r4, #1", "")
KERN(k_umull_chain, "mvn r6, #0", "umull r4, r6, r6, r6", "", "")
KERN(k_udiv_7_3, "movs r5, #7; movs r6, #3", "udiv r4, r5, r6", "", "")
KERN(k_udiv_max_3, "mvn r5, #0; movs r6, #3", "udiv r4, r5, r6", "", "")
KERN(k_udiv_max_65536, "mvn r5, #0; mov r6, #0x10000", "udiv r4, r5, r6", "", "")
KERN(k_udiv_max_max, "mvn r5, #0; mvn r6, #0", "udiv r4, r5, r6", "", "")
KERN(k_sdiv_m100_7, "mvn r5, #99; movs r6, #7", "sdiv r4, r5, r6", "", "")
KERN(k_b, "", "b 2f", "2:", "")
KERN(k_bcc_taken, "", "bne 2f", "2:", "")
KERN(k_bcc_not, "", "beq 9f", "", "")
KERN(k_cbnz_taken, "", "cbnz r1, 2f", "adds r4, #1", "2:")
KERN(k_cbz_not, "", "cbz r1, 9f", "", "")

/* The call target of k_bl and k_blx: a bare label, so adr gives its even address. */
__asm__(".pushsection .text.kern33, \"ax\", %progbits\n"
	".align 2\n"
	"kern33_ret:\n"
	"bx lr\n"
	".popsection\n");

KERN(k_bl, "", "bl kern33_ret", "", "")
KERN(k_blx, "adr r9, kern33_ret; orr r9, r9, #1", "blx r9", "", "")
KERN(k_it_taken, "", "it ne", "addne r4, #1", "")
KERN(k_it_not, "", "it eq", "addeq r4, #1", "")
KERN(k_mrs, "", "mrs r4, primask", "", "")
KERN(k_msr, "movs r4, #0", "msr basepri, r4", "", "")
KERN(k_dmb, "", "dmb", "", "")
KERN(k_dsb, "", "dsb", "", "")
KERN(k_isb, "", "isb", "", "")

/*
 * Exception entry and return (7.94). x_pend: r0 = NVIC.ISPR word, r1 = the IRQ's bit, r2 = a
 * four-word stamp block; DWT.CYCCNT before the pending store into word 0 and after the barriers
 * into word 3. x_isr, the IRQ's handler: CYCCNT at its first and last load into words 1 and 2
 * of x_stamp. With the IRQ disabled the same sequence is the control.
 */
__attribute__((naked, aligned(16), section(".text.kern33"))) uint32_t
x_pend(__attribute__((unused)) volatile uint32_t *ispr, __attribute__((unused)) uint32_t bit,
       __attribute__((unused)) volatile uint32_t *stamps)
{
	__asm__ volatile("push {r4-r6, lr}\n"
			 "movw r4, #:lower16:" DWT_CYCCNT "\n"
			 "movt r4, #:upper16:" DWT_CYCCNT "\n"
			 "ldr r5, [r4]\n"
			 "str r1, [r0]\n"
			 "dsb\n"
			 "isb\n"
			 "ldr r6, [r4]\n"
			 "str r5, [r2]\n"
			 "str r6, [r2, #12]\n"
			 "sub r0, r6, r5\n"
			 "pop {r4-r6, pc}\n");
}

__attribute__((naked, aligned(16), section(".text.kern33"))) void x_isr(void)
{
	__asm__ volatile("movw r0, #:lower16:" DWT_CYCCNT "\n"
			 "movt r0, #:upper16:" DWT_CYCCNT "\n"
			 "ldr r1, [r0]\n"
			 "ldr r3, [r0]\n"
			 "movw r2, #:lower16:x_stamp\n"
			 "movt r2, #:upper16:x_stamp\n"
			 "str r1, [r2, #4]\n"
			 "str r3, [r2, #8]\n"
			 "bx lr\n");
}

/* The end of the copied region. */
__asm__(".pushsection .text.kern33, \"ax\", %progbits\n"
	".global kern33_end\n"
	"kern33_end:\n"
	".popsection\n");

__attribute__((aligned(16), section(".bss.x_stamp"))) volatile uint32_t x_stamp[4];

/*
 * Straight-line blocks for the instruction cache: 16-bit ALU instructions over a given span,
 * run once after a cache flush and once warm (the caller brackets them).
 */
#define ICBLOCK(name, bytes)                                                                       \
	__attribute__((naked, aligned(16), section(".text.icblocks"))) uint32_t name(void)        \
	{                                                                                          \
		__asm__ volatile("push {r4, r11, lr}\n"                                            \
				 "movw r11, #:lower16:" DWT_CYCCNT "\n"                            \
				 "movt r11, #:upper16:" DWT_CYCCNT "\n"                            \
				 "ldr r12, [r11]\n"                                                \
				 ".rept (" #bytes " / 2)\n"                                        \
				 "adds r4, #1\n"                                                   \
				 ".endr\n"                                                         \
				 "ldr r0, [r11]\n"                                                 \
				 "sub r0, r0, r12\n"                                               \
				 "pop {r4, r11, pc}\n"                                             \
				 ".ltorg\n");                                                      \
	}

ICBLOCK(ic_512, 512)
ICBLOCK(ic_1k, 1024)
ICBLOCK(ic_2k, 2048)
ICBLOCK(ic_4k, 4096)
ICBLOCK(ic_8k, 8192)
ICBLOCK(ic_16k, 16384)
