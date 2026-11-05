/*
 * Synthesizer frequency-switch probe (docs/radio-tables.md 7.90). Receive only, except
 * FSWITCH_PROBE_TX (E15: in-band advertising on 2402 MHz).
 *
 * A passive scanner with a 10.24 s window brings the radio to RXSEARCH on 2402 MHz through the
 * open sequencer (RXWARM, docs/radio-tables.md 7.80). The M33 then locks its interrupts, so the
 * link layer never ends the window, and measures the synthesizer directly: the LO counter
 * (SYNTH.LOCNT*), frequency steps with and without VCO calibration, the open-loop tuning curve,
 * the frequency range, and the demodulator's frequency-offset estimate on frames from nearby
 * advertisers. Without FSWITCH_PROBE_TX nothing transmits: the RAC never leaves the receive
 * states. The image ends in a loop; reset it to run again. Builds: prj.conf on the BGM220 kit
 * (bgm220_ek4314a/bgm220pc22hna), prj.conf + prj-qemu.conf on the QEMU board (bg22_ek4108a).
 *
 * Register map: Silicon Labs' public xG22 register model (SYNTH_S/RAC_S/MODEM_S _register.py and
 * _field.py, Simplicity SDK v2025.6.1), M33 non-secure aliases; set/clear aliases at +0x1000 and
 * +0x2000. Timebase: DWT.CYCCNT at SYSCLK, HFRCODPLL 76.8 MHz (docs/radio-tables.md 7.74).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/bluetooth/bluetooth.h>

#define SET 0x1000U
#define CLR 0x2000U

#define SYNTH 0xB8018000UL
#define SYNTH_STATUS (SYNTH + 0x008U)
#define SYNTH_CMD (SYNTH + 0x00CU)
#define SYNTH_CTRL (SYNTH + 0x010U)
#define SYNTH_CALCTRL (SYNTH + 0x014U)
#define SYNTH_VCDACCTRL (SYNTH + 0x02CU)
#define SYNTH_FREQ (SYNTH + 0x034U)
#define SYNTH_IFFREQ (SYNTH + 0x038U)
#define SYNTH_DIVCTRL (SYNTH + 0x03CU)
#define SYNTH_CHCTRL (SYNTH + 0x040U)
#define SYNTH_CHSP (SYNTH + 0x044U)
#define SYNTH_CALOFFSET (SYNTH + 0x048U)
#define SYNTH_VCOTUNING (SYNTH + 0x04CU)
#define SYNTH_VCORANGE (SYNTH + 0x054U)
#define SYNTH_VCOGAIN (SYNTH + 0x058U)
#define SYNTH_CHPDACCTRL (SYNTH + 0x068U)
#define SYNTH_IF (SYNTH + 0x078U)
#define SYNTH_LOCNTCTRL (SYNTH + 0x088U)
#define SYNTH_LOCNTSTATUS (SYNTH + 0x08CU)
#define SYNTH_LOCNTTARGET (SYNTH + 0x090U)

#define STATUS_INLOCK BIT(0)
#define STATUS_CAPCALERROR BIT(14)
#define CMD_SYNTHSTART BIT(0)
#define CMD_SYNTHSTOP BIT(1)
#define CMD_ENABLEIF BIT(2)
#define CMD_CAPCALSTART BIT(4)
#define VCDACCTRL_VCDACEN BIT(6)
#define VCDACCTRL_LPFEN BIT(7)
#define VCDACCTRL_LPFQSEN BIT(8)
#define VCOTUNING_CODE_MASK 0x7FFU
#define VCOTUNING_VCAPSEL_SHIFT 11U
#define VCOTUNING_VCAPSEL_MASK (0x1FU << VCOTUNING_VCAPSEL_SHIFT)
#define VCOTUNING_TOP_BIT 10
#define VCORANGE_LOAD BIT(11)
#define VCORANGE_THROUGH BIT(18)
#define IF_ALL 0x237U
#define LOCNT_ENABLE BIT(0)
#define LOCNT_CLEAR BIT(1)
#define LOCNT_RUN BIT(2)
#define LOCNT_READ BIT(3)
#define LOCNT_NUMCYCLE_SHIFT 4U
#define LOCNT_NUMCYCLE_MASK (0xFU << LOCNT_NUMCYCLE_SHIFT)
#define LOCNT_FCALRUNCLKEN BIT(11)
#define LOCNT_COUNT_MASK 0x7FFFFU
#define LOCNT_BUSY BIT(19)
/* Closed loop the counter counts only with FCALRUNCLKEN (E1); NUMCYCLE saturates at 12. */
#define CL LOCNT_FCALRUNCLKEN
#define LOCNT_NUMCYCLE_MAX 12U
#define SYNTH_LPFCTRL1RX (SYNTH + 0x0A0U)
#define SYNTH_LPFCTRL1TX (SYNTH + 0x0A4U)
#define SYNTH_LPFCTRL2RX (SYNTH + 0x0A8U)
#define SYNTH_LPFCTRL2TX (SYNTH + 0x0ACU)
#define SYNTH_DSMCTRLRX (SYNTH + 0x0B0U)
#define SYNTH_DSMCTRLTX (SYNTH + 0x0B4U)

#define RAC 0xB8020000UL
#define RAC_STATUS (RAC + 0x00CU)
#define RAC_SYNTHENCTRL (RAC + 0x094U)
#define RAC_SYTRIM1 (RAC + 0x154U)
#define RAC_STATE_SHIFT 24U
#define RAC_STATE_MASK 0xFU
#define RAC_STATE_RXSEARCH 2U
#define RAC_STATE_RXFRAME 3U
#define SYNTHENCTRL_VCOSTARTUP BIT(1)
#define SYNTHENCTRL_LPFBWSEL BIT(20)
#define RAC_SYTRIM0 (RAC + 0x150U)
#define RAC_SYEN (RAC + 0x15CU)
#define SYEN_SYCHPLPEN BIT(1)
/* sequencer/radio_seq.S EFR32_RAC_SYTRIM0_TX. */
#define SYTRIM0_TX ((5U << 3) | (7U << 9) | (1U << 13) | (1U << 14) | (3U << 17))
/* SYTRIM1.SYLODIVLDOTRIMNDIO (bits 5:2): 3 while the synthesizer starts, 1 running
 * (sequencer/radio_seq.S, EFR32_RAC_SYTRIM1_STARTUP/RUNNING). */
#define SYTRIM1_NDIO_MASK (0xFU << 2)
#define SYTRIM1_NDIO_STARTUP (3U << 2)

#define MODEM 0xB8014000UL
#define MODEM_FREQOFFEST (MODEM + 0x010U)
#define FREQOFFEST_MASK 0x1FFFU
#define FREQOFFEST_SIGN 0x1000U
#define MODEM_DIGMIXCTRL (MODEM + 0x10CU)
#define DIGMIXFREQ_MASK 0x000FFFFFU

#define FRC 0xB8004000UL
#define FRC_IF (FRC + 0x068U)
#define FRC_IF_RXDONE BIT(4)
#define BUFC_RX 0xBA00004CUL
#define BUFC_RX_ADDR (BUFC_RX + 0x004U)
#define BUFC_RX_WRITEOFFSET (BUFC_RX + 0x008U)
#define BUFC_RX_READOFFSET (BUFC_RX + 0x00CU)

#define DEMCR 0xE000EDFCUL
#define DEMCR_TRCENA BIT(24)
#define DWT_CTRL 0xE0001000UL
#define DWT_CYCCNT 0xE0001004UL

/* SYSCLK (HFRCODPLL), the DWT rate; SYNTH.FREQ unit f_HFXO / 2^19 (hal/radio.c). */
#define CPU_HZ 76800000U
#define HFXO_HZ 38400000ULL
#define FREQ_FRACTION_BITS 19U
#define HZ_PER_KHZ 1000ULL
#define CH37_KHZ 2402000U

#define CYC_PER_US (CPU_HZ / 1000000U)
/* Bounds on every hardware poll, in reads. */
#define BUSY_RISE_POLLS 16U
#define BUSY_FALL_POLLS 400000U

static inline uint32_t rd(uintptr_t a)
{
	return *(volatile uint32_t *)a;
}

static inline void wr(uintptr_t a, uint32_t v)
{
	*(volatile uint32_t *)a = v;
}

static inline uint32_t cyc(void)
{
	return rd(DWT_CYCCNT);
}

/* Frames from nearby advertisers keep arriving: consume them so the 512-octet RX FIFO never
 * overflows (the RAC would stop in RXOVERFLOW, docs/radio-tables.md 7.80). */
static uint32_t rac_state(void)
{
	return (rd(RAC_STATUS) >> RAC_STATE_SHIFT) & RAC_STATE_MASK;
}

#define BUFC_RX_CMD (BUFC_RX + 0x028U)
#define BUFC_CMD_CLEAR BIT(0)
#define BUFC_IF 0xBA000114UL
#define RAC_CTRL (RAC + 0x014U)
#define RAC_CTRL_FORCEDISABLE BIT(0)
#define RAC_STATE_OFF 0U
#define RAC_STATE_RXOVERFLOW 6U
#define REARM_POLLS 100000U

static uint32_t overflows;

/* RXOVERFLOW leaves only through FORCEDISABLE (7.80): off, clear the FIFO, release; SWRXEN still
 * asserted, the RAC warms up again (the sequencer's RXWARM recalibrates at SYNTH.FREQ). */
static void rearm(void)
{
	uint32_t k;

	if (rac_state() != RAC_STATE_RXOVERFLOW) {
		return;
	}
	if (overflows++ == 0U) {
		printk("OVF write=%u read=%u bufc_if=0x%08x frc_if=0x%08x\n", rd(BUFC_RX_WRITEOFFSET),
		       rd(BUFC_RX_READOFFSET), rd(BUFC_IF), rd(FRC_IF));
	}
	wr(RAC_CTRL + SET, RAC_CTRL_FORCEDISABLE);
	for (k = 0U; k < REARM_POLLS && rac_state() != RAC_STATE_OFF; k++) {
	}
	wr(BUFC_RX_CMD, BUFC_CMD_CLEAR);
	wr(BUFC_IF + CLR, UINT32_MAX);
	/* The sequencer's calibration expects the LO counter idle: with FCALRUNCLKEN left
	 * set, the next RXWARM left VCOTUNING at 0x0780 and the LO unlocked near 2236 MHz. */
	wr(SYNTH_LOCNTCTRL, 0U);
	wr(RAC_CTRL + CLR, RAC_CTRL_FORCEDISABLE);
	for (k = 0U; k < REARM_POLLS && rac_state() != RAC_STATE_RXSEARCH; k++) {
	}
}

/* Frames from nearby advertisers keep arriving: empty the FIFO whenever no frame is being
 * received, so it never overflows (the RAC would stop in RXOVERFLOW, docs/radio-tables.md 7.80). */
static void drain(void)
{
	if (rac_state() == RAC_STATE_RXSEARCH) {
		wr(BUFC_RX_CMD, BUFC_CMD_CLEAR);
		wr(FRC_IF + CLR, FRC_IF_RXDONE);
	} else {
		rearm();
	}
}

static void spin_raw(uint32_t n)
{
	uint32_t t = cyc();

	while (cyc() - t < n) {
	}
}

static void spin_cyc(uint32_t n)
{
	uint32_t t = cyc();

	drain();
	while (cyc() - t < n) {
	}
}

static uint32_t word_of_hz(uint64_t hz)
{
	return (uint32_t)(((hz << FREQ_FRACTION_BITS) + HFXO_HZ / 2U) / HFXO_HZ);
}

static int32_t dword_of_hz(int64_t hz)
{
	int64_t num = hz * (1LL << FREQ_FRACTION_BITS);

	return (int32_t)((num >= 0 ? num + (int64_t)HFXO_HZ / 2 : num - (int64_t)HFXO_HZ / 2) /
			 (int64_t)HFXO_HZ);
}

/* One LO count: LOCNTSTATUS (count, BUSY clear) after a measurement of 2^(n+1) HFXO periods.
 * *busy_seen: BUSY was read set at least once after RUN. */
static uint32_t locnt(uint32_t n, uint32_t extra, uint32_t *duration, bool *busy_seen)
{
	uint32_t s = 0U;
	uint32_t t0;
	uint32_t k;
	bool seen = false;

	wr(SYNTH_LOCNTCTRL, LOCNT_ENABLE | LOCNT_READ | extra | (n << LOCNT_NUMCYCLE_SHIFT));
	wr(SYNTH_LOCNTCTRL + SET, LOCNT_CLEAR);
	t0 = cyc();
	wr(SYNTH_LOCNTCTRL + SET, LOCNT_RUN);
	for (k = 0U; k < BUSY_RISE_POLLS; k++) {
		s = rd(SYNTH_LOCNTSTATUS);
		if (s & LOCNT_BUSY) {
			seen = true;
			break;
		}
	}
	for (k = 0U; (s & LOCNT_BUSY) && k < BUSY_FALL_POLLS; k++) {
		s = rd(SYNTH_LOCNTSTATUS);
	}
	if (duration != NULL) {
		*duration = cyc() - t0;
	}
	if (busy_seen != NULL) {
		*busy_seen = seen;
	}
	return s;
}

/* The sequencer's successive-approximation VCO calibration (efr32_seq_vco_calibrate), on the M33.
 * Requires the calibration state (efr32_seq_synth_lock up to LOCNTCTRL.ENABLE). */
static const uint8_t sar_low_bits[VCOTUNING_TOP_BIT + 1] = {0, 0, 0, 1, 1, 2, 4, 8, 16, 32, 32};
static const uint8_t sar_numcycle[VCOTUNING_TOP_BIT + 1] = {5, 5, 5, 4, 4, 4, 4, 3, 2, 2, 2};

static uint32_t sar(uint32_t vcapsel_bits)
{
	uint32_t acc = vcapsel_bits;

	wr(SYNTH_LOCNTCTRL + SET, LOCNT_READ);
	for (int b = VCOTUNING_TOP_BIT; b >= 0; b--) {
		uint32_t trial = acc | BIT(b);
		uint32_t s;

		wr(SYNTH_VCOTUNING, trial | sar_low_bits[b]);
		wr(SYNTH_LOCNTCTRL + CLR, LOCNT_NUMCYCLE_MASK);
		wr(SYNTH_LOCNTCTRL + SET, ((uint32_t)sar_numcycle[b] << LOCNT_NUMCYCLE_SHIFT) |
						  LOCNT_CLEAR);
		wr(SYNTH_LOCNTCTRL + SET, LOCNT_RUN);
		s = rd(SYNTH_LOCNTSTATUS);
		for (uint32_t k = 0U; k < BUSY_RISE_POLLS && !(s & LOCNT_BUSY); k++) {
			s = rd(SYNTH_LOCNTSTATUS);
		}
		for (uint32_t k = 0U; (s & LOCNT_BUSY) && k < BUSY_FALL_POLLS; k++) {
			s = rd(SYNTH_LOCNTSTATUS);
		}
		if ((s & LOCNT_COUNT_MASK) > rd(SYNTH_LOCNTTARGET)) {
			acc = trial;
		}
	}
	wr(SYNTH_LOCNTCTRL + CLR, LOCNT_READ);
	wr(SYNTH_VCOTUNING, acc);
	return acc;
}

/* The sequencer's delay units (efr32_seq_delay) taken as microseconds. */
#define SEQ_DELAY_VCDAC_US 4U
#define SEQ_DELAY_START_US 2U

struct retune_times {
	uint32_t to_cal;   /* entry to calibration state */
	uint32_t cal;      /* SAR (or cached code write) */
	uint32_t to_start; /* entry to SYNTHSTART */
	uint32_t total;    /* entry to the end of the sequence */
};

/* The synthesizer half of the open TX->RX retune (efr32_seq_rx_tune + efr32_seq_synth_lock),
 * on the M33, for a running receiver: stop, new FREQ, open-loop calibration (SAR, or @p code
 * when @p cal is false), start. Returns VCOTUNING. */
static uint32_t retune(uint32_t word, bool cal, uint32_t code, struct retune_times *t)
{
	uint32_t t0 = cyc();
	uint32_t sytrim1 = rd(RAC_SYTRIM1);
	uint32_t t1;
	uint32_t t2;
	uint32_t t3;

	wr(SYNTH_CMD, CMD_SYNTHSTOP | CMD_ENABLEIF);
	wr(SYNTH_FREQ, word);
	wr(RAC_SYTRIM1, (sytrim1 & ~SYTRIM1_NDIO_MASK) | SYTRIM1_NDIO_STARTUP);
	wr(SYNTH_VCDACCTRL + SET, VCDACCTRL_VCDACEN | VCDACCTRL_LPFEN | VCDACCTRL_LPFQSEN);
	spin_raw(SEQ_DELAY_VCDAC_US * CYC_PER_US);
	wr(RAC_SYNTHENCTRL + CLR, SYNTHENCTRL_VCOSTARTUP);
	wr(SYNTH_VCORANGE + SET, VCORANGE_LOAD);
	wr(SYNTH_VCORANGE + SET, VCORANGE_THROUGH);
	wr(SYNTH_CMD, CMD_CAPCALSTART);
	wr(SYNTH_LOCNTCTRL + SET, LOCNT_ENABLE);
	t1 = cyc();
	if (cal) {
		code = sar(code & VCOTUNING_VCAPSEL_MASK);
	} else {
		wr(SYNTH_VCOTUNING, code);
	}
	t2 = cyc();
	wr(SYNTH_CMD, CMD_SYNTHSTART);
	wr(SYNTH_LOCNTCTRL, 0U);
	spin_raw(SEQ_DELAY_START_US * CYC_PER_US);
	wr(SYNTH_VCDACCTRL + CLR, VCDACCTRL_VCDACEN);
	wr(SYNTH_VCDACCTRL + CLR, VCDACCTRL_LPFQSEN);
	spin_raw(SEQ_DELAY_START_US * CYC_PER_US);
	wr(SYNTH_VCORANGE + CLR, VCORANGE_LOAD | VCORANGE_THROUGH);
	wr(RAC_SYTRIM1, sytrim1);
	t3 = cyc();
	if (t != NULL) {
		t->to_cal = t1 - t0;
		t->cal = t2 - t1;
		t->to_start = t2 - t0;
		t->total = t3 - t0;
	}
	return code;
}

static uint32_t f0_word;
static uint32_t f0_tuning;

static void dump(const char *tag)
{
	printk("%s rac=%u FREQ=0x%08x IFFREQ=0x%08x CHCTRL=%u CHSP=%u CALOFFSET=0x%x "
	       "VCOTUNING=0x%04x STATUS=0x%08x IF=0x%03x CTRL=0x%08x CALCTRL=0x%08x\n",
	       tag, rac_state(), rd(SYNTH_FREQ), rd(SYNTH_IFFREQ), rd(SYNTH_CHCTRL), rd(SYNTH_CHSP),
	       rd(SYNTH_CALOFFSET), rd(SYNTH_VCOTUNING), rd(SYNTH_STATUS), rd(SYNTH_IF),
	       rd(SYNTH_CTRL), rd(SYNTH_CALCTRL));
	printk("%s VCDACCTRL=0x%08x DIVCTRL=0x%08x VCORANGE=0x%08x VCOGAIN=0x%08x CHPDACCTRL=0x%08x "
	       "LOCNTCTRL=0x%08x SYTRIM1=0x%08x SYNTHENCTRL=0x%08x\n",
	       tag, rd(SYNTH_VCDACCTRL), rd(SYNTH_DIVCTRL), rd(SYNTH_VCORANGE), rd(SYNTH_VCOGAIN),
	       rd(SYNTH_CHPDACCTRL), rd(SYNTH_LOCNTCTRL), rd(RAC_SYTRIM1), rd(RAC_SYNTHENCTRL));
}

/* Back to the scan carrier, calibrated, and checked with a long LO count. */
static void restore(const char *tag)
{
	uint32_t d;
	uint32_t code = retune(f0_word, true, f0_tuning, NULL);
	uint32_t s;

	spin_cyc(500U * CYC_PER_US);
	s = locnt(12U, CL, &d, NULL);
	wr(SYNTH_LOCNTCTRL, 0U);
	printk("%s restore code=0x%04x (was 0x%04x) count12=%u target=%u rac=%u overflows=%u\n", tag, code,
	       f0_tuning, s & LOCNT_COUNT_MASK, rd(SYNTH_LOCNTTARGET), rac_state(), overflows);
}

/* E1: what the LO counter counts in RXSEARCH (closed loop), for every NUMCYCLE. */
static void e1_locnt(void)
{
	static const uint32_t extras[] = {0U, LOCNT_FCALRUNCLKEN};

	for (uint32_t x = 0U; x < ARRAY_SIZE(extras); x++) {
		for (uint32_t n = 0U; n <= 15U; n++) {
			for (uint32_t r = 0U; r < 3U; r++) {
				uint32_t d;
				bool seen;
				uint32_t s = locnt(n, extras[x], &d, &seen);

				printk("E1 x=%u n=%u count=%u busy=%u seen=%u target=%u cyc=%u\n", x,
				       n, s & LOCNT_COUNT_MASK, !!(s & LOCNT_BUSY), seen,
				       rd(SYNTH_LOCNTTARGET), d);
			}
		}
	}
	wr(SYNTH_LOCNTCTRL, 0U);
	printk("E1 rac=%u\n", rac_state());
}

/*
 * E2: closed-loop step of SYNTH.FREQ by @p hz, no recalibration. Back-to-back LO counts of
 * NUMCYCLE @p n from the write on, averaged over the repetitions, up and down.
 */
#define E2_REPS 16U
#define E2_WINDOWS_MAX 64U
#define E2_SETTLE_US 400U

static uint32_t e2_sum[2][E2_WINDOWS_MAX];
static uint32_t e2_tsum[2][E2_WINDOWS_MAX];

static void e2_step(int32_t hz, uint32_t n, uint32_t windows)
{
	int32_t dw = dword_of_hz(hz);
	uint32_t flags[2] = {0U, 0U};
	uint32_t settled[2];
	uint32_t targets[2];

	memset(e2_sum, 0, sizeof(e2_sum));
	memset(e2_tsum, 0, sizeof(e2_tsum));
	for (uint32_t r = 0U; r < E2_REPS; r++) {
		for (uint32_t dir = 0U; dir < 2U; dir++) {
			uint32_t t0;

			spin_cyc(E2_SETTLE_US * CYC_PER_US);
			wr(SYNTH_IF + CLR, IF_ALL);
			t0 = cyc();
			wr(SYNTH_FREQ, dir == 0U ? f0_word + (uint32_t)dw : f0_word);
			for (uint32_t k = 0U; k < windows; k++) {
				uint32_t ts = cyc() - t0;
				uint32_t s = locnt(n, CL, NULL, NULL);

				e2_tsum[dir][k] += ts;
				e2_sum[dir][k] += s & LOCNT_COUNT_MASK;
				drain();
			}
			flags[dir] |= rd(SYNTH_IF);
		}
	}
	for (uint32_t dir = 0U; dir < 2U; dir++) {
		wr(SYNTH_FREQ, dir == 0U ? f0_word + (uint32_t)dw : f0_word);
		spin_cyc(E2_SETTLE_US * CYC_PER_US);
		settled[dir] = locnt(12U, CL, NULL, NULL) & LOCNT_COUNT_MASK;
		targets[dir] = rd(SYNTH_LOCNTTARGET);
	}
	wr(SYNTH_LOCNTCTRL, 0U);
	printk("E2 step hz=%d dw=%d n=%u reps=%u up_if=0x%03x down_if=0x%03x settled12 up=%u/%u "
	       "down=%u/%u status=0x%08x rac=%u\n",
	       hz, dw, n, E2_REPS, flags[0], flags[1], settled[0], targets[0], settled[1],
	       targets[1], rd(SYNTH_STATUS), rac_state());
	for (uint32_t k = 0U; k < windows; k++) {
		printk("E2 k=%u t_up=%u c_up=%u t_dn=%u c_dn=%u\n", k, e2_tsum[0][k] / E2_REPS,
		       e2_sum[0][k], e2_tsum[1][k] / E2_REPS, e2_sum[1][k]);
	}
}

static void e2_steps(void)
{
	static const int32_t steps[] = {1000000, 4000000, 8000000, -4000000, 100000, 400000};

	for (uint32_t i = 0U; i < ARRAY_SIZE(steps); i++) {
		e2_step(steps[i], 5U, 48U);
	}
	e2_step(8000000, 4U, 64U);
	e2_step(16000000, 4U, 64U);
	e2_step(4000000, 8U, 32U);
	e2_step(16000000, 8U, 32U);
	e2_step(-16000000, 8U, 32U);
}

/* E3: open-loop VCO frequency against the tuning code (calibration state, VCDAC driving the
 * control voltage), LO count of NUMCYCLE 8. */
static void e3_curve(void)
{
	static const uint32_t vcapsels[] = {0U, 8U, 16U, 24U, 31U};
	uint32_t sytrim1 = rd(RAC_SYTRIM1);

	wr(SYNTH_CMD, CMD_SYNTHSTOP | CMD_ENABLEIF);
	wr(RAC_SYTRIM1, (sytrim1 & ~SYTRIM1_NDIO_MASK) | SYTRIM1_NDIO_STARTUP);
	wr(SYNTH_VCDACCTRL + SET, VCDACCTRL_VCDACEN | VCDACCTRL_LPFEN | VCDACCTRL_LPFQSEN);
	spin_raw(SEQ_DELAY_VCDAC_US * CYC_PER_US);
	wr(RAC_SYNTHENCTRL + CLR, SYNTHENCTRL_VCOSTARTUP);
	wr(SYNTH_VCORANGE + SET, VCORANGE_LOAD);
	wr(SYNTH_VCORANGE + SET, VCORANGE_THROUGH);
	wr(SYNTH_CMD, CMD_CAPCALSTART);
	wr(SYNTH_LOCNTCTRL + SET, LOCNT_ENABLE);
	for (uint32_t v = 0U; v < ARRAY_SIZE(vcapsels); v++) {
		for (uint32_t code = 0U; code <= VCOTUNING_CODE_MASK + 1U; code += 64U) {
			uint32_t c = MIN(code, VCOTUNING_CODE_MASK);
			uint32_t s;

			if (vcapsels[v] != 16U && c != 0U && c != 1024U && c != VCOTUNING_CODE_MASK) {
				continue;
			}
			wr(SYNTH_VCOTUNING, (vcapsels[v] << VCOTUNING_VCAPSEL_SHIFT) | c);
			spin_cyc(20U * CYC_PER_US);
			s = locnt(8U, 0U, NULL, NULL);
			printk("E3 vcapsel=%u code=%u count8=%u target=%u\n", vcapsels[v], c,
			       s & LOCNT_COUNT_MASK, rd(SYNTH_LOCNTTARGET));
		}
	}
	wr(SYNTH_LOCNTCTRL, 0U);
	wr(SYNTH_VCOTUNING, f0_tuning);
	wr(SYNTH_CMD, CMD_SYNTHSTART);
	spin_raw(SEQ_DELAY_START_US * CYC_PER_US);
	wr(SYNTH_VCDACCTRL + CLR, VCDACCTRL_VCDACEN);
	wr(SYNTH_VCDACCTRL + CLR, VCDACCTRL_LPFQSEN);
	spin_raw(SEQ_DELAY_START_US * CYC_PER_US);
	wr(SYNTH_VCORANGE + CLR, VCORANGE_LOAD | VCORANGE_THROUGH);
	wr(RAC_SYTRIM1, sytrim1);
	restore("E3");
}

/* E4: retune with calibration (SAR) or a cached code, by @p hz and back; timing of the
 * sequence, then the settling: LO counts of NUMCYCLE 5 from the sequence's end. */
#define E4_WINDOWS 32U
#define E4_REPS 8U

static void e4_retune_one(int32_t hz, bool cal)
{
	static uint32_t sum[E4_WINDOWS];
	static uint32_t tsum[E4_WINDOWS];
	uint32_t word = f0_word + (uint32_t)dword_of_hz(hz);
	struct retune_times t;
	struct retune_times tmax = {0};
	struct retune_times tmin = {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
	uint32_t code = 0U;
	uint32_t cached;
	uint32_t s12;

	/* The cached code: a calibration at the destination first. */
	cached = retune(word, true, f0_tuning, NULL);
	spin_cyc(E2_SETTLE_US * CYC_PER_US);
	(void)retune(f0_word, true, f0_tuning, NULL);
	memset(sum, 0, sizeof(sum));
	memset(tsum, 0, sizeof(tsum));
	for (uint32_t r = 0U; r < E4_REPS; r++) {
		uint32_t t0;

		spin_cyc(E2_SETTLE_US * CYC_PER_US);
		code = retune(word, cal, cal ? f0_tuning : cached, &t);
		t0 = cyc();
		for (uint32_t k = 0U; k < E4_WINDOWS; k++) {
			uint32_t ts = cyc() - t0;

			sum[k] += locnt(5U, CL, NULL, NULL) & LOCNT_COUNT_MASK;
			tsum[k] += ts;
		}
		tmax.to_cal = MAX(tmax.to_cal, t.to_cal);
		tmax.cal = MAX(tmax.cal, t.cal);
		tmax.to_start = MAX(tmax.to_start, t.to_start);
		tmax.total = MAX(tmax.total, t.total);
		tmin.to_cal = MIN(tmin.to_cal, t.to_cal);
		tmin.cal = MIN(tmin.cal, t.cal);
		tmin.to_start = MIN(tmin.to_start, t.to_start);
		tmin.total = MIN(tmin.total, t.total);
		spin_cyc(E2_SETTLE_US * CYC_PER_US);
		(void)retune(f0_word, true, f0_tuning, NULL);
	}
	(void)retune(word, cal, cal ? f0_tuning : cached, NULL);
	spin_cyc(E2_SETTLE_US * CYC_PER_US);
	s12 = locnt(12U, CL, NULL, NULL);
	printk("E4 hz=%d cal=%u code=0x%04x cached=0x%04x f0code=0x%04x to_cal=%u..%u cal=%u..%u "
	       "to_start=%u..%u total=%u..%u count12=%u target=%u status=0x%08x rac=%u\n",
	       hz, cal, code, cached, f0_tuning, tmin.to_cal, tmax.to_cal, tmin.cal, tmax.cal,
	       tmin.to_start, tmax.to_start, tmin.total, tmax.total, s12 & LOCNT_COUNT_MASK,
	       rd(SYNTH_LOCNTTARGET), rd(SYNTH_STATUS), rac_state());
	for (uint32_t k = 0U; k < E4_WINDOWS; k++) {
		printk("E4 k=%u t=%u c=%u\n", k, tsum[k] / E4_REPS, sum[k]);
	}
	wr(SYNTH_LOCNTCTRL, 0U);
	restore("E4");
}

static void e4_retunes(void)
{
	static const int32_t steps[] = {1000000, 2000000, 10000000, 24000000, 78000000};

	for (uint32_t i = 0U; i < ARRAY_SIZE(steps); i++) {
		e4_retune_one(steps[i], true);
		e4_retune_one(steps[i], false);
	}
}

/*
 * E5: the frequency range. At each carrier: calibration from VCAPSEL @p vcapsel (the sequencer
 * always uses 16), the loop closed, then the settled LO count against the programmed LO
 * (count12 = f_LO x 2^13 / f_HFXO = (FREQ + IFFREQ) / 2^6 with LOSIDE high).
 */
#define IFFREQ_MASK 0xFFFFFU
#define COUNT12_SHIFT (FREQ_FRACTION_BITS - (LOCNT_NUMCYCLE_MAX + 1U))

static void e5_sweep(uint32_t low_khz, uint32_t high_khz, uint32_t step_khz)
{
	static const uint32_t vcapsels[] = {16U, 0U, 31U};
	uint32_t iff = rd(SYNTH_IFFREQ) & IFFREQ_MASK;

	for (uint32_t v = 0U; v < ARRAY_SIZE(vcapsels); v++) {
		for (uint32_t khz = low_khz; khz <= high_khz; khz += step_khz) {
			uint32_t word = word_of_hz((uint64_t)khz * HZ_PER_KHZ);
			uint32_t code;
			uint32_t s;
			uint32_t status;

			wr(SYNTH_IF + CLR, IF_ALL);
			code = retune(word, true, vcapsels[v] << VCOTUNING_VCAPSEL_SHIFT, NULL);
			status = rd(SYNTH_STATUS);
			spin_cyc(E2_SETTLE_US * CYC_PER_US);
			s = locnt(LOCNT_NUMCYCLE_MAX, CL, NULL, NULL);
			printk("E5 v=%u khz=%u code=%u calstatus=0x%05x count12=%u expect=%u if=0x%03x "
			       "status=0x%05x rac=%u\n",
			       vcapsels[v], khz, code & VCOTUNING_CODE_MASK, status,
			       s & LOCNT_COUNT_MASK, (word + iff) >> COUNT12_SHIFT, rd(SYNTH_IF),
			       rd(SYNTH_STATUS), rac_state());
			wr(SYNTH_LOCNTCTRL, 0U);
		}
	}
}

static void e5_range(void)
{
	e5_sweep(CONFIG_FSWITCH_PROBE_RANGE_LOW_KHZ, CONFIG_FSWITCH_PROBE_RANGE_HIGH_KHZ,
		 CONFIG_FSWITCH_PROBE_RANGE_STEP_KHZ);
	restore("E5");
}

/* E13: the two edges of the range at a finer step, and E2 at NUMCYCLE 3 next to E11. */
#define EDGE_LOW_FROM_KHZ 2120000U
#define EDGE_LOW_TO_KHZ 2200000U
#define EDGE_HIGH_FROM_KHZ 2580000U
#define EDGE_HIGH_TO_KHZ 2720000U
#define EDGE_STEP_KHZ 2000U

static void e13_edges(void)
{
	e5_sweep(EDGE_LOW_FROM_KHZ, EDGE_LOW_TO_KHZ, EDGE_STEP_KHZ);
	e5_sweep(EDGE_HIGH_FROM_KHZ, EDGE_HIGH_TO_KHZ, EDGE_STEP_KHZ);
	restore("E13");
}


/*
 * E6: the demodulator's frequency-offset estimate (MODEM.FREQOFFEST, 13-bit two's complement)
 * of frames from nearby advertisers on 2402 MHz, with the LO moved by known offsets (SYNTH.FREQ,
 * no recalibration). One line per received frame.
 */
#define E6_DWELL_MS 1500U
#define FIFO_BYTES 512U
#define TRAILER_CRCOK BIT(7)
#define ADV_HEADER_OCTETS 2U
#define ADVA_OCTETS 6U

#define MODEM_TIMDETSTATUS (MODEM + 0x00CU)
#define TIMDET_FREQOFFEST_SHIFT 8U
#define BYTE_MASK 0xFFU
#define BYTE_SIGN 0x80U

static int32_t sext13(uint32_t v)
{
	int32_t x = (int32_t)(v & FREQOFFEST_MASK);

	return (x & FREQOFFEST_SIGN) ? x - (int32_t)(FREQOFFEST_SIGN << 1) : x;
}

static int32_t sext8(uint32_t v)
{
	int32_t x = (int32_t)(v & BYTE_MASK);

	return (x & BYTE_SIGN) ? x - (int32_t)(BYTE_SIGN << 1) : x;
}

/*
 * Polls for @p dwell_ms. While a frame is being received (RAC in RXFRAME) MODEM.FREQOFFEST and
 * TIMDETSTATUS are sampled (a read at RXDONE mostly returns 0: the estimate is live, not
 * latched); at RXDONE the frame's AdvA, CRC result and the last nonzero samples are printed.
 */
static void e6_dwell(int32_t tag, uint32_t dwell_ms)
{
	uint32_t t0 = cyc();
	uint32_t foe = 0U;
	uint32_t timdet = 0U;
	uint32_t samples = 0U;

	while (cyc() - t0 < dwell_ms * (CPU_HZ / 1000U)) {
		rearm();
		if (rac_state() == RAC_STATE_RXFRAME) {
			uint32_t v = rd(MODEM_FREQOFFEST);
			uint32_t t = rd(MODEM_TIMDETSTATUS);

			samples++;
			if (v & FREQOFFEST_MASK) {
				foe = v;
			}
			if ((t >> TIMDET_FREQOFFEST_SHIFT) & BYTE_MASK) {
				timdet = t;
			}
		}
		if (rd(FRC_IF) & FRC_IF_RXDONE) {
			const volatile uint8_t *fifo = (const volatile uint8_t *)rd(BUFC_RX_ADDR);
			uint32_t r = rd(BUFC_RX_READOFFSET) % FIFO_BYTES;
			uint32_t len = fifo[(r + 1U) % FIFO_BYTES];

			if (r + ADV_HEADER_OCTETS + len + 1U <= FIFO_BYTES && len >= ADVA_OCTETS) {
				const volatile uint8_t *p = &fifo[r];

				printk("E6 hz=%d foe=%d timdet=%d n=%u crc=%u len=%u pdu=%x "
				       "a=%02x%02x%02x%02x%02x%02x\n",
				       tag, sext13(foe), sext8(timdet >> TIMDET_FREQOFFEST_SHIFT),
				       samples, !!(p[ADV_HEADER_OCTETS + len] & TRAILER_CRCOK), len,
				       p[0] & 0xFU, p[7], p[6], p[5], p[4], p[3], p[2]);
			}
			foe = 0U;
			timdet = 0U;
			samples = 0U;
			drain();
		}
	}
}

static void e6_foe(void)
{
	static const int32_t offsets[] = {0,     50000,  -50000, 100000, -100000, 150000, -150000,
					  25000, -25000, 200000, -200000, 300000, -300000, 0};

	for (uint32_t i = 0U; i < ARRAY_SIZE(offsets); i++) {
		wr(SYNTH_FREQ, f0_word + (uint32_t)dword_of_hz(offsets[i]));
		spin_cyc(E2_SETTLE_US * CYC_PER_US);
		e6_dwell(offsets[i], E6_DWELL_MS);
		printk("E6 end hz=%d rac=%u\n", offsets[i], rac_state());
	}
	wr(SYNTH_FREQ, f0_word);

	/*
	 * The same offsets applied by the digital mixer instead of the LO: MODEM.DIGMIXCTRL.
	 * DIGMIXFREQ in units of f_ADC / 2^20, f_ADC = f_LO / 2^9 on the 1M PHY (7.83). Tagged
	 * offset + 10^9.
	 */
	uint32_t dmx = rd(MODEM_DIGMIXCTRL);
	uint64_t lo_word = (uint64_t)f0_word + (rd(SYNTH_IFFREQ) & 0xFFFFFU);
	int64_t adc_hz = (int64_t)((lo_word * HFXO_HZ) >> (FREQ_FRACTION_BITS + 9U));

	for (uint32_t i = 0U; i < ARRAY_SIZE(offsets); i++) {
		int32_t dd = (int32_t)((int64_t)offsets[i] * (1LL << 20) / adc_hz);

		wr(MODEM_DIGMIXCTRL, (dmx & ~DIGMIXFREQ_MASK) |
					     (((dmx & DIGMIXFREQ_MASK) + (uint32_t)dd) & DIGMIXFREQ_MASK));
		spin_cyc(E2_SETTLE_US * CYC_PER_US);
		e6_dwell(offsets[i] + 1000000000, E6_DWELL_MS);
		printk("E6 dmx end hz=%d dd=%d rac=%u\n", offsets[i], dd, rac_state());
	}
	wr(MODEM_DIGMIXCTRL, dmx);
}

/* E7: other ways to move the carrier: CHCTRL.CHNO x CHSP, CALOFFSET, VCOADDCAP/VCOSUBCAP. */
#define CMD_VCOADDCAP BIT(9)
#define CMD_VCOSUBCAP BIT(10)

static uint32_t count12(void)
{
	spin_cyc(E2_SETTLE_US * CYC_PER_US);
	return locnt(LOCNT_NUMCYCLE_MAX, CL, NULL, NULL) & LOCNT_COUNT_MASK;
}

static void e7_other(void)
{
	static const uint32_t caloffsets[] = {0x10U, 0x100U, 0x1000U, 0x4000U, 0x7FFFU};
	uint32_t base = count12();
	uint32_t chsp = (uint32_t)dword_of_hz(1000000);

	printk("E7 base count12=%u\n", base);
	wr(SYNTH_CHSP, chsp);
	printk("E7 chsp=%u chno=0 count12=%u\n", chsp, count12());
	for (uint32_t ch = 1U; ch <= 3U; ch++) {
		wr(SYNTH_CHCTRL, ch);
		printk("E7 chsp=%u chno=%u count12=%u\n", chsp, ch, count12());
	}
	wr(SYNTH_CHCTRL, 0U);
	wr(SYNTH_CHSP, 0U);
	for (uint32_t i = 0U; i < ARRAY_SIZE(caloffsets); i++) {
		wr(SYNTH_CALOFFSET, caloffsets[i]);
		printk("E7 caloffset=0x%x count12=%u readback=0x%x\n", caloffsets[i], count12(),
		       rd(SYNTH_CALOFFSET));
	}
	wr(SYNTH_CALOFFSET, 0U);
	printk("E7 caloffset=0 count12=%u\n", count12());
	for (uint32_t i = 0U; i < 4U; i++) {
		wr(SYNTH_CMD, CMD_VCOADDCAP);
		printk("E7 addcap %u vcotuning=0x%04x count12=%u if=0x%03x\n", i + 1U,
		       rd(SYNTH_VCOTUNING), count12(), rd(SYNTH_IF));
	}
	for (uint32_t i = 0U; i < 8U; i++) {
		wr(SYNTH_CMD, CMD_VCOSUBCAP);
		printk("E7 subcap %u vcotuning=0x%04x count12=%u if=0x%03x\n", i + 1U,
		       rd(SYNTH_VCOTUNING), count12(), rd(SYNTH_IF));
	}
	/* The tuning code written directly while the loop is closed. */
	for (int32_t d = -64; d <= 64; d += 32) {
		wr(SYNTH_VCOTUNING, (uint32_t)((int32_t)f0_tuning + d));
		printk("E7 vcotuning=0x%04x (f0 %+d) count12=%u\n", rd(SYNTH_VCOTUNING), d, count12());
	}
	wr(SYNTH_LOCNTCTRL, 0U);
	restore("E7");
}

/*
 * E9: the step response of E2 with the loop configured as the sequencer configures it for TX
 * (loop filter words, LPFBWSEL, charge-pump trims), applied while receiving.
 */
/*
 * E10: what LOCNTTARGET follows. In the calibration state (as the sequencer's SAR runs it, the
 * NUMCYCLE field rewritten through the clear/set aliases before each RUN) and closed loop, for
 * each NUMCYCLE: target before and after RUN, and the count.
 */
static void e10_target_one(const char *tag, bool open)
{
	for (uint32_t n = 0U; n <= LOCNT_NUMCYCLE_MAX + 1U; n++) {
		uint32_t before;
		uint32_t s;

		wr(SYNTH_LOCNTCTRL + CLR, LOCNT_NUMCYCLE_MASK);
		wr(SYNTH_LOCNTCTRL + SET, (n << LOCNT_NUMCYCLE_SHIFT) | LOCNT_CLEAR |
						  (open ? 0U : CL) | LOCNT_ENABLE | LOCNT_READ);
		before = rd(SYNTH_LOCNTTARGET);
		wr(SYNTH_LOCNTCTRL + SET, LOCNT_RUN);
		s = rd(SYNTH_LOCNTSTATUS);
		for (uint32_t k = 0U; k < BUSY_RISE_POLLS && !(s & LOCNT_BUSY); k++) {
			s = rd(SYNTH_LOCNTSTATUS);
		}
		for (uint32_t k = 0U; (s & LOCNT_BUSY) && k < BUSY_FALL_POLLS; k++) {
			s = rd(SYNTH_LOCNTSTATUS);
		}
		printk("E10 %s n=%u target_before=%u target_after=%u count=%u ctrl=0x%04x\n", tag, n,
		       before, rd(SYNTH_LOCNTTARGET), s & LOCNT_COUNT_MASK, rd(SYNTH_LOCNTCTRL));
	}
}

static void e10_target(void)
{
	uint32_t sytrim1 = rd(RAC_SYTRIM1);

	e10_target_one("closed", false);
	wr(SYNTH_LOCNTCTRL, 0U);
	wr(SYNTH_CMD, CMD_SYNTHSTOP | CMD_ENABLEIF);
	wr(RAC_SYTRIM1, (sytrim1 & ~SYTRIM1_NDIO_MASK) | SYTRIM1_NDIO_STARTUP);
	wr(SYNTH_VCDACCTRL + SET, VCDACCTRL_VCDACEN | VCDACCTRL_LPFEN | VCDACCTRL_LPFQSEN);
	spin_raw(SEQ_DELAY_VCDAC_US * CYC_PER_US);
	wr(SYNTH_VCORANGE + SET, VCORANGE_LOAD);
	wr(SYNTH_VCORANGE + SET, VCORANGE_THROUGH);
	printk("E10 before CAPCALSTART target=%u\n", rd(SYNTH_LOCNTTARGET));
	wr(SYNTH_CMD, CMD_CAPCALSTART);
	printk("E10 after CAPCALSTART target=%u\n", rd(SYNTH_LOCNTTARGET));
	wr(SYNTH_LOCNTCTRL + SET, LOCNT_ENABLE);
	e10_target_one("cal", true);
	/* The carrier changed in the calibration state: does the target follow FREQ? */
	wr(SYNTH_FREQ, f0_word + (uint32_t)dword_of_hz(10000000));
	printk("E10 cal FREQ+10MHz target=%u\n", rd(SYNTH_LOCNTTARGET));
	e10_target_one("cal+10M", true);
	wr(SYNTH_FREQ, f0_word);
	wr(SYNTH_LOCNTCTRL, 0U);
	wr(RAC_SYTRIM1, sytrim1);
	restore("E10");
}

/*
 * E11: the first microseconds after a SYNTH.FREQ step at fine time resolution: one LO count of
 * NUMCYCLE @p n started a staggered delay after the write, averaged over E11_REPS, up and down.
 * The printed delay runs from the FREQ write to the entry of locnt() (RUN follows two register
 * writes later).
 */
#define E11_REPS 64U
#define E11_DELAYS 40U
#define E11_DELAY_STEP_CYC 8U
#define E11_SETTLE_US 400U

static void e11_one(int32_t hz, uint32_t n)
{
	int32_t dw = dword_of_hz(hz);

	printk("E11 step hz=%d n=%u reps=%u\n", hz, n, E11_REPS);
	for (uint32_t d = 0U; d < E11_DELAYS; d++) {
		uint32_t sum[2] = {0U, 0U};
		uint32_t tsum[2] = {0U, 0U};

		for (uint32_t r = 0U; r < E11_REPS; r++) {
			for (uint32_t dir = 0U; dir < 2U; dir++) {
				uint32_t t0;
				uint32_t ts;

				spin_cyc(E11_SETTLE_US * CYC_PER_US);
				t0 = cyc();
				wr(SYNTH_FREQ, dir == 0U ? f0_word + (uint32_t)dw : f0_word);
				while (cyc() - t0 < d * E11_DELAY_STEP_CYC) {
				}
				ts = cyc() - t0;
				sum[dir] += locnt(n, CL, NULL, NULL) & LOCNT_COUNT_MASK;
				tsum[dir] += ts;
			}
		}
		printk("E11 d=%u t_up=%u c_up=%u t_dn=%u c_dn=%u\n", d, tsum[0] / E11_REPS, sum[0],
		       tsum[1] / E11_REPS, sum[1]);
	}
	wr(SYNTH_FREQ, f0_word);
	wr(SYNTH_LOCNTCTRL, 0U);
}

static void e11_early(void)
{
	/* Control: the same word written again. */
	e11_one(0, 3U);
	e11_one(1000000, 3U);
	e11_one(4000000, 3U);
	e11_one(250000, 5U);
}

/*
 * E12: a stepped chirp written from the M33 as a LoRa transmitter or de-chirping receiver would:
 * two up-chirps of bandwidth @p bw_hz and spreading factor @p sf, @p k steps per symbol, around
 * the scan carrier (f0 - BW/2 .. f0 + BW/2, then the wrap). One LO count of NUMCYCLE @p n
 * started at step s0 of each repetition; the count against the programmed staircase gives the
 * mean frequency error over the window. Printed: the run time and the step schedule's lateness.
 */
#define E12_MAX_STEPS 512U
#define E12_REPS 32U
#define E12_WINDOWS 16U
#define E12_START_SETTLE_US 50U

static uint32_t e12_words[E12_MAX_STEPS];

static void e12_chirp_run(uint32_t bw_hz, uint32_t sf, uint32_t k, uint32_t n, bool ref)
{
	uint32_t steps = 2U * k;
	uint32_t sym_cyc = (uint32_t)(((uint64_t)CPU_HZ << sf) / bw_hz);
	uint32_t step_cyc = sym_cyc / k;
	uint32_t stride = MAX(steps / E12_WINDOWS, 1U);

	if (steps > E12_MAX_STEPS) {
		return;
	}
	for (uint32_t i = 0U; i < steps; i++) {
		int64_t hz = -(int64_t)bw_hz / 2 + (int64_t)(i % k) * bw_hz / k;

		e12_words[i] = f0_word + (uint32_t)dword_of_hz(hz);
		if (ref) {
			e12_words[i] = e12_words[0];
		}
	}
	printk("E12 cfg ref=%u bw=%u sf=%u k=%u n=%u step_cyc=%u sym_cyc=%u w0=%u dw=%d\n", ref, bw_hz, sf, k,
	       n, step_cyc, sym_cyc, e12_words[0], (int32_t)(e12_words[1] - e12_words[0]));
	for (uint32_t s0 = 0U; s0 < steps; s0 += stride) {
		uint32_t sum = 0U;
		uint32_t tsum = 0U;
		uint32_t late_max = 0U;

		for (uint32_t r = 0U; r < E12_REPS; r++) {
			uint32_t t0;
			uint32_t s;

			spin_cyc(E11_SETTLE_US * CYC_PER_US);
			wr(SYNTH_FREQ, e12_words[0]);
			spin_cyc(E12_START_SETTLE_US * CYC_PER_US);
			wr(SYNTH_LOCNTCTRL, LOCNT_ENABLE | LOCNT_READ | CL | (n << LOCNT_NUMCYCLE_SHIFT));
			wr(SYNTH_LOCNTCTRL + SET, LOCNT_CLEAR);
			t0 = cyc();
			for (uint32_t i = 0U; i < steps; i++) {
				uint32_t due = i * step_cyc;
				uint32_t now;

				while ((now = cyc() - t0) < due) {
				}
				wr(SYNTH_FREQ, e12_words[i]);
				late_max = MAX(late_max, now - due);
				if (i == s0) {
					tsum += cyc() - t0;
					wr(SYNTH_LOCNTCTRL + SET, LOCNT_RUN);
				}
			}
			while (cyc() - t0 < steps * step_cyc) {
			}
			s = rd(SYNTH_LOCNTSTATUS);
			for (uint32_t q = 0U; (s & LOCNT_BUSY) && q < BUSY_FALL_POLLS; q++) {
				s = rd(SYNTH_LOCNTSTATUS);
			}
			sum += s & LOCNT_COUNT_MASK;
		}
		printk("E12 s0=%u t_run=%u sum=%u late_max=%u\n", s0, tsum / E12_REPS, sum, late_max);
		drain();
	}
	wr(SYNTH_FREQ, f0_word);
	wr(SYNTH_LOCNTCTRL, 0U);
}

/* Each chirp after a reference pass: the same windows with the LO held at the first step, so the
 * LO counter's own bias at this timing cancels. */
static void e12_chirp(uint32_t bw_hz, uint32_t sf, uint32_t k, uint32_t n)
{
	e12_chirp_run(bw_hz, sf, k, n, true);
	e12_chirp_run(bw_hz, sf, k, n, false);
}

static void e12_chirps(void)
{
	e12_chirp(812500U, 7U, 128U, 8U);
	e12_chirp(812500U, 7U, 32U, 8U);
	e12_chirp(812500U, 7U, 16U, 8U);
	e12_chirp(1625000U, 7U, 128U, 7U);
	e12_chirp(1625000U, 7U, 32U, 7U);
	e12_chirp(406250U, 7U, 128U, 8U);
	e12_chirp(203125U, 7U, 128U, 9U);
	e12_chirp(812500U, 8U, 256U, 8U);
	/* Scaled up: 12.8 MHz over 160 us (about the SF7 / 812.5 kHz symbol), so the tracking error
	 * resolves in the LO count. */
	e12_chirp(12800000U, 11U, 128U, 5U);
}

/* Runs @p body with the loop configured as the sequencer configures it for TX. */
static void tx_loop(const char *tag, void (*body)(void))
{
	uint32_t saved[6] = {rd(SYNTH_LPFCTRL1RX), rd(SYNTH_LPFCTRL2RX), rd(RAC_SYNTHENCTRL),
			     rd(RAC_SYEN), rd(RAC_SYTRIM0), rd(SYNTH_DSMCTRLRX)};

	wr(SYNTH_LPFCTRL1RX, rd(SYNTH_LPFCTRL1TX));
	wr(SYNTH_LPFCTRL2RX, rd(SYNTH_LPFCTRL2TX));
	wr(SYNTH_DSMCTRLRX, rd(SYNTH_DSMCTRLTX));
	wr(RAC_SYNTHENCTRL + SET, SYNTHENCTRL_LPFBWSEL);
	wr(RAC_SYEN + CLR, SYEN_SYCHPLPEN);
	wr(RAC_SYTRIM0, SYTRIM0_TX);
	printk("%s tx-loop lpf1=0x%08x lpf2=0x%08x dsm=0x%08x (rx 0x%08x 0x%08x 0x%08x)\n", tag,
	       rd(SYNTH_LPFCTRL1RX), rd(SYNTH_LPFCTRL2RX), rd(SYNTH_DSMCTRLRX), saved[0], saved[1],
	       saved[5]);
	body();
	wr(SYNTH_LPFCTRL1RX, saved[0]);
	wr(SYNTH_LPFCTRL2RX, saved[1]);
	wr(RAC_SYNTHENCTRL, saved[2]);
	wr(RAC_SYEN, saved[3]);
	wr(RAC_SYTRIM0, saved[4]);
	wr(SYNTH_DSMCTRLRX, saved[5]);
	printk("%s tx-loop end\n", tag);
}

static void e9_body(void)
{
	e2_step(4000000, 5U, 48U);
	e2_step(16000000, 4U, 64U);
	e11_early();
	e12_chirp(812500U, 7U, 128U, 8U);
	e12_chirp(12800000U, 11U, 128U, 5U);
}

static void e9_tx_loop(void)
{
	tx_loop("E9", e9_body);
	restore("E9");
}

/*
 * E14: E2 (back-to-back windows) and E11 (staggered single windows) at NUMCYCLE 3 on the same
 * steps, receive loop then TX loop, the receiver tuned to 2399 MHz where no Bluetooth frame
 * arrives to fill the FIFO (a re-arm would recalibrate mid-measurement).
 */
#define E14_KHZ 2399000U

static void e14_body(void)
{
	e2_step(0, 3U, 64U);
	e2_step(1000000, 3U, 64U);
	e2_step(4000000, 3U, 64U);
	e2_step(4000000, 5U, 48U);
	e11_one(0, 3U);
	e11_one(1000000, 3U);
	e11_one(4000000, 3U);
	e11_one(-4000000, 3U);
	e12_chirp(812500U, 7U, 128U, 8U);
	e12_chirp(1625000U, 7U, 128U, 7U);
}

static void e14_crosscheck(void)
{
	uint32_t saved = f0_word;

	f0_word = word_of_hz((uint64_t)E14_KHZ * HZ_PER_KHZ);
	f0_tuning = retune(f0_word, true, f0_tuning, NULL);
	spin_cyc(E2_SETTLE_US * CYC_PER_US);
	printk("E14 rx-loop\n");
	e14_body();
	tx_loop("E14", e14_body);
	f0_word = saved;
	restore("E14");
}

/*
 * E16: large SYNTH.FREQ steps without recalibration, followed for about 2 ms: one repetition,
 * back-to-back LO counts of NUMCYCLE 10 (53.3 us), up then down, at 2399 MHz.
 */
#define E16_WINDOWS 40U
#define E16_N 10U
#define E16_SETTLE_US 3000U

static uint32_t e16_t[E16_WINDOWS];
static uint32_t e16_c[E16_WINDOWS];

static void e16_one(int32_t hz)
{
	int32_t dw = dword_of_hz(hz);

	for (uint32_t dir = 0U; dir < 2U; dir++) {
		uint32_t t0;

		spin_cyc(E16_SETTLE_US * CYC_PER_US);
		wr(SYNTH_IF + CLR, IF_ALL);
		t0 = cyc();
		wr(SYNTH_FREQ, dir == 0U ? f0_word + (uint32_t)dw : f0_word);
		for (uint32_t k = 0U; k < E16_WINDOWS; k++) {
			uint32_t ts = cyc() - t0;
			uint32_t s = locnt(E16_N, CL, NULL, NULL);

			e16_t[k] = ts;
			e16_c[k] = s & LOCNT_COUNT_MASK;
		}
		for (uint32_t k = 0U; k < E16_WINDOWS; k++) {
			printk("E16 hz=%d dir=%u k=%u t=%u c=%u\n", hz, dir, k, e16_t[k], e16_c[k]);
		}
		printk("E16 hz=%d dir=%u if=0x%03x status=0x%05x\n", hz, dir, rd(SYNTH_IF),
		       rd(SYNTH_STATUS));
	}
}

static void e16_large(void)
{
	static const int32_t steps[] = {8000000,   10000000,  12000000,  16000000, 20000000,
					24000000,  -8000000,  -10000000, -12000000, -16000000,
					-20000000, -24000000};
	uint32_t saved = f0_word;

	f0_word = word_of_hz((uint64_t)E14_KHZ * HZ_PER_KHZ);
	f0_tuning = retune(f0_word, true, f0_tuning, NULL);
	printk("E16 f0=0x%08x code=0x%04x\n", f0_word, f0_tuning);
	for (uint32_t i = 0U; i < ARRAY_SIZE(steps); i++) {
		e16_one(steps[i]);
	}
	wr(SYNTH_LOCNTCTRL, 0U);
	f0_word = saved;
	restore("E16");
}

/*
 * E15 (FSWITCH_PROBE_TX): SYNTH.FREQ steps while the RAC transmits. Non-connectable advertising
 * on channel 37 only (2402 MHz, normal Bluetooth power, in the 2.4 GHz ISM band); in each PDU
 * the M33, interrupts locked for well under the PDU's airtime, steps the carrier by
 * E15_STEP_HZ (to 2402.5 MHz, still in the band) and back, with LO counts of NUMCYCLE 5 and
 * SYNTH.STATUS.INLOCK sampled after each step.
 */
#define RAC_STATE_TX 9U
#define E15_REPS 40U
#define E15_WINDOWS 10U
#define E15_STEP_HZ 500000
#define E15_WAIT_MS 2000U
#define E15_ADV_INTERVAL 0x00A0
#define E15_MFG_OCTETS 29U

static uint32_t e15_sum[3][E15_WINDOWS];
static uint32_t e15_tsum[3][E15_WINDOWS];
static uint32_t e15_lock[3][E15_WINDOWS];

static void e15_tx(void)
{
	static const uint8_t mfg[E15_MFG_OCTETS] = {0xFF, 0xFF};
	const struct bt_data ad[] = {
		BT_DATA(BT_DATA_MANUFACTURER_DATA, mfg, sizeof(mfg)),
	};
	const struct bt_le_adv_param param = BT_LE_ADV_PARAM_INIT(
		BT_LE_ADV_OPT_USE_IDENTITY | BT_LE_ADV_OPT_DISABLE_CHAN_38 |
			BT_LE_ADV_OPT_DISABLE_CHAN_39,
		E15_ADV_INTERVAL, E15_ADV_INTERVAL, NULL);
	uint32_t done = 0U;
	uint32_t still_tx = 0U;
	uint32_t word = 0U;
	int err = bt_le_adv_start(&param, ad, ARRAY_SIZE(ad), NULL, 0);

	printk("E15 adv %d\n", err);
	if (err != 0) {
		return;
	}
	while (done < E15_REPS) {
		uint32_t t0 = k_uptime_get_32();
		uint32_t key;

		while (rac_state() != RAC_STATE_TX && k_uptime_get_32() - t0 < E15_WAIT_MS) {
		}
		key = irq_lock();
		if (rac_state() == RAC_STATE_TX) {
			word = rd(SYNTH_FREQ);
			for (uint32_t phase = 0U; phase < 3U; phase++) {
				uint32_t ts0 = cyc();

				if (phase == 1U) {
					wr(SYNTH_FREQ, word + (uint32_t)dword_of_hz(E15_STEP_HZ));
				} else if (phase == 2U) {
					wr(SYNTH_FREQ, word);
				}
				for (uint32_t k = 0U; k < E15_WINDOWS; k++) {
					uint32_t ts = cyc() - ts0;

					e15_sum[phase][k] += locnt(5U, CL, NULL, NULL) & LOCNT_COUNT_MASK;
					e15_lock[phase][k] += rd(SYNTH_STATUS) & STATUS_INLOCK;
					e15_tsum[phase][k] += ts;
				}
			}
			wr(SYNTH_LOCNTCTRL, 0U);
			still_tx += rac_state() == RAC_STATE_TX;
			done++;
		}
		irq_unlock(key);
		k_msleep(1);
	}
	(void)bt_le_adv_stop();
	printk("E15 reps=%u still_tx=%u freq=0x%08x step_hz=%d dw=%d\n", done, still_tx, word,
	       E15_STEP_HZ, dword_of_hz(E15_STEP_HZ));
	for (uint32_t phase = 0U; phase < 3U; phase++) {
		for (uint32_t k = 0U; k < E15_WINDOWS; k++) {
			printk("E15 phase=%u k=%u t=%u sum=%u inlock=%u\n", phase, k,
			       e15_tsum[phase][k] / E15_REPS, e15_sum[phase][k], e15_lock[phase][k]);
		}
	}
	printk("fswitch: done\n");
}

static const struct bt_le_scan_param scan_param = {
	.type = BT_LE_SCAN_TYPE_PASSIVE,
	.options = BT_LE_SCAN_OPT_NONE,
	/* The longest window: the radio stays in RXSEARCH on one channel. */
	.interval = 0x4000,
	.window = 0x4000,
};

int main(void)
{
	uint32_t key;
	int err;

	wr(DEMCR, rd(DEMCR) | DEMCR_TRCENA);
	wr(DWT_CYCCNT, 0U);
	wr(DWT_CTRL, rd(DWT_CTRL) | 1U);

	err = bt_enable(NULL);
	printk("fswitch: bt_enable %d set 0x%x\n", err, CONFIG_FSWITCH_PROBE_SET);
	if (err != 0) {
		return 0;
	}
	if (IS_ENABLED(CONFIG_FSWITCH_PROBE_TX)) {
		e15_tx();
		return 0;
	}
	err = bt_le_scan_start(&scan_param, NULL);
	printk("fswitch: scan %d\n", err);
	if (err != 0) {
		return 0;
	}
	for (;;) {
		k_msleep(20);
		if (rac_state() == RAC_STATE_RXSEARCH &&
		    rd(SYNTH_FREQ) == word_of_hz((uint64_t)CH37_KHZ * HZ_PER_KHZ)) {
			k_msleep(20);
			key = irq_lock();
			if (rac_state() == RAC_STATE_RXSEARCH &&
			    rd(SYNTH_FREQ) == word_of_hz((uint64_t)CH37_KHZ * HZ_PER_KHZ)) {
				break;
			}
			irq_unlock(key);
		}
	}
	f0_word = rd(SYNTH_FREQ);
	f0_tuning = rd(SYNTH_VCOTUNING);
	dump("E0");
	if (CONFIG_FSWITCH_PROBE_SET & BIT(0)) {
		e1_locnt();
	}
	if (CONFIG_FSWITCH_PROBE_SET & BIT(1)) {
		e2_steps();
	}
	if (CONFIG_FSWITCH_PROBE_SET & BIT(2)) {
		e3_curve();
	}
	if (CONFIG_FSWITCH_PROBE_SET & BIT(3)) {
		e4_retunes();
	}
	if (CONFIG_FSWITCH_PROBE_SET & BIT(4)) {
		e5_range();
	}
	if (CONFIG_FSWITCH_PROBE_SET & BIT(5)) {
		e6_foe();
	}
	if (CONFIG_FSWITCH_PROBE_SET & BIT(6)) {
		e7_other();
	}
	if (CONFIG_FSWITCH_PROBE_SET & BIT(8)) {
		e9_tx_loop();
	}
	if (CONFIG_FSWITCH_PROBE_SET & BIT(9)) {
		e10_target();
	}
	if (CONFIG_FSWITCH_PROBE_SET & BIT(10)) {
		e11_early();
	}
	if (CONFIG_FSWITCH_PROBE_SET & BIT(11)) {
		e12_chirps();
	}
	if (CONFIG_FSWITCH_PROBE_SET & BIT(12)) {
		e13_edges();
	}
	if (CONFIG_FSWITCH_PROBE_SET & BIT(13)) {
		e14_crosscheck();
	}
	if (CONFIG_FSWITCH_PROBE_SET & BIT(14)) {
		e16_large();
	}
	dump("END");
	printk("fswitch: done\n");
	for (;;) {
	}
	return 0;
}
