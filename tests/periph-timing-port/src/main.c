/*
 * IADC warm-up and conversion times and TRNG fill times (docs/radio-tables.md 7.89), for the
 * QEMU model's timing (vendor/qemu hw/arm/efr32bg22.c efr32_iadc_queue, hw/arm/efr32-trng.c).
 * The same image runs on silicon and in QEMU; both print one line per figure.
 *
 * Timebase: the DWT cycle counter (HCLK), calibrated against the system timer (LFXO) over
 * CAL_SECONDS. Each interval is a command write followed by polling a status register, so a
 * figure is the hardware time plus at most one poll period (printed as "poll").
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/drivers/adc.h>
#include <soc.h>

#define CAL_SECONDS 2U
#define REPS 32U
/* Longest wait for one event before it counts as missing: far above every figure measured. */
#define TIMEOUT_CYCLES (SystemCoreClock / 10U)

/* IADC settings: CLK_SRC_ADC = EM01GRPACLK / 2 (HSCLKRATE DIV2), as sl_hal_iadc_init() picks for
 * the 40 MHz limit from 76.8 MHz; TIMEBASE + 1 CLK_SRC_ADC cycles per microsecond; ADC_CLK =
 * CLK_SRC_ADC / (PRESCALE + 1), at most 10 MHz in normal mode.
 */
static uint32_t src_hsclkrate = _IADC_CTRL_HSCLKRATE_DIV2;
#define TIMEBASE_1US 38U
#define PRESCALE_10MHZ 3U

static uint32_t dwt_hz;

struct stat {
	uint32_t n, min, max;
	uint64_t sum;
};

static void stat_add(struct stat *s, uint32_t v)
{
	if (s->n == 0U || v < s->min) {
		s->min = v;
	}
	if (v > s->max) {
		s->max = v;
	}
	s->sum += v;
	s->n++;
}

static uint32_t to_ns(uint32_t cycles)
{
	return (uint32_t)((uint64_t)cycles * NSEC_PER_SEC / dwt_hz);
}

static void stat_print(const char *what, const struct stat *s)
{
	if (s->n == 0U) {
		printk("T %-28s n 0\n", what);
		return;
	}
	printk("T %-28s n %3u min %7u max %7u mean %7u ns (cycles %u..%u)\n", what, s->n,
	       to_ns(s->min), to_ns(s->max), to_ns((uint32_t)(s->sum / s->n)), s->min, s->max);
}

static void dwt_calibrate(void)
{
	uint32_t hz = sys_clock_hw_cycles_per_sec();
	uint32_t c0, c1, d0, d1;

	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CYCCNT = 0U;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
	c0 = k_cycle_get_32();
	while (k_cycle_get_32() == c0) {
	}
	c0 = k_cycle_get_32();
	d0 = DWT->CYCCNT;
	while (k_cycle_get_32() - c0 < CAL_SECONDS * hz) {
	}
	c1 = k_cycle_get_32();
	d1 = DWT->CYCCNT;
	dwt_hz = (uint32_t)((uint64_t)(d1 - d0) * hz / (c1 - c0));
	printk("CAL dwt %u Hz over %u system cycles at %u Hz (nominal cpu %u)\n", dwt_hz, c1 - c0,
	       hz, SystemCoreClock);
	printk("CMU EM01GRPACLKCTRL 0x%08x IADCCLKCTRL 0x%08x SYSCLKCTRL 0x%08x\n",
	       CMU->EM01GRPACLKCTRL, CMU->IADCCLKCTRL, CMU->SYSCLKCTRL);
}

/* Cycles from now until (*reg & mask) == want; 0 on timeout. */
static inline uint32_t wait_for(const volatile uint32_t *reg, uint32_t mask, uint32_t want, uint32_t t0)
{
	while ((*reg & mask) != want) {
		if (DWT->CYCCNT - t0 > TIMEOUT_CYCLES) {
			return 0U;
		}
	}
	return DWT->CYCCNT - t0;
}

/*
 * A start command at a different phase of the IADC's clocks each time: a spin of a varying
 * length (steps of DITHER_STEP CPU cycles, up to DITHER_SPAN), so a series samples the command's
 * synchronisation instead of repeating one phase.
 */
#define DITHER_STEP 7U
#define DITHER_SPAN 211U

static void dither(void)
{
	static uint32_t phase;
	uint32_t t0 = DWT->CYCCNT;

	phase = (phase + DITHER_STEP) % DITHER_SPAN;
	while (DWT->CYCCNT - t0 < phase) {
	}
}

/* ---- IADC, register level ---- */

static uint32_t iadc_input(uint32_t port, uint32_t pin)
{
	return (_IADC_SINGLE_PORTNEG_GND << _IADC_SINGLE_PORTNEG_SHIFT) |
	       (port << _IADC_SINGLE_PORTPOS_SHIFT) | (pin << _IADC_SINGLE_PINPOS_SHIFT);
}

#define IN_PD2 iadc_input(_IADC_SINGLE_PORTPOS_PORTD, 2U)
#define IN_PD3 iadc_input(_IADC_SINGLE_PORTPOS_PORTD, 3U)

static void iadc_config(uint32_t warmup, uint32_t timebase, uint32_t prescale, uint32_t osrhs)
{
	IADC0->EN_CLR = IADC_EN_EN;
	IADC0->CTRL = (warmup << _IADC_CTRL_WARMUPMODE_SHIFT) |
		      (timebase << _IADC_CTRL_TIMEBASE_SHIFT) |
		      (src_hsclkrate << _IADC_CTRL_HSCLKRATE_SHIFT);
	IADC0->CFG[0].CFG = (_IADC_CFG_RESETVALUE & ~(_IADC_CFG_OSRHS_MASK | _IADC_CFG_REFSEL_MASK)) |
			    (osrhs << _IADC_CFG_OSRHS_SHIFT) |
			    (_IADC_CFG_REFSEL_VDDX << _IADC_CFG_REFSEL_SHIFT);
	IADC0->CFG[0].SCHED = prescale << _IADC_SCHED_PRESCALE_SHIFT;
	IADC0->SINGLEFIFOCFG = _IADC_SINGLEFIFOCFG_RESETVALUE;
	IADC0->SCANFIFOCFG = _IADC_SCANFIFOCFG_RESETVALUE;
	/* The scan table takes writes only while the IADC is disabled (a write faults otherwise). */
	IADC0->SCANTABLE[0].SCAN = IN_PD2;
	IADC0->SCANTABLE[1].SCAN = IN_PD3;
	IADC0->SCANTABLE[2].SCAN = IN_PD2;
	IADC0->SCANTABLE[3].SCAN = IN_PD3;
	IADC0->EN_SET = IADC_EN_EN;
	IADC0->SINGLE = IN_PD2;
	IADC0->CMD = IADC_CMD_SINGLEFIFOFLUSH | IADC_CMD_SCANFIFOFLUSH;
	IADC0->IF_CLR = _IADC_IF_MASK;
}

/* One single conversion of @input: start to FIFO data valid; the result is popped. */
static uint32_t iadc_single(uint32_t input, uint32_t *warm_at)
{
	uint32_t t0, dt;

	IADC0->SINGLE = input;
	dither();
	t0 = DWT->CYCCNT;
	IADC0->CMD = IADC_CMD_SINGLESTART;
	if (warm_at != NULL) {
		*warm_at = wait_for(&IADC0->STATUS, IADC_STATUS_ADCWARM, IADC_STATUS_ADCWARM, t0);
	}
	dt = wait_for(&IADC0->STATUS, IADC_STATUS_SINGLEFIFODV, IADC_STATUS_SINGLEFIFODV, t0);
	(void)IADC0->SINGLEFIFODATA;
	return dt;
}

/* Poll period of the loop above (a status read and a DWT read). */
static void poll_period(void)
{
	struct stat s = {0};

	for (unsigned int i = 0; i < REPS; i++) {
		uint32_t a = DWT->CYCCNT;

		(void)IADC0->STATUS;
		stat_add(&s, DWT->CYCCNT - a);
	}
	stat_print("poll (IADC STATUS read)", &s);
}

/* Single conversions, each after a pause long enough for the IADC to power down or settle. */
static void iadc_series(const char *name, uint32_t warmup, uint32_t timebase, uint32_t prescale,
			uint32_t osrhs, bool alternate)
{
	struct stat first = {0}, rest = {0}, warm = {0};
	char label[40];

	iadc_config(warmup, timebase, prescale, osrhs);
	(void)IADC0->STATUS;
	for (unsigned int i = 0; i < REPS + 1U; i++) {
		uint32_t w = 0U;
		uint32_t dt = iadc_single(alternate && (i & 1U) ? IN_PD3 : IN_PD2,
					  warmup == _IADC_CTRL_WARMUPMODE_KEEPWARM ? NULL : &w);

		stat_add(i == 0U ? &first : &rest, dt);
		if (w != 0U) {
			stat_add(&warm, w);
		}
		k_busy_wait(100);
	}
	snprintk(label, sizeof(label), "%s first", name);
	stat_print(label, &first);
	snprintk(label, sizeof(label), "%s next", name);
	stat_print(label, &rest);
	if (warm.n != 0U) {
		snprintk(label, sizeof(label), "%s ADCWARM", name);
		stat_print(label, &warm);
	}
	printk("R %s CTRL 0x%08x CFG0 0x%08x SCHED0 0x%08x\n", name, IADC0->CTRL,
	       IADC0->CFG[0].CFG, IADC0->CFG[0].SCHED);
}

/* Scan of two entries (PD2, PD3), kept warm: start to the first and to both results. */
#define SCAN_MAX 4U

/* A scan of @entries table entries (PD2, PD3 alternating): start to each entry's result. */
static void iadc_scan(const char *name, uint32_t warmup, uint32_t osrhs, uint32_t entries)
{
	struct stat at[SCAN_MAX] = {0};
	char label[40];

	iadc_config(warmup, TIMEBASE_1US, PRESCALE_10MHZ, osrhs);
	IADC0->MASKREQ = BIT_MASK(entries);
	for (unsigned int i = 0; i < REPS + 1U; i++) {
		uint32_t t0, t[SCAN_MAX];

		dither();
		t0 = DWT->CYCCNT;
		IADC0->CMD = IADC_CMD_SCANSTART;
		for (uint32_t e = 0; e < entries; e++) {
			t[e] = wait_for(&IADC0->SCANFIFOSTAT, _IADC_SCANFIFOSTAT_FIFOREADCNT_MASK,
					e + 1U, t0);
		}
		for (uint32_t e = 0; e < entries; e++) {
			(void)IADC0->SCANFIFODATA;
			if (i > 0U) {
				stat_add(&at[e], t[e]);
			}
		}
		k_busy_wait(100);
	}
	for (uint32_t e = 0; e < entries; e++) {
		snprintk(label, sizeof(label), "%s entry %u", name, e + 1U);
		stat_print(label, &at[e]);
	}
}

static void iadc_raw(void)
{
	CMU->CLKEN0_SET = CMU_CLKEN0_IADC0 | CMU_CLKEN0_GPIO;
	GPIO->CDBUSALLOC = GPIO_CDBUSALLOC_CDEVEN0_ADC0 | GPIO_CDBUSALLOC_CDODD0_ADC0;
	iadc_config(_IADC_CTRL_WARMUPMODE_NORMAL, TIMEBASE_1US, PRESCALE_10MHZ,
		    _IADC_CFG_OSRHS_HISPD2);
	poll_period();
	iadc_series("normal tb38 osr2", _IADC_CTRL_WARMUPMODE_NORMAL, TIMEBASE_1US, PRESCALE_10MHZ,
		    _IADC_CFG_OSRHS_HISPD2, false);
	iadc_series("normal tb76 osr2", _IADC_CTRL_WARMUPMODE_NORMAL, 2U * TIMEBASE_1US + 1U,
		    PRESCALE_10MHZ, _IADC_CFG_OSRHS_HISPD2, false);
	iadc_series("standby tb38 osr2", _IADC_CTRL_WARMUPMODE_KEEPINSTANDBY, TIMEBASE_1US,
		    PRESCALE_10MHZ, _IADC_CFG_OSRHS_HISPD2, false);
	for (uint32_t osr = _IADC_CFG_OSRHS_HISPD2; osr <= _IADC_CFG_OSRHS_HISPD64; osr++) {
		char name[24];

		snprintk(name, sizeof(name), "warm osr%u", 2U << osr);
		iadc_series(name, _IADC_CTRL_WARMUPMODE_KEEPWARM, TIMEBASE_1US, PRESCALE_10MHZ, osr,
			    false);
	}
	iadc_series("warm osr2 presc7", _IADC_CTRL_WARMUPMODE_KEEPWARM, TIMEBASE_1US, 7U,
		    _IADC_CFG_OSRHS_HISPD2, false);
	iadc_series("warm osr2 alternate", _IADC_CTRL_WARMUPMODE_KEEPWARM, TIMEBASE_1US,
		    PRESCALE_10MHZ, _IADC_CFG_OSRHS_HISPD2, true);
	iadc_series("warm osr2 presc15", _IADC_CTRL_WARMUPMODE_KEEPWARM, TIMEBASE_1US, 15U,
		    _IADC_CFG_OSRHS_HISPD2, false);
	iadc_series("warm osr64 presc15", _IADC_CTRL_WARMUPMODE_KEEPWARM, TIMEBASE_1US, 15U,
		    _IADC_CFG_OSRHS_HISPD64, false);
	/* What of a warm conversion scales with TIMEBASE. */
	iadc_series("warm osr2 tb76", _IADC_CTRL_WARMUPMODE_KEEPWARM, 2U * TIMEBASE_1US + 1U,
		    PRESCALE_10MHZ, _IADC_CFG_OSRHS_HISPD2, false);
	iadc_series("warm osr2 tb19", _IADC_CTRL_WARMUPMODE_KEEPWARM, (TIMEBASE_1US + 1U) / 2U,
		    PRESCALE_10MHZ, _IADC_CFG_OSRHS_HISPD2, false);
	/* The same ADC_CLK (9.6 MHz) from CLK_SRC_ADC 19.2 MHz: what scales with CLK_SRC_ADC. */
	src_hsclkrate = _IADC_CTRL_HSCLKRATE_DIV4;
	iadc_series("div4 normal tb18 osr2", _IADC_CTRL_WARMUPMODE_NORMAL,
		    (TIMEBASE_1US + 1U) / 2U - 1U, 1U, _IADC_CFG_OSRHS_HISPD2, false);
	iadc_series("div4 warm osr2 presc1", _IADC_CTRL_WARMUPMODE_KEEPWARM,
		    (TIMEBASE_1US + 1U) / 2U - 1U, 1U, _IADC_CFG_OSRHS_HISPD2, false);
	iadc_series("div4 warm osr64 presc1", _IADC_CTRL_WARMUPMODE_KEEPWARM,
		    (TIMEBASE_1US + 1U) / 2U - 1U, 1U, _IADC_CFG_OSRHS_HISPD64, false);
	src_hsclkrate = _IADC_CTRL_HSCLKRATE_DIV2;
	src_hsclkrate = _IADC_CTRL_HSCLKRATE_DIV3;
	iadc_series("div3 warm osr2 presc2", _IADC_CTRL_WARMUPMODE_KEEPWARM, 25U, 2U,
		    _IADC_CFG_OSRHS_HISPD2, false);
	src_hsclkrate = _IADC_CTRL_HSCLKRATE_DIV2;
	iadc_series("warm osr2 presc5", _IADC_CTRL_WARMUPMODE_KEEPWARM, TIMEBASE_1US, 5U,
		    _IADC_CFG_OSRHS_HISPD2, false);
	iadc_series("warm osr2 presc11", _IADC_CTRL_WARMUPMODE_KEEPWARM, TIMEBASE_1US, 11U,
		    _IADC_CFG_OSRHS_HISPD2, false);
	iadc_series("warm osr8 presc7", _IADC_CTRL_WARMUPMODE_KEEPWARM, TIMEBASE_1US, 7U,
		    _IADC_CFG_OSRHS_HISPD8, false);
	iadc_scan("scan warm osr2", _IADC_CTRL_WARMUPMODE_KEEPWARM, _IADC_CFG_OSRHS_HISPD2, 4U);
	iadc_scan("scan warm osr8", _IADC_CTRL_WARMUPMODE_KEEPWARM, _IADC_CFG_OSRHS_HISPD8, 4U);
	iadc_scan("scan warm osr32", _IADC_CTRL_WARMUPMODE_KEEPWARM, _IADC_CFG_OSRHS_HISPD32, 2U);
	iadc_scan("scan normal osr2", _IADC_CTRL_WARMUPMODE_NORMAL, _IADC_CFG_OSRHS_HISPD2, 2U);
	IADC0->EN_CLR = IADC_EN_EN;
}

/* ---- IADC through Zephyr's driver, as the product reads it (owo_shim.c owo_adc_start) ---- */

static const struct adc_dt_spec sense = ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 0);
static volatile uint32_t done_at;

static enum adc_action done_cb(const struct device *dev, const struct adc_sequence *seq,
			       uint16_t index)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(seq);
	ARG_UNUSED(index);
	done_at = DWT->CYCCNT;
	return ADC_ACTION_FINISH;
}

static void iadc_driver(void)
{
	struct stat async = {0}, sync = {0};
	uint16_t buf;
	struct adc_sequence_options opts = {.callback = done_cb};
	struct adc_sequence seq = {
		.options = &opts,
		.channels = BIT(sense.channel_id),
		.buffer = &buf,
		.buffer_size = sizeof(buf),
		.resolution = sense.resolution,
		.oversampling = sense.oversampling,
	};

	if (adc_channel_setup_dt(&sense) != 0) {
		printk("adc setup failed\n");
		return;
	}
	for (unsigned int i = 0; i < REPS + 1U; i++) {
		uint32_t t0;
		int err;

		done_at = 0U;
		t0 = DWT->CYCCNT;
		err = adc_read_async(sense.dev, &seq, NULL);
		while (err == 0 && done_at == 0U && DWT->CYCCNT - t0 < TIMEOUT_CYCLES) {
		}
		if (err == 0 && done_at != 0U && i > 0U) {
			stat_add(&async, done_at - t0);
		}
		k_busy_wait(100);
		seq.options = NULL;
		t0 = DWT->CYCCNT;
		err = adc_read(sense.dev, &seq);
		if (err == 0 && i > 0U) {
			stat_add(&sync, DWT->CYCCNT - t0);
		}
		seq.options = &opts;
		k_busy_wait(100);
	}
	stat_print("driver read_async->callback", &async);
	stat_print("driver adc_read", &sync);
	printk("R driver CTRL 0x%08x CFG0 0x%08x SCHED0 0x%08x SCAN0 0x%08x\n", IADC0->CTRL,
	       IADC0->CFG[0].CFG, IADC0->CFG[0].SCHED, IADC0->SCANTABLE[0].SCAN);
}

/* ---- TRNG ---- */

#define TRNG CRYPTOACC_RNGCTRL
#define TRNG_FIFO_WORDS 64U

#define TRNG_TRACE 20U

/* Eight fills of the FIFO from ENABLE; the first fill's level changes are printed ("L"). */
static void trng_series(const char *name, uint32_t nblocks, uint32_t clkdiv, uint32_t wait,
			uint32_t extra)
{
	struct stat first = {0}, step = {0}, full = {0};
	uint32_t trace_t[TRNG_TRACE], trace_l[TRNG_TRACE], ntrace = 0U;
	char label[48];

	for (unsigned int r = 0; r < 8U; r++) {
		uint32_t t0, level = 0U, last = 0U;

		TRNG->RNGCTRL = CRYPTOACC_RNGCTRL_SOFTRESET;
		TRNG->RNGCTRL = 0U;
		TRNG->CLKDIV = clkdiv;
		TRNG->INITWAITVAL = wait;
		t0 = DWT->CYCCNT;
		TRNG->RNGCTRL = CRYPTOACC_RNGCTRL_ENABLE | extra |
				(nblocks << _CRYPTOACC_RNGCTRL_NB128BITBLOCKS_SHIFT);
		while (level < TRNG_FIFO_WORDS) {
			uint32_t now = TRNG->FIFOLEVEL;
			uint32_t t = DWT->CYCCNT - t0;

			if (t > 4U * TIMEOUT_CYCLES) {
				break;
			}
			if (now != level) {
				if (level == 0U) {
					stat_add(&first, t);
				} else {
					stat_add(&step, t - last);
				}
				if (r == 0U && ntrace < TRNG_TRACE) {
					trace_t[ntrace] = t;
					trace_l[ntrace++] = now;
				}
				level = now;
				last = t;
			}
		}
		if (level >= TRNG_FIFO_WORDS) {
			stat_add(&full, last);
		}
		if (r == 0U) {
			printk("R %s level %u status 0x%08x\n", name, level, TRNG->RNGSTATUS);
		}
	}
	TRNG->RNGCTRL = CRYPTOACC_RNGCTRL_SOFTRESET;
	TRNG->RNGCTRL = 0U;
	printk("L %s", name);
	for (uint32_t i = 0; i < ntrace; i++) {
		printk(" %u:%u", to_ns(trace_t[i]), trace_l[i]);
	}
	printk("\n");
	snprintk(label, sizeof(label), "%s first word", name);
	stat_print(label, &first);
	snprintk(label, sizeof(label), "%s level step", name);
	stat_print(label, &step);
	snprintk(label, sizeof(label), "%s full", name);
	stat_print(label, &full);
}

static void trng(void)
{
	uint32_t wait;

	CMU->CLKEN1_SET = CMU_CLKEN1_CRYPTOACC;
	wait = TRNG->INITWAITVAL;
	printk("R trng reset RNGCTRL 0x%08x INITWAITVAL 0x%08x CLKDIV 0x%08x FIFODEPTH %u\n",
	       TRNG->RNGCTRL, wait, TRNG->CLKDIV, TRNG->FIFODEPTH);
	trng_series("trng nb4 div0", 4U, 0U, wait, 0U);
	trng_series("trng nb1 div0", 1U, 0U, wait, 0U);
	trng_series("trng nb8 div0", 8U, 0U, wait, 0U);
	trng_series("trng nb4 div1", 4U, 1U, wait, 0U);
	trng_series("trng nb4 div3", 4U, 3U, wait, 0U);
	trng_series("trng nb4 div0 bypnist", 4U, 0U, wait, CRYPTOACC_RNGCTRL_BYPNIST);
	trng_series("trng nb4 div1 bypnist", 4U, 1U, wait, CRYPTOACC_RNGCTRL_BYPNIST);
	trng_series("trng nb4 div0 wait16", 4U, 0U, 0x10U, 0U);
	trng_series("trng nb4 div0 wait256", 4U, 0U, 0x100U, 0U);
	trng_series("trng nb4 div0 wait4096", 4U, 0U, 0x1000U, 0U);
	trng_series("trng nb4 div0 wait16384", 4U, 0U, 0x4000U, 0U);
	trng_series("trng nb4 div1 wait16", 4U, 1U, 0x10U, 0U);
	trng_series("trng nb4 div0 wait16 bypnist", 4U, 0U, 0x10U, CRYPTOACC_RNGCTRL_BYPNIST);
	TRNG->INITWAITVAL = wait;
}

int main(void)
{
	dwt_calibrate();
	iadc_driver();
	iadc_raw();
	trng();
	printk("periph-timing: done\n");
	return 0;
}
