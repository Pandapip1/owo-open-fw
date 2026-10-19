/*
 * Test peripherals for the native_sim run of the output stage:
 *  - owo,test-pwm: a PWM that remembers its duty
 *  - SPI emulators for the ti,sn74hc595 chain (shift register + latch + serial out) and the
 *    lltc,ltc2634 DAC (records the last code)
 *  - "plant": a small model of the circuit that turns the pin/DAC/switch state into the voltage the
 *    emulated ADC converts, so the real supervisor sees consistent feedback
 *  - accessors and knobs for the Rust test
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <errno.h>
#include <stdint.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc/adc_emul.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/spi_emul.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

/* ------------------------------------------------------------------ PWM */

#define DT_DRV_COMPAT owo_test_pwm

static atomic_t pwm_percent;

static int tpwm_set_cycles(const struct device *dev, uint32_t ch, uint32_t period, uint32_t pulse,
			   pwm_flags_t flags)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(ch);
	ARG_UNUSED(flags);
	atomic_set(&pwm_percent, period ? (pulse * 100U) / period : 0);
	return 0;
}

static int tpwm_cycles_per_sec(const struct device *dev, uint32_t ch, uint64_t *cycles)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(ch);
	*cycles = 1000000000ULL; /* one cycle = 1 ns */
	return 0;
}

static DEVICE_API(pwm, tpwm_api) = {
	.set_cycles = tpwm_set_cycles,
	.get_cycles_per_sec = tpwm_cycles_per_sec,
};

#define TPWM(n) DEVICE_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL, 50, &tpwm_api);
DT_INST_FOREACH_STATUS_OKAY(TPWM)

/* ------------------------------------------------------------ SPI emulators */

static uint8_t hc_sr[4];      /* shift register contents; hc_sr[3] is the far end */
static uint32_t hc_latched;   /* what the outputs show: copy of the chain at the last CS rise */
static atomic_t corrupt_readback;
static atomic_t hc_updates;

static uint32_t sr_word(void)
{
	return ((uint32_t)hc_sr[3] << 24) | ((uint32_t)hc_sr[2] << 16) | ((uint32_t)hc_sr[1] << 8) |
	       hc_sr[0];
}

static int hc595_io(const struct emul *target, const struct spi_config *config,
		    const struct spi_buf_set *tx_bufs, const struct spi_buf_set *rx_bufs)
{
	size_t n = 0;

	ARG_UNUSED(target);
	for (size_t i = 0; tx_bufs != NULL && i < tx_bufs->count; i++) {
		const uint8_t *tx = tx_bufs->buffers[i].buf;
		uint8_t *rx = (rx_bufs != NULL && i < rx_bufs->count) ? rx_bufs->buffers[i].buf : NULL;

		for (size_t b = 0; b < tx_bufs->buffers[i].len; b++, n++) {
			uint8_t out = hc_sr[3];

			if (atomic_get(&corrupt_readback)) {
				out ^= 0x01;
			}
			hc_sr[3] = hc_sr[2];
			hc_sr[2] = hc_sr[1];
			hc_sr[1] = hc_sr[0];
			hc_sr[0] = tx ? tx[b] : 0;
			if (rx != NULL) {
				rx[b] = out;
			}
		}
	}
	/* A transaction with a chip select ends with its rising edge: the chain is latched. The
	 * read-back transaction is made without the chip select, so it does not latch.
	 */
	if (config->cs.gpio.port != NULL && n > 0) {
		hc_latched = sr_word();
		atomic_inc(&hc_updates);
	}
	return 0;
}

static uint32_t dac_code;
static atomic_t dac_frames;

static int ltc_io(const struct emul *target, const struct spi_config *config,
		  const struct spi_buf_set *tx_bufs, const struct spi_buf_set *rx_bufs)
{
	ARG_UNUSED(target);
	ARG_UNUSED(config);
	ARG_UNUSED(rx_bufs);
	if (tx_bufs == NULL || tx_bufs->count < 1 || tx_bufs->buffers[0].len != 3) {
		return -EIO;
	}
	const uint8_t *f = tx_bufs->buffers[0].buf;
	uint8_t cmd = f[0] >> 4;

	if (cmd == 0x3) { /* write and update */
		dac_code = (((uint32_t)f[1] << 8) | f[2]) >> 4;
		atomic_inc(&dac_frames);
	}
	return 0;
}

static int emul_init(const struct emul *target, const struct device *parent)
{
	ARG_UNUSED(target);
	ARG_UNUSED(parent);
	return 0;
}

static struct spi_emul_api hc595_api = {.io = hc595_io};
static struct spi_emul_api ltc_api = {.io = ltc_io};

#undef DT_DRV_COMPAT
#define DT_DRV_COMPAT ti_sn74hc595
#define HC_EMUL(n) EMUL_DT_INST_DEFINE(n, emul_init, NULL, NULL, &hc595_api, NULL)
DT_INST_FOREACH_STATUS_OKAY(HC_EMUL)

#undef DT_DRV_COMPAT
#define DT_DRV_COMPAT lltc_ltc2634
#define LTC_EMUL(n) EMUL_DT_INST_DEFINE(n, emul_init, NULL, NULL, &ltc_api, NULL)
DT_INST_FOREACH_STATUS_OKAY(LTC_EMUL)

/* ------------------------------------------------------------------- plant */

#define SUIT DT_NODELABEL(suit_output)
static const struct device *const gpio0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));
static const struct device *const adc = DEVICE_DT_GET(DT_NODELABEL(test_adc));

static atomic_t gain_percent = ATOMIC_INIT(100);
static atomic_t adc_hang;
static K_SEM_DEFINE(hang_sem, 0, 1);
static atomic_t conversions;
static atomic_t shoot;       /* conversions that saw both gates of a leg (or both highs) on */
static atomic_t live_open;   /* conversions that saw the bridge closed with the stage not powered */
static uint32_t trace[32];
static atomic_t trace_len;
static uint32_t last_state = UINT32_MAX;
static int64_t closed_first_us, closed_last_us;
static uint32_t max_closed_us;

/* 0 off, 1 positive, 2 negative, 3 illegal */
uint32_t t_bridge_state(void)
{
	int ha = gpio_emul_output_get(gpio0, 0), hb = gpio_emul_output_get(gpio0, 1);
	int la = gpio_emul_output_get(gpio0, 2), lb = gpio_emul_output_get(gpio0, 3);

	if (!ha && !hb) {
		return 0; /* both high sides off: nothing is driven, whatever the low sides do */
	}
	if (ha && !hb && !la && lb) {
		return 1;
	}
	if (hb && !ha && !lb && la) {
		return 2;
	}
	return 3;
}

uint32_t t_supply_on(void)
{
	return gpio_emul_output_get(gpio0, 4) && gpio_emul_output_get(gpio0, 5);
}

static uint32_t plant_level(void)
{
	uint32_t st = t_bridge_state();
	uint32_t level;

	if ((st != 1 && st != 2) || !t_supply_on() || atomic_get(&pwm_percent) == 0 || hc_latched == 0) {
		return 0;
	}
	level = dac_code * 0x9C4U / 0x12FEDU;
	return level * (uint32_t)atomic_get(&gain_percent) / 100U;
}

static int plant_value(const struct device *dev, unsigned int chan, void *data, uint32_t *result)
{
	uint32_t st = t_bridge_state();
	int64_t now = k_ticks_to_us_near64(k_uptime_ticks());

	ARG_UNUSED(dev);
	ARG_UNUSED(chan);
	ARG_UNUSED(data);

	if (atomic_get(&adc_hang)) {
		(void)k_sem_take(&hang_sem, K_FOREVER); /* a stuck converter */
	}
	atomic_inc(&conversions);
	if (st == 3) {
		atomic_inc(&shoot);
	}
	if (gpio_emul_output_get(gpio0, 0) && gpio_emul_output_get(gpio0, 2)) {
		atomic_inc(&shoot);
	}
	if (gpio_emul_output_get(gpio0, 1) && gpio_emul_output_get(gpio0, 3)) {
		atomic_inc(&shoot);
	}
	if ((st == 1 || st == 2) && !t_supply_on()) {
		atomic_inc(&live_open);
	}
	if (st != last_state) {
		int n = atomic_get(&trace_len);

		if (n < (int)ARRAY_SIZE(trace)) {
			trace[n] = st;
			atomic_set(&trace_len, n + 1);
		}
		last_state = st;
	}
	if (st == 1 || st == 2) {
		if (closed_first_us == 0) {
			closed_first_us = now;
		}
		closed_last_us = now;
		if ((uint32_t)(closed_last_us - closed_first_us) > max_closed_us) {
			max_closed_us = closed_last_us - closed_first_us;
		}
	} else {
		closed_first_us = 0;
	}
	/* The ADC reads (level + 3) steps of 19.15 mV, the inverse of the sense scaling. */
	*result = (plant_level() + 3U) * 19150U / 1000U;
	return 0;
}

void t_plant_start(void)
{
	(void)adc_emul_value_func_set(adc, 0, plant_value, NULL);
}

/* ------------------------------------------------------------ test interface */

uint32_t t_switch_word(void) { return hc_latched; }
uint32_t t_switch_updates(void) { return (uint32_t)atomic_get(&hc_updates); }
uint32_t t_dac_code(void) { return dac_code; }
uint32_t t_dac_frames(void) { return (uint32_t)atomic_get(&dac_frames); }
uint32_t t_pwm_percent(void) { return (uint32_t)atomic_get(&pwm_percent); }
uint32_t t_shoot(void) { return (uint32_t)atomic_get(&shoot); }
uint32_t t_live_open(void) { return (uint32_t)atomic_get(&live_open); }
uint32_t t_conversions(void) { return (uint32_t)atomic_get(&conversions); }
uint32_t t_max_closed_us(void) { return max_closed_us; }
uint32_t t_trace_len(void) { return (uint32_t)atomic_get(&trace_len); }
uint32_t t_trace(uint32_t i) { return i < ARRAY_SIZE(trace) ? trace[i] : 0; }

void t_set_gain_percent(uint32_t g) { atomic_set(&gain_percent, g); }
void t_set_corrupt_readback(int on) { atomic_set(&corrupt_readback, on); }

void t_set_adc_hang(int on)
{
	atomic_set(&adc_hang, on);
	if (!on) {
		k_sem_give(&hang_sem);
	}
}

void t_reset_observations(void)
{
	atomic_set(&shoot, 0);
	atomic_set(&live_open, 0);
	atomic_set(&trace_len, 0);
	last_state = UINT32_MAX;
	max_closed_us = 0;
	closed_first_us = 0;
}

void t_exit(int code)
{
	extern void posix_exit(int);

	printk("native_sim test exit %d\n", code);
	posix_exit(code);
}
