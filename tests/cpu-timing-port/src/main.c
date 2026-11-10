/*
 * Cortex-M33 instruction and access timing (docs/radio-tables.md 7.75, 7.93), for the QEMU
 * model's per-instruction-class cycle counts. The same image runs on silicon and in QEMU and
 * prints one line per figure.
 *
 * Kernels (src/kern33.c) run with interrupts locked, from flash and from a copy in SRAM, at
 * N = 10 and N = 110 iterations; "iter" is the DWT.CYCCNT cost of one iteration in
 * millicycles, "op" that less the empty loop, divided by the 8 copies of the body. Then
 * library and firmware functions (cold call after an instruction-cache flush, then warm), and
 * straight-line blocks for the instruction cache's size and miss cost. Then exception entry
 * and return on the M33, and the M0+'s kernels and exception run on the radio sequencer core
 * (src/kern0.c, 7.94).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <string.h>
#include <soc.h>
#include <zephyr/irq.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/sys_io.h>

#define N_LO 10U
#define N_HI 110U
#define REPS 3U
#define CALL_REPS 6U

typedef uint32_t (*kern_fn)(uint32_t n, uintptr_t target);

#define KERNELS(X)                                                                                 \
	X(k_empty, T_NONE)                                                                         \
	X(k_alu, T_NONE)                                                                           \
	X(k_alu_pair, T_NONE)                                                                      \
	X(k_alu_w, T_NONE)                                                                         \
	X(k_mov, T_NONE)                                                                           \
	X(k_ubfx, T_NONE)                                                                          \
	X(k_nop, T_NONE)                                                                           \
	X(k_nop_w, T_NONE)                                                                         \
	X(k_ldr, T_LOAD_ALL)                                                                       \
	X(k_ldr_w, T_LOAD_ALL)                                                                     \
	X(k_ldrb, T_MEM)                                                                           \
	X(k_ldrh, T_MEM)                                                                           \
	X(k_str, T_STORE_ALL)                                                                      \
	X(k_strb, T_SRAM)                                                                          \
	X(k_ldr_use, T_MEM)                                                                        \
	X(k_ldr_alu, T_LOAD_ALL)                                                                   \
	X(k_str_alu, T_STORE_ALL)                                                                  \
	X(k_ldr_ldr, T_LOAD_ALL)                                                                   \
	X(k_ldr_str_dep, T_SRAM)                                                                   \
	X(k_ldr_str_indep, T_SRAM)                                                                 \
	X(k_ldrb_strb_wb, T_SRAM)                                                                  \
	X(k_str_pre_wb, T_SRAM)                                                                    \
	X(k_str_post_wb, T_SRAM)                                                                   \
	X(k_ldr_lit, T_NONE)                                                                       \
	X(k_ldr_lit_alu, T_NONE)                                                                   \
	X(k_ldrd, T_MEM)                                                                           \
	X(k_strd, T_SRAM)                                                                          \
	X(k_ldm2, T_MEM)                                                                           \
	X(k_ldm4, T_MEM)                                                                           \
	X(k_ldm8, T_MEM)                                                                           \
	X(k_stm4, T_SRAM)                                                                          \
	X(k_push_pop1, T_NONE)                                                                     \
	X(k_push_pop4, T_NONE)                                                                     \
	X(k_push_pop8, T_NONE)                                                                     \
	X(k_mul, T_NONE)                                                                           \
	X(k_mla, T_NONE)                                                                           \
	X(k_umull, T_NONE)                                                                         \
	X(k_umlal, T_NONE)                                                                         \
	X(k_umaal, T_NONE)                                                                         \
	X(k_umull_use, T_NONE)                                                                     \
	X(k_umull_chain, T_NONE)                                                                   \
	X(k_udiv_7_3, T_NONE)                                                                      \
	X(k_udiv_max_3, T_NONE)                                                                    \
	X(k_udiv_max_65536, T_NONE)                                                                \
	X(k_udiv_max_max, T_NONE)                                                                  \
	X(k_sdiv_m100_7, T_NONE)                                                                   \
	X(k_b, T_NONE)                                                                             \
	X(k_bcc_taken, T_NONE)                                                                     \
	X(k_bcc_not, T_NONE)                                                                       \
	X(k_cbnz_taken, T_NONE)                                                                    \
	X(k_cbz_not, T_NONE)                                                                       \
	X(k_bl, T_NONE)                                                                            \
	X(k_blx, T_NONE)                                                                           \
	X(k_it_taken, T_NONE)                                                                      \
	X(k_it_not, T_NONE)                                                                        \
	X(k_mrs, T_NONE)                                                                           \
	X(k_msr, T_NONE)                                                                           \
	X(k_dmb, T_NONE)                                                                           \
	X(k_dsb, T_NONE)                                                                           \
	X(k_isb, T_NONE)

#define KERN_DECL(name, targets) extern uint32_t name(uint32_t n, uintptr_t target);
KERNELS(KERN_DECL)
extern uint8_t kern33_start[], kern33_end[];
extern uint32_t ic_512(void), ic_1k(void), ic_2k(void), ic_4k(void), ic_8k(void), ic_16k(void);

/* Targets: main SRAM, flash, and the slave ports of docs/radio-tables.md 7.74/7.75. */
enum target {
	TGT_SRAM,
	TGT_FLASH,
	TGT_SEQRAM,
	TGT_RAC,
	TGT_RAC_SR2,
	TGT_BUFC,
	TGT_TIMER1,
	TGT_CMU,
	TGT_GPIO,
	TGT_COUNT,
};
#define BIT_T(t) (1U << (t))
#define T_NONE BIT_T(TGT_SRAM)
#define T_SRAM BIT_T(TGT_SRAM)
#define T_MEM (BIT_T(TGT_SRAM) | BIT_T(TGT_FLASH))
#define T_LOAD_ALL                                                                                 \
	(T_MEM | BIT_T(TGT_SEQRAM) | BIT_T(TGT_RAC) | BIT_T(TGT_BUFC) | BIT_T(TGT_TIMER1) |        \
	 BIT_T(TGT_CMU) | BIT_T(TGT_GPIO))
#define T_STORE_ALL (BIT_T(TGT_SRAM) | BIT_T(TGT_SEQRAM) | BIT_T(TGT_RAC_SR2))

static uint32_t sram_buf[16] __aligned(16);
static const uint32_t flash_buf[16] __aligned(16) = {
	0x11111111, 0x22222222, 0x33333333, 0x44444444, 0x55555555, 0x66666666, 0x77777777, 0x88888888,
};

static const char *const target_name[TGT_COUNT] = {
	"sram", "flash", "seqram", "rac", "rac.sr2", "bufc", "timer1.cnt", "cmu", "gpio",
};

static uintptr_t target_addr(enum target t)
{
	switch (t) {
	case TGT_SRAM:
		return (uintptr_t)sram_buf;
	case TGT_FLASH:
		return (uintptr_t)flash_buf;
	case TGT_SEQRAM:
		return 0xB0002000U;
	case TGT_RAC:
		return 0xB802000CU;
	case TGT_RAC_SR2:
		return 0xB8020048U;
	case TGT_BUFC:
		return 0xBA000000U;
	case TGT_TIMER1:
		return (uintptr_t)&TIMER1->CNT;
	case TGT_CMU:
		return (uintptr_t)&CMU->SYSCLKCTRL;
	default:
		return (uintptr_t)&GPIO->P[0].DIN;
	}
}

struct kernel {
	const char *name;
	kern_fn fn;
	uint32_t targets;
};

#define KERN_ENTRY(name, t) {#name, name, t},
static const struct kernel kernels[] = {KERNELS(KERN_ENTRY)};

/* The kernels copied to SRAM: same bytes, so every PC-relative reference still holds. */
static uint8_t sram_code[8192] __aligned(16);

static kern_fn in_sram(kern_fn fn)
{
	uintptr_t off = ((uintptr_t)fn & ~1U) - (uintptr_t)kern33_start;

	return (kern_fn)((uintptr_t)sram_code + off + 1U);
}

static uint32_t run_min(kern_fn fn, uint32_t n, uintptr_t target)
{
	uint32_t best = UINT32_MAX;

	for (uint32_t i = 0; i < REPS; i++) {
		unsigned int key = irq_lock();
		uint32_t c = fn(n, target);

		irq_unlock(key);
		best = MIN(best, c);
	}
	return best;
}

/* Millicycles per iteration. */
static int32_t per_iter(kern_fn fn, uintptr_t target)
{
	(void)run_min(fn, N_LO, target);
	return (int32_t)(((int64_t)run_min(fn, N_HI, target) - run_min(fn, N_LO, target)) * 1000 /
			 (int64_t)(N_HI - N_LO));
}

static void run_kernels(void)
{
	for (int code = 0; code < 2; code++) {
		const char *where = code ? "sram" : "flash";
		kern_fn empty = code ? in_sram(k_empty) : k_empty;
		int32_t base = per_iter(empty, (uintptr_t)sram_buf);

		for (size_t k = 0; k < ARRAY_SIZE(kernels); k++) {
			kern_fn fn = code ? in_sram(kernels[k].fn) : kernels[k].fn;

			for (int t = 0; t < TGT_COUNT; t++) {
				if (!(kernels[k].targets & BIT_T(t))) {
					continue;
				}
				int32_t it = per_iter(fn, target_addr(t));

				printk("K %-18s code %-5s target %-10s iter %6d op %6d\n",
				       kernels[k].name, where, target_name[t], it, (it - base) / 8);
			}
		}
	}
}

static void icache_flush(void)
{
	ICACHE0->CMD = ICACHE_CMD_FLUSH;
	__DSB();
	__ISB();
}

static __noinline void empty_call(void)
{
	__asm__ volatile("");
}

/* The 7.75 call: format and string in SRAM, as the debugger set them up on silicon. */
static char fmt_buf[] = "%d %x %s|%u-%08x";
static char str_buf[] = "hello world";
static char out_buf[96];
static uint8_t copy_src[256], copy_dst[256];

enum call {
	CALL_EMPTY,
	CALL_SNPRINTK,
	CALL_MEMCPY,
	CALL_MEMSET,
	CALL_STRLEN,
	CALL_COUNT,
};
static const char *const call_name[CALL_COUNT] = {"empty", "snprintk", "memcpy256", "memset256",
						  "strlen"};

static uint32_t call_once(enum call c)
{
	unsigned int key = irq_lock();
	uint32_t t0 = DWT->CYCCNT;

	switch (c) {
	case CALL_EMPTY:
		empty_call();
		break;
	case CALL_SNPRINTK:
		snprintk(out_buf, sizeof(out_buf), fmt_buf, 123456, 0xbeef, str_buf, 42U,
			 0xdeadbeefU);
		break;
	case CALL_MEMCPY:
		memcpy(copy_dst, copy_src, sizeof(copy_dst));
		break;
	case CALL_MEMSET:
		memset(copy_dst, 0x5a, sizeof(copy_dst));
		break;
	default:
		__asm__ volatile("" ::"r"(strlen(str_buf)));
		break;
	}
	uint32_t t1 = DWT->CYCCNT;

	irq_unlock(key);
	return t1 - t0;
}

static void run_calls(void)
{
	for (int c = 0; c < CALL_COUNT; c++) {
		uint32_t cold, warm = UINT32_MAX;

		icache_flush();
		cold = call_once(c);
		for (uint32_t i = 0; i < CALL_REPS; i++) {
			warm = MIN(warm, call_once(c));
		}
		printk("C %-10s cold %6u warm %6u\n", call_name[c], cold, warm);
	}
	printk("C snprintk output \"%s\"\n", out_buf);
}

static void run_icache(void)
{
	static const struct {
		const char *name;
		uint32_t (*fn)(void);
		uint32_t bytes;
	} blocks[] = {
		{"ic_512", ic_512, 512},   {"ic_1k", ic_1k, 1024},   {"ic_2k", ic_2k, 2048},
		{"ic_4k", ic_4k, 4096},    {"ic_8k", ic_8k, 8192},   {"ic_16k", ic_16k, 16384},
	};

	for (size_t b = 0; b < ARRAY_SIZE(blocks); b++) {
		unsigned int key = irq_lock();
		uint32_t cold, warm, hits[2], misses[2];

		icache_flush();
		ICACHE0->CMD = ICACHE_CMD_STARTPC;
		cold = blocks[b].fn();
		ICACHE0->CMD = ICACHE_CMD_STOPPC;
		hits[0] = ICACHE0->PCHITS;
		misses[0] = ICACHE0->PCMISSES;
		ICACHE0->CMD = ICACHE_CMD_STARTPC;
		warm = blocks[b].fn();
		ICACHE0->CMD = ICACHE_CMD_STOPPC;
		hits[1] = ICACHE0->PCHITS;
		misses[1] = ICACHE0->PCMISSES;
		irq_unlock(key);
		printk("I %-7s bytes %5u insns %5u cold %6u (hits %5u misses %5u) warm %6u "
		       "(hits %5u misses %5u)\n",
		       blocks[b].name, blocks[b].bytes, blocks[b].bytes / 2U, cold, hits[0],
		       misses[0], warm, hits[1], misses[1]);
	}
}

/*
 * Exception entry and return on the M33 (7.94): SW0, a software-only line, pended through
 * NVIC.ISPR at zero-latency priority (so irq_lock() leaves it), its handler x_isr reached
 * through the flash vector table (Zephyr's, VTOR at reset), through a copy in SRAM, and with
 * the caller and handler themselves in SRAM. "ctl" is the same sequence with the line disabled.
 * Cycles: entry = the handler's first CYCCNT less the caller's, body = its last less its first,
 * ret = the caller's after the return less the handler's last.
 */
#define X_IRQ SW0_IRQn
#define X_REPS 6U
#define VTOR_WORDS (16U + CONFIG_NUM_IRQS)
extern volatile uint32_t x_stamp[4];
extern uint32_t x_pend(volatile uint32_t *ispr, uint32_t bit, volatile uint32_t *stamps);
extern void x_isr(void);
static uint32_t sram_vectors[128] __aligned(512);
BUILD_ASSERT(VTOR_WORDS <= ARRAY_SIZE(sram_vectors));

typedef uint32_t (*pend_fn)(volatile uint32_t *ispr, uint32_t bit, volatile uint32_t *stamps);

static uintptr_t sram_alias(uintptr_t fn)
{
	return (uintptr_t)sram_code + ((fn & ~1U) - (uintptr_t)kern33_start) + 1U;
}

static void x_once(pend_fn pend, bool enabled, uint32_t out[4])
{
	unsigned int key = irq_lock();

	NVIC_ClearPendingIRQ(X_IRQ);
	if (enabled) {
		NVIC_EnableIRQ(X_IRQ);
	} else {
		NVIC_DisableIRQ(X_IRQ);
	}
	x_stamp[1] = 0U;
	x_stamp[2] = 0U;
	(void)pend(&NVIC->ISPR[X_IRQ / 32U], BIT(X_IRQ % 32U), x_stamp);
	NVIC_DisableIRQ(X_IRQ);
	NVIC_ClearPendingIRQ(X_IRQ);
	for (int i = 0; i < 4; i++) {
		out[i] = x_stamp[i];
	}
	irq_unlock(key);
}

static void x_case(const char *vectors, bool sram_code_too)
{
	pend_fn pend = (pend_fn)(sram_code_too ? sram_alias((uintptr_t)x_pend) : (uintptr_t)x_pend);
	const char *where = sram_code_too ? "sram" : "flash";
	uint32_t s[4], ctl = UINT32_MAX, entry = UINT32_MAX, body = UINT32_MAX, ret = UINT32_MAX;

	for (uint32_t r = 0; r < X_REPS; r++) {
		if (r == 0U) {
			icache_flush();
		}
		x_once(pend, true, s);
		if (r == 0U) {
			printk("X vectors %-5s code %-5s cold entry %4u body %4u ret %4u\n", vectors,
			       where, s[1] - s[0], s[2] - s[1], s[3] - s[2]);
		}
		entry = MIN(entry, s[1] - s[0]);
		body = MIN(body, s[2] - s[1]);
		ret = MIN(ret, s[3] - s[2]);
		x_once(pend, false, s);
		ctl = MIN(ctl, s[3] - s[0]);
	}
	printk("X vectors %-5s code %-5s warm entry %4u body %4u ret %4u ctl %4u\n", vectors,
	       where, entry, body, ret, ctl);
}

static void run_exceptions(void)
{
	uint32_t vtor = SCB->VTOR;

	IRQ_DIRECT_CONNECT(X_IRQ, 0, x_isr, IRQ_ZERO_LATENCY);
	printk("X VTOR 0x%08x CONTROL 0x%08x CCR 0x%08x\n", vtor, __get_CONTROL(), SCB->CCR);
	x_case("flash", false);
	memcpy(sram_vectors, (const void *)vtor, VTOR_WORDS * sizeof(uint32_t));
	SCB->VTOR = (uint32_t)sram_vectors;
	__DSB();
	__ISB();
	x_case("sram", false);
	sram_vectors[16U + X_IRQ] = sram_alias((uintptr_t)x_isr);
	__DSB();
	x_case("sram", true);
	SCB->VTOR = vtor;
	__DSB();
	__ISB();
}

/*
 * The M0+ kernels (src/kern0.c) on the radio sequencer core: held in RAC.CTRL.SEQRESET for
 * the copy into SEQRAM and released with CPUWAITDIS, as hal/radio.c's radio_reset() does.
 * While it runs this core only waits on its own clock: an M33 SEQRAM access during the run
 * would slow the M0+'s (7.74).
 */
#define RAC_EN 0xB8020004U
#define RAC_CTRL 0xB8020014U
#define RAC_CTRL_SEQRESET BIT(24)
#define RAC_CTRL_CPUWAITDIS BIT(26)
#define SEQRAM_M33 0xB0000000U
/* kern0.c's result layout and parameters. */
#define K0_RESULTS 0x3000U
#define K0_RESULTS_END 0x3710U
#define K0_RES_KERN 8U
#define K0_RES_X 0x200U
#define K0_DONE_MAGIC 0x4E44304BU
#define K0_N_LO 10U
#define K0_N_HI 110U
#define K0_XREPS 4U
#define K0_COUNT_MASK 0xFFFFFFU
/* 19 kernels x 2 N x 3 runs x at most 110 x 40 cycles is 52 ms at the slowest M0+ clock in
 * 7.75, 19.2 MHz (HFXO SYSCLK / 2); four times that. */
#define K0_RUN_US 200000U
extern const uint8_t kern0_image[];
extern const size_t kern0_image_size;

static const char *const k0_name[] = {
	"empty",     "nop",       "alu",       "str_a0",    "str_a2",    "str_a4",    "str_a6",
	"ldr_a0",    "ldr_a2",    "ldr_a4",    "ldr_a6",    "stralu_a0", "stralu_a2", "stralu_a4",
	"stralu_a6", "delay_a0",  "delay_a2",  "delay_a4",  "delay_a6",
};

static uint32_t seqram_rd(uint32_t off)
{
	return sys_read32(SEQRAM_M33 + off);
}

static void run_m0(void)
{
	size_t bytes = kern0_image_size;

	sys_write32(1U, RAC_EN);
	sys_write32(RAC_CTRL_SEQRESET, RAC_CTRL);
	__DSB();
	for (size_t i = 0; i < bytes; i += 4U) {
		sys_write32(sys_get_le32(&kern0_image[i]), SEQRAM_M33 + i);
	}
	for (uint32_t off = K0_RESULTS; off < K0_RESULTS_END; off += 4U) {
		sys_write32(0U, SEQRAM_M33 + off);
	}
	__DSB();
	sys_write32(RAC_CTRL_CPUWAITDIS, RAC_CTRL);
	k_busy_wait(K0_RUN_US);
	if (seqram_rd(K0_RESULTS) != K0_DONE_MAGIC) {
		printk("M0 not done: 0x%08x\n", seqram_rd(K0_RESULTS));
		sys_write32(RAC_CTRL_SEQRESET, RAC_CTRL);
		return;
	}

	uint32_t count = seqram_rd(K0_RESULTS + 4U);
	int32_t base = 0;

	printk("M0 image %u B, kernels %u\n", (unsigned int)bytes, count);
	for (uint32_t k = 0; k < count && k < ARRAY_SIZE(k0_name); k++) {
		uint32_t lo = seqram_rd(K0_RESULTS + K0_RES_KERN + 8U * k);
		uint32_t hi = seqram_rd(K0_RESULTS + K0_RES_KERN + 8U * k + 4U);
		int32_t it = (int32_t)((hi - lo) * 1000U / (K0_N_HI - K0_N_LO));

		if (k == 0U) {
			base = it;
		}
		printk("M %-10s lo %6u hi %6u iter %6d op %6d\n", k0_name[k], lo, hi, it,
		       (it - base) / 8);
	}
	for (uint32_t r = 0; r < 2U * K0_XREPS; r++) {
		uint32_t o = K0_RESULTS + K0_RES_X + 16U * r;
		uint32_t t0 = seqram_rd(o), e1 = seqram_rd(o + 4U), e2 = seqram_rd(o + 8U);
		uint32_t t3 = seqram_rd(o + 12U);

		if (r & 1U) {
			printk("MX enabled entry %4u body %4u ret %4u total %4u\n",
			       (t0 - e1) & K0_COUNT_MASK, (e1 - e2) & K0_COUNT_MASK,
			       (e2 - t3) & K0_COUNT_MASK, (t0 - t3) & K0_COUNT_MASK);
		} else {
			printk("MX ctl total %4u\n", (t0 - t3) & K0_COUNT_MASK);
		}
	}
	sys_write32(RAC_CTRL_SEQRESET, RAC_CTRL);
}

int main(void)
{
	/* Bus clocks of every target (the radio blocks, TIMER1, GPIO, the cache's registers). */
	CMU->CLKEN0 = _CMU_CLKEN0_MASK;
	CMU->CLKEN1 = _CMU_CLKEN1_MASK;
	CMU->RADIOCLKCTRL = CMU_RADIOCLKCTRL_EN;
	TIMER1->EN = TIMER_EN_EN;
	TIMER1->TOP = 0xFFFFU;
	TIMER1->CMD = TIMER_CMD_START;
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CYCCNT = 0;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

	__ASSERT_NO_MSG((size_t)(kern33_end - kern33_start) <= sizeof(sram_code));
	memcpy(sram_code, kern33_start, kern33_end - kern33_start);
	__DSB();
	__ISB();

	printk("cpu-timing-port: HCLK %u Hz, SYSCLKCTRL 0x%08x, MSC.READCTRL 0x%08x, "
	       "ICACHE.CTRL 0x%08x, kernels %u B\n",
	       SystemCoreClock, CMU->SYSCLKCTRL, MSC->READCTRL, ICACHE0->CTRL,
	       (unsigned int)(kern33_end - kern33_start));
	run_kernels();
	run_calls();
	run_icache();
	run_exceptions();
	run_m0();
	printk("cpu-timing-port: done\n");
	return 0;
}
