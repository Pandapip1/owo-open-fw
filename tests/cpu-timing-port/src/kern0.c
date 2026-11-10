/*
 * Cortex-M0+ (radio sequencer core) timing kernels (docs/radio-tables.md 7.75, 7.93, 7.94).
 * A self-contained image for SEQRAM, linked at 0 on its own (kern0.ld, CMakeLists.txt) and
 * embedded in the M33 application as data: the M33 copies it to the M0+'s address 0
 * (0xB0000000 on the M33), holds the core in RAC.CTRL.SEQRESET for the copy and releases it
 * with CPUWAITDIS.
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
 * The kernels, the handler and the pend between its stamps stay asm: their exact instruction
 * sequence and placement is what is measured.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdint.h>

#define REG(addr) (*(volatile uint32_t *)(addr))

#define SYST_CSR 0xE000E010U
#define SYST_RVR 0xE000E014U
/* SYST_CVR and K0_STAMP carry no U suffix: the asm below uses them too. */
#define SYST_CVR 0xE000E018
/* CSR: ENABLE, CLKSOURCE = processor clock, no interrupt. */
#define SYST_CSR_RUN 5U
#define SYST_MAX 0x00FFFFFFU
#define NVIC_ISER 0xE000E100U
#define NVIC_ICER 0xE000E180U
#define NVIC_ISPR 0xE000E200U
#define NVIC_ICPR 0xE000E280U
#define K0_IRQ 23

/* M0+ addresses, shared with main.c. The sequencer's own stack top (radio_seq.S). */
#define K0_STACK_TOP 0x00003e00U
#define K0_RESULTS 0x00003000U
#define K0_TARGET 0x00003800U
#define K0_DONE_MAGIC 0x4E44304BU
#define K0_N_LO 10U
#define K0_N_HI 110U
#define K0_REPS 3U
#define K0_XREPS 4U
/*
 * Results: word 0 done, word 1 kernel count, then lo/hi per kernel; at K0_RES_X the exception
 * runs (2 x K0_XREPS, odd ones enabled, four stamps each); K0_STAMP the handler's two words.
 */
#define K0_RES_DONE 0U
#define K0_RES_COUNT 4U
#define K0_RES_KERN 8U
#define K0_RES_X 0x200U
#define K0_STAMP 0x00003700

#define FIXED_EXCEPTIONS 16
#define EXTERNAL_IRQS 32

typedef uint32_t (*k0_fn)(uint32_t n, uint32_t target);

void k0_reset(void);
void k0_isr(void);

static void k0_fault(void)
{
	for (;;) {
	}
}

__attribute__((section(".vectors"), used)) static void (*const k0_vectors[])(void) = {
	(void (*)(void))K0_STACK_TOP,
	k0_reset,
	[2 ... FIXED_EXCEPTIONS + K0_IRQ - 1] = k0_fault,
	[FIXED_EXCEPTIONS + K0_IRQ] = k0_isr,
	[FIXED_EXCEPTIONS + K0_IRQ + 1 ... FIXED_EXCEPTIONS + EXTERNAL_IRQS - 1] = k0_fault,
};

#define STR_(x) #x
#define STR(x) STR_(x)

/* Stamps of one exception run: before the pend, handler entry, handler exit, after return. */
__attribute__((naked)) void k0_isr(void)
{
	__asm__ volatile("ldr r1, =" STR(SYST_CVR) "\n"
			 "ldr r0, [r1]\n"
			 "ldr r3, [r1]\n"
			 "ldr r2, =" STR(K0_STAMP) "\n"
			 "str r0, [r2, #4]\n"
			 "str r3, [r2, #8]\n"
			 "bx lr\n"
			 ".ltorg\n");
}

#define K0(name, align, op1, op2)                                                                  \
	__attribute__((naked, aligned(8))) uint32_t name(__attribute__((unused)) uint32_t n,   \
							 __attribute__((unused)) uint32_t target) \
	{                                                                                          \
		__asm__ volatile("push {r4-r7, lr}\n"                                              \
				 "ldr r3, =" STR(SYST_CVR) "\n"                                    \
				 "ldr r2, [r3]\n"                                                  \
				 "b 2f\n"                                                          \
				 ".ltorg\n"                                                        \
				 ".align 3\n"                                                      \
				 "2:\n"                                                            \
				 ".rept (" #align " / 2)\n"                                        \
				 "nop\n"                                                           \
				 ".endr\n"                                                         \
				 "1:\n"                                                            \
				 ".rept 8\n" op1 "\n" op2 "\n"                                     \
				 ".endr\n"                                                         \
				 "subs r0, #1\n"                                                   \
				 "bne 1b\n"                                                        \
				 "ldr r0, [r3]\n"                                                  \
				 "subs r0, r2, r0\n"                                               \
				 "lsls r0, r0, #8\n"                                               \
				 "lsrs r0, r0, #8\n"                                               \
				 "pop {r4-r7, pc}\n");                                             \
	}

/* radio_seq.S's efr32_seq_delay pass (ldr, subs, str, taken bpl on a stack word), N passes. */
#define K0DELAY(name, align)                                                                       \
	__attribute__((naked, aligned(8))) uint32_t name(__attribute__((unused)) uint32_t n,   \
							 __attribute__((unused)) uint32_t target) \
	{                                                                                          \
		__asm__ volatile("push {r4-r7, lr}\n"                                              \
				 "sub sp, #4\n"                                                    \
				 "movs r4, #30\n"                                                  \
				 "subs r0, #1\n"                                                   \
				 "muls r4, r0, r4\n"                                               \
				 "str r4, [sp]\n"                                                  \
				 "ldr r3, =" STR(SYST_CVR) "\n"                                    \
				 "ldr r2, [r3]\n"                                                  \
				 "b 2f\n"                                                          \
				 ".ltorg\n"                                                        \
				 ".align 3\n"                                                      \
				 "2:\n"                                                            \
				 ".rept (" #align " / 2)\n"                                        \
				 "nop\n"                                                           \
				 ".endr\n"                                                         \
				 "1:\n"                                                            \
				 "ldr r0, [sp]\n"                                                  \
				 "subs r0, #30\n"                                                  \
				 "str r0, [sp]\n"                                                  \
				 "bpl 1b\n"                                                        \
				 "ldr r0, [r3]\n"                                                  \
				 "subs r0, r2, r0\n"                                               \
				 "lsls r0, r0, #8\n"                                               \
				 "lsrs r0, r0, #8\n"                                               \
				 "add sp, #4\n"                                                    \
				 "pop {r4-r7, pc}\n");                                             \
	}

K0(k0_empty, 0, "", "")
K0(k0_nop, 0, "nop", "")
K0(k0_alu, 0, "adds r4, #1", "")
K0(k0_str_a0, 0, "str r4, [r1]", "")
K0(k0_str_a2, 2, "str r4, [r1]", "")
K0(k0_str_a4, 4, "str r4, [r1]", "")
K0(k0_str_a6, 6, "str r4, [r1]", "")
K0(k0_ldr_a0, 0, "ldr r4, [r1]", "")
K0(k0_ldr_a2, 2, "ldr r4, [r1]", "")
K0(k0_ldr_a4, 4, "ldr r4, [r1]", "")
K0(k0_ldr_a6, 6, "ldr r4, [r1]", "")
K0(k0_stralu_a0, 0, "str r4, [r1]", "adds r5, #1")
K0(k0_stralu_a2, 2, "str r4, [r1]", "adds r5, #1")
K0(k0_stralu_a4, 4, "str r4, [r1]", "adds r5, #1")
K0(k0_stralu_a6, 6, "str r4, [r1]", "adds r5, #1")
K0DELAY(k0_delay_a0, 0)
K0DELAY(k0_delay_a2, 2)
K0DELAY(k0_delay_a4, 4)
K0DELAY(k0_delay_a6, 6)

/* main.c's k0_name[] lists the same kernels in this order. */
static const k0_fn k0_table[] = {
	k0_empty,     k0_nop,       k0_alu,       k0_str_a0,    k0_str_a2,
	k0_str_a4,    k0_str_a6,    k0_ldr_a0,    k0_ldr_a2,    k0_ldr_a4,
	k0_ldr_a6,    k0_stralu_a0, k0_stralu_a2, k0_stralu_a4, k0_stralu_a6,
	k0_delay_a0,  k0_delay_a2,  k0_delay_a4,  k0_delay_a6,
};
#define K0_KERNELS (sizeof(k0_table) / sizeof(k0_table[0]))

/* Best of K0_REPS runs of one kernel at N iterations. */
static uint32_t k0_best(k0_fn kernel, uint32_t n)
{
	uint32_t best = SYST_MAX;

	for (uint32_t r = 0; r < K0_REPS; r++) {
		uint32_t t = kernel(n, K0_TARGET);

		if (t < best) {
			best = t;
		}
	}
	return best;
}

/* One exception run, the IRQ enabled or not; its four stamps to out[0..3]. */
static void k0_xrun(uint32_t enabled, volatile uint32_t *out)
{
	volatile uint32_t *stamp = (volatile uint32_t *)K0_STAMP;
	uint32_t bit = 1U << K0_IRQ, before, after;

	REG(NVIC_ICPR) = bit;
	REG(enabled ? NVIC_ISER : NVIC_ICER) = bit;
	stamp[1] = 0U;
	stamp[2] = 0U;
	/* The stamps around the pend: the control's whole cost is these instructions. */
	__asm__ volatile("ldr %0, [%2]\n"
			 "str %4, [%3]\n"
			 "dsb\n"
			 "isb\n"
			 "ldr %1, [%2]\n"
			 : "=&l"(before), "=&l"(after)
			 : "l"(SYST_CVR), "l"(NVIC_ISPR), "l"(bit)
			 : "memory");
	out[0] = before;
	out[1] = stamp[1];
	out[2] = stamp[2];
	out[3] = after;
	REG(NVIC_ICER) = bit;
	REG(NVIC_ICPR) = bit;
}

void k0_reset(void)
{
	volatile uint32_t *results = (volatile uint32_t *)K0_RESULTS;
	volatile uint32_t *kern = (volatile uint32_t *)(K0_RESULTS + K0_RES_KERN);
	volatile uint32_t *xrun = (volatile uint32_t *)(K0_RESULTS + K0_RES_X);

	__asm__ volatile("cpsid i" ::: "memory");
	REG(SYST_RVR) = SYST_MAX;
	REG(SYST_CVR) = SYST_MAX;
	REG(SYST_CSR) = SYST_CSR_RUN;
	for (uint32_t k = 0; k < K0_KERNELS; k++) {
		kern[2U * k] = k0_best(k0_table[k], K0_N_LO);
		kern[2U * k + 1U] = k0_best(k0_table[k], K0_N_HI);
	}
	__asm__ volatile("cpsie i" ::: "memory");
	/* The control first, then alternately enabled and not (main.c: odd runs are enabled). */
	for (uint32_t r = 2U * K0_XREPS; r > 0U; r--) {
		k0_xrun(r & 1U, xrun);
		xrun += 4;
	}
	results[K0_RES_COUNT / 4U] = K0_KERNELS;
	results[K0_RES_DONE / 4U] = K0_DONE_MAGIC;
	for (;;) {
		__asm__ volatile("wfi");
	}
}
