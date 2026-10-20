/*
 * One-to-one wrappers around Zephyr APIs that are static inline or syscalls (invisible to
 * bindgen), the devicetree lookups for the "owo,suit-output" node, and the timestamped feedback
 * stream. No decisions are made here: every policy lives in the Rust crates, every hardware
 * driver is a stock Zephyr driver.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/drivers/dac.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/timing/timing.h>

/*
 * The Rust application provides this.  The job must stay in C because this
 * module owns the Kconfig-controlled static Zephyr thread.  It is deliberately
 * a preemptive (not cooperative or ISR) job: taking the application mutex and
 * executing the regulator/GPIO safe-off sequence may block, and neither is
 * legal in interrupt context.  Priority 0 is Zephyr's highest preemptive
 * priority.  Cooperative priorities could prevent ordinary application work
 * from running, while meta-IRQ priorities cannot take this mutex.
 */
/* Optional because the reusable output module is also linked by native_sim tests. */
extern void owo_stim_monitor_poll(void) __attribute__((weak));

#if defined(CONFIG_OWO_STIM_UNVALIDATED_HARDWARE)
static void owo_stim_monitor_job(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	for (;;) {
		if (owo_stim_monitor_poll != NULL) {
			owo_stim_monitor_poll();
		}
		k_sleep(K_MSEC(CONFIG_OWO_STIM_MONITOR_INTERVAL_MS));
	}
}

K_THREAD_DEFINE(owo_stim_monitor_thread,
		CONFIG_OWO_STIM_MONITOR_STACK_SIZE,
		owo_stim_monitor_job, NULL, NULL, NULL,
		0, 0, 0);
#endif

int owo_gpio_configure(const void *g, int active)
{
	return gpio_pin_configure_dt((const struct gpio_dt_spec *)g,
				     active ? GPIO_OUTPUT_ACTIVE : GPIO_OUTPUT_INACTIVE);
}

int owo_gpio_set(const void *g, int active)
{
	return gpio_pin_set_dt((const struct gpio_dt_spec *)g, active);
}

int owo_gpio_port_set_masked(const void *port, uint32_t mask, uint32_t value)
{
	return gpio_port_set_masked_raw((const struct device *)port, mask, value);
}

int owo_regulator_set(const void *reg, int on)
{
	return on ? regulator_enable((const struct device *)reg)
		  : regulator_disable((const struct device *)reg);
}

int owo_spi_transceive(const void *s, const uint8_t *tx, uint8_t *rx, size_t len)
{
	struct spi_buf txb = {.buf = (void *)tx, .len = len};
	struct spi_buf rxb = {.buf = rx, .len = len};
	struct spi_buf_set txs = {.buffers = &txb, .count = 1};
	struct spi_buf_set rxs = {.buffers = &rxb, .count = 1};

	return spi_transceive_dt((const struct spi_dt_spec *)s, &txs, rx ? &rxs : NULL);
}

int owo_dac_channel_setup(const void *d, uint8_t ch, uint8_t resolution)
{
	struct dac_channel_cfg cfg = {.channel_id = ch, .resolution = resolution};

	return dac_channel_setup((const struct device *)d, &cfg);
}

int owo_dac_write(const void *d, uint8_t ch, uint32_t value)
{
	return dac_write_value((const struct device *)d, ch, value);
}

int owo_pwm_set_percent(const void *p, uint8_t percent)
{
	const struct pwm_dt_spec *spec = p;

	if (percent > 100) {
		return -EINVAL;
	}
	return pwm_set_dt(spec, spec->period, (uint32_t)((uint64_t)spec->period * percent / 100U));
}

void owo_busy_wait_us(uint32_t us)
{
	k_busy_wait(us);
}

/* ---- fine counter for phase widths (Zephyr Counter API) ---- */

int owo_counter_start(const void *d)
{
	const struct device *dev = d;

	if (counter_get_max_top_value(dev) != UINT32_MAX || counter_get_frequency(dev) < 1000000U) {
		return -ENOTSUP; /* needs 32 bits and at most 1 us per tick */
	}
	return counter_start(dev);
}

void owo_counter_stop(const void *d)
{
	(void)counter_stop((const struct device *)d);
}

uint32_t owo_counter_mark(const void *d)
{
	uint32_t t = 0;

	(void)counter_get_value((const struct device *)d, &t);
	return t;
}

uint32_t owo_counter_since_us(const void *d, uint32_t mark)
{
	const struct device *dev = d;
	uint32_t now = 0;

	(void)counter_get_value(dev, &now);
	return (uint32_t)((uint64_t)(uint32_t)(now - mark) * 1000000ULL / counter_get_frequency(dev));
}

/* ---- timing (Zephyr timing API; the backend is the board's choice) ---- */

void owo_timing_start(void)
{
	timing_init();
	timing_start();
}

uint64_t owo_timing_stamp(void)
{
	return timing_counter_get();
}

uint64_t owo_timing_to_us(uint64_t cycles)
{
	return timing_cycles_to_ns(cycles) / 1000U;
}

/* ---- feedback stream: Zephyr ADC async sequence that repeats from its own callback ---- */

struct sample {
	uint64_t stamp;
	uint16_t raw;
};

/* Depth 1, "latest wins": the ISR drops the old entry and stores the new; readers peek. */
K_MSGQ_DEFINE(sample_q, sizeof(struct sample), 1, 8);
K_SEM_DEFINE(sample_first, 0, 1);
static uint16_t sample_buf;
static atomic_t sample_stop;
static struct adc_sequence_options sample_opts;
static struct adc_sequence sample_seq;

static enum adc_action sample_cb(const struct device *dev, const struct adc_sequence *seq,
				 uint16_t index)
{
	struct sample s;
	struct sample old;

	ARG_UNUSED(dev);
	ARG_UNUSED(index);
	if (atomic_get(&sample_stop)) {
		timing_stop(); /* last conversion: release the timing source (PM lock, power) */
		return ADC_ACTION_FINISH;
	}
	s.stamp = timing_counter_get();
	s.raw = *(const uint16_t *)seq->buffer;
	(void)k_msgq_get(&sample_q, &old, K_NO_WAIT);
	(void)k_msgq_put(&sample_q, &s, K_NO_WAIT);
	k_sem_give(&sample_first);
	return ADC_ACTION_REPEAT;
}

int owo_sense_start(const void *p, uint32_t interval_us)
{
	const struct adc_dt_spec *a = p;
	int r = adc_channel_setup_dt(a);

	if (r < 0) {
		return r;
	}
	k_msgq_purge(&sample_q);
	k_sem_reset(&sample_first);
	atomic_set(&sample_stop, 0);
	sample_opts = (struct adc_sequence_options){.interval_us = interval_us, .callback = sample_cb};
	sample_seq = (struct adc_sequence){
		.options = &sample_opts,
		.channels = BIT(a->channel_id),
		.buffer = &sample_buf,
		.buffer_size = sizeof(sample_buf),
		.resolution = a->resolution,
	};
	/* Blocks briefly if a previous run is still finishing. No signal: nobody waits for it. */
	return adc_read_async(a->dev, &sample_seq, NULL);
}

/* Thread context only. The ADC completion callback gives this after publishing the sample. */
int owo_sense_wait_first(uint32_t timeout_us)
{
	return k_sem_take(&sample_first, K_USEC(timeout_us));
}

void owo_sense_stop(void)
{
	atomic_set(&sample_stop, 1); /* the next conversion returns ADC_ACTION_FINISH */
}

/* 0 and *raw/*stamp set, or -ENODATA if nothing completed since owo_sense_start(). */
int owo_sense_latest(uint16_t *raw, uint64_t *stamp)
{
	struct sample s;

	if (k_msgq_peek(&sample_q, &s) != 0) {
		return -ENODATA;
	}
	*raw = s.raw;
	*stamp = s.stamp;
	return 0;
}

/* Zephyr's own conversion: the channel's resolution, gain and reference. */
int32_t owo_adc_raw_to_mv(const void *p, uint16_t raw)
{
	const struct adc_dt_spec *a = p;
	int32_t v = raw;

	return adc_raw_to_millivolts_dt(a, &v) < 0 ? -1 : v;
}

/* ---- devicetree: the "owo,suit-output" node ---- */

#define SUIT DT_COMPAT_GET_ANY_STATUS_OKAY(owo_suit_output)

#if DT_HAS_COMPAT_STATUS_OKAY(owo_suit_output)

static const struct gpio_dt_spec bridge[4] = {
	GPIO_DT_SPEC_GET_BY_IDX(SUIT, bridge_gpios, 0), GPIO_DT_SPEC_GET_BY_IDX(SUIT, bridge_gpios, 1),
	GPIO_DT_SPEC_GET_BY_IDX(SUIT, bridge_gpios, 2), GPIO_DT_SPEC_GET_BY_IDX(SUIT, bridge_gpios, 3),
};

#define SUPPLY_DEV(i, _) DEVICE_DT_GET(DT_PHANDLE_BY_IDX(SUIT, supplies, i))
static const struct device *const supplies[] = {
	LISTIFY(DT_PROP_LEN(SUIT, supplies), SUPPLY_DEV, (,), _)
};

#define ROUTE(i, _) DT_PROP_BY_IDX(SUIT, switch_routes, i)
static const uint32_t switch_routes[] = {
	LISTIFY(DT_PROP_LEN(SUIT, switch_routes), ROUTE, (,), _)
};

static const struct device *const switch_dev = DEVICE_DT_GET(DT_PHANDLE(SUIT, switch));
static const struct device *const dac_dev =
	DEVICE_DT_GET(DT_IO_CHANNELS_CTLR_BY_NAME(SUIT, amplitude));
static const struct device *const phase_timer = DEVICE_DT_GET(DT_PHANDLE(SUIT, phase_timer));
static const struct pwm_dt_spec boost = PWM_DT_SPEC_GET_BY_IDX(SUIT, 0);
static const struct adc_dt_spec sense = ADC_DT_SPEC_GET_BY_NAME(SUIT, sense);

/*
 * Reset baseline.  This runs on every normal boot after the device drivers have
 * initialised but before APPLICATION main (and therefore before Rust, Bluetooth,
 * or the output backend).  Do not rely on a pre-reset callback: a watchdog or
 * brown-out reset cannot run one.  Every operation is best effort so a missing
 * peripheral cannot prevent the remaining independent safe-off operations.
 */
static int owo_suit_reset_safe_off(void)
{
	struct dac_channel_cfg dac_cfg = {
		.channel_id = DT_PHA_BY_NAME(SUIT, io_channels, amplitude, output),
		.resolution = DT_PROP(SUIT, amplitude_resolution),
	};
	uint32_t switch_mask = 0;
	size_t i;

	/* Same order as Stimulator::safe_off: bridge, amplitude, routing, supply. */
	for (i = 0; i < ARRAY_SIZE(bridge); ++i) {
		(void)gpio_pin_configure_dt(&bridge[i],
			(i < 2) ? GPIO_OUTPUT_INACTIVE : GPIO_OUTPUT_ACTIVE);
	}
	if (device_is_ready(dac_dev)) {
		(void)dac_channel_setup(dac_dev, &dac_cfg);
		(void)dac_write_value(dac_dev, dac_cfg.channel_id, 0);
	}
	for (i = 0; i < ARRAY_SIZE(switch_routes); ++i) {
		switch_mask |= switch_routes[i];
	}
	if (device_is_ready(switch_dev)) {
		(void)gpio_port_set_masked_raw(switch_dev, switch_mask, 0);
	}
	if (device_is_ready(boost.dev)) {
		(void)pwm_set_dt(&boost, boost.period, 0);
	}
	if (device_is_ready(phase_timer)) {
		(void)counter_stop(phase_timer);
	}
	for (i = 0; i < ARRAY_SIZE(supplies); ++i) {
		if (device_is_ready(supplies[i])) {
			(void)regulator_disable(supplies[i]);
		}
	}
	return 0;
}

SYS_INIT(owo_suit_reset_safe_off, APPLICATION, 0);

/* Board parameters. Kinds: see zephyr-io. */
uint32_t owo_suit_param(int kind)
{
	switch (kind) {
	case 0: return DT_PHA_BY_NAME(SUIT, io_channels, amplitude, output);
	case 1: return DT_PROP(SUIT, amplitude_resolution);
	case 2: return DT_PROP_BY_IDX(SUIT, amplitude_scale, 0);
	case 3: return DT_PROP_BY_IDX(SUIT, amplitude_scale, 1);
	case 4: return DT_PROP(SUIT, max_level);
	case 5: return DT_PROP(SUIT, sense_uv_per_level);
	case 6: return DT_PROP(SUIT, sense_offset);
	case 7: return DT_PROP(SUIT, boost_duty_percent);
	case 8: return ARRAY_SIZE(supplies);
	case 9: return ARRAY_SIZE(switch_routes);
	default: return 0;
	}
}

const uint32_t *owo_suit_switch_routes(void)
{
	return switch_routes;
}

const void *owo_suit_handle(int kind)
{
	const void *h = NULL;
	const struct device *dev = NULL;

	switch (kind) {
	case 0: case 1: case 2: case 3:
		h = &bridge[kind]; dev = bridge[kind].port; break;
	case 4: h = switch_dev; dev = switch_dev; break;
	case 5: h = dac_dev; dev = dac_dev; break;
	case 6: h = &boost; dev = boost.dev; break;
	case 7: h = &sense; dev = sense.dev; break;
	case 8: h = phase_timer; dev = phase_timer; break;
	default:
		/* 16 + i: supply regulator i */
		if (kind >= 16 && (size_t)(kind - 16) < ARRAY_SIZE(supplies)) {
			h = supplies[kind - 16]; dev = supplies[kind - 16]; break;
		}
		return NULL;
	}
	return (dev != NULL && device_is_ready(dev)) ? h : NULL;
}

#else

const void *owo_suit_handle(int kind)
{
	ARG_UNUSED(kind);
	return NULL;
}

uint32_t owo_suit_param(int kind)
{
	ARG_UNUSED(kind);
	return 0;
}

const uint32_t *owo_suit_switch_routes(void)
{
	return NULL;
}

#endif

/*
 * Hardware watchdog (devicetree alias "watchdog0"). Started once with a reset-the-SoC
 * timeout; paused in EM2/EM3 (WDT_OPT_PAUSE_IN_SLEEP) so long sleeps are safe, but it keeps
 * counting while the CPU idles in EM1, so the caller must feed it at least every timeout.
 */
#if DT_HAS_ALIAS(watchdog0) && DT_NODE_HAS_STATUS_OKAY(DT_ALIAS(watchdog0))
static const struct device *const wdt_dev = DEVICE_DT_GET(DT_ALIAS(watchdog0));
static int wdt_channel = -1;

int owo_wdt_start(uint32_t timeout_ms)
{
	struct wdt_timeout_cfg cfg = {
		.window = {.min = 0, .max = timeout_ms},
		.flags = WDT_FLAG_RESET_SOC,
	};
	int ch;

	if (!device_is_ready(wdt_dev)) {
		return -ENODEV;
	}
	ch = wdt_install_timeout(wdt_dev, &cfg);
	if (ch < 0) {
		return ch;
	}
	wdt_channel = ch;
	return wdt_setup(wdt_dev, WDT_OPT_PAUSE_IN_SLEEP);
}

int owo_wdt_feed(void)
{
	return wdt_channel < 0 ? -ENODEV : wdt_feed(wdt_dev, wdt_channel);
}
#else
int owo_wdt_start(uint32_t timeout_ms)
{
	return -ENODEV;
}

int owo_wdt_feed(void)
{
	return -ENODEV;
}
#endif
