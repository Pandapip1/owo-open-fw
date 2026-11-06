/*
 * Cortex-M33 instruction and access timing (docs/radio-tables.md 7.75, 7.93), for the QEMU
 * model's per-instruction-class cycle counts. The same image runs on silicon and in QEMU and
 * prints one line per figure.
 *
 * Kernels (src/kern33.S) run with interrupts locked, from flash and from a copy in SRAM, at
 * N = 10 and N = 110 iterations; "iter" is the DWT.CYCCNT cost of one iteration in
 * millicycles, "op" that less the empty loop, divided by the 8 copies of the body. Then
 * library and firmware functions (cold call after an instruction-cache flush, then warm), and
 * straight-line blocks for the instruction cache's size and miss cost.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <string.h>
#include <soc.h>

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
	printk("cpu-timing-port: done\n");
	return 0;
}
