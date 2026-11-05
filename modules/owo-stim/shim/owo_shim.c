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
#include <zephyr/drivers/dac.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/init.h>
#if defined(CONFIG_SOC_FAMILY_SILABS_S2)
#include <zephyr/drivers/pinctrl.h>
#include <em_device.h>
#include <sl_clock_manager.h>
#include <sl_hal_gpio.h>
#include <sl_hal_i2c.h>
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

/*
 * ---- fine timestamps for phase widths (Zephyr system clock) ----
 *
 * The system clock's hardware cycles are the timebase, so no counter device is
 * owned here. The handle is only an identity token for the Rust wrapper.
 */
static const char fine_clock;

int owo_counter_start(const void *d)
{
	ARG_UNUSED(d);
	if (sys_clock_hw_cycles_per_sec() < 1000000U) {
		return -ENOTSUP; /* needs at most 1 us per cycle */
	}
	return 0;
}

void owo_counter_stop(const void *d)
{
	ARG_UNUSED(d);
}

uint32_t owo_counter_mark(const void *d)
{
	ARG_UNUSED(d);
	return k_cycle_get_32();
}

uint32_t owo_counter_since_us(const void *d, uint32_t mark)
{
	ARG_UNUSED(d);
	return k_cyc_to_us_floor32(k_cycle_get_32() - mark);
}

/*
 * ---- conversions started by the supervisor (feedback and battery) ----
 *
 * One conversion at a time, of the channel the caller names: Zephyr's asynchronous read of a
 * one-sample sequence, whose completion callback (the ADC interrupt) publishes the result. The
 * supervisor starts each conversion at a point of the pulse it chooses and polls for the result,
 * so the ADC only interrupts where it was asked to; between pulses nothing samples.
 */
enum { CONV_IDLE, CONV_RUNNING, CONV_DONE };
static atomic_t conv_state;
static uint16_t conv_buf;
static struct adc_sequence_options conv_opts;
static struct adc_sequence conv_seq;

static enum adc_action conv_cb(const struct device *dev, const struct adc_sequence *seq,
			       uint16_t index)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(seq);
	ARG_UNUSED(index);
	atomic_set(&conv_state, CONV_DONE);
	return ADC_ACTION_FINISH;
}

/* Once per channel, before its first conversion (its gain, reference and input). */
int owo_adc_setup(const void *p)
{
	return adc_channel_setup_dt((const struct adc_dt_spec *)p);
}

/*
 * Start one conversion; thread context. -EBUSY while an earlier one has not completed: its
 * asynchronous read still holds the driver, and a new read would block on it for ever.
 */
int owo_adc_start(const void *p)
{
	const struct adc_dt_spec *a = p;
	int r;

	if (atomic_get(&conv_state) == CONV_RUNNING) {
		return -EBUSY;
	}
	atomic_set(&conv_state, CONV_RUNNING);
	conv_opts = (struct adc_sequence_options){.callback = conv_cb};
	conv_seq = (struct adc_sequence){
		.options = &conv_opts,
		.channels = BIT(a->channel_id),
		.buffer = &conv_buf,
		.buffer_size = sizeof(conv_buf),
		.resolution = a->resolution,
		.oversampling = a->oversampling,
	};
	r = adc_read_async(a->dev, &conv_seq, NULL);
	if (r < 0) {
		atomic_set(&conv_state, CONV_IDLE); /* refused: nothing is running */
	}
	return r;
}

/* 1 and *raw once the conversion completed, 0 while it runs or if none was started. */
int owo_adc_poll(uint16_t *raw)
{
	if (!atomic_cas(&conv_state, CONV_DONE, CONV_IDLE)) {
		return 0;
	}
	*raw = conv_buf;
	return 1;
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
static const struct pwm_dt_spec boost = PWM_DT_SPEC_GET_BY_IDX(SUIT, 0);
static const struct adc_dt_spec sense = ADC_DT_SPEC_GET_BY_NAME(SUIT, sense);
static const struct adc_dt_spec battery = ADC_DT_SPEC_GET_BY_NAME(SUIT, battery);
BUILD_ASSERT(DT_PROP_LEN(SUIT, battery_divider) == 2, "battery-divider is <num den>");
#if defined(CONFIG_ADC_SILABS_IADC)
/* owo-suit-hw.conf sizes the IADC driver's channel table for these channels. */
BUILD_ASSERT(DT_IO_CHANNELS_INPUT_BY_NAME(SUIT, sense) < CONFIG_ADC_SILABS_IADC_CHANNELS &&
	     DT_IO_CHANNELS_INPUT_BY_NAME(SUIT, battery) < CONFIG_ADC_SILABS_IADC_CHANNELS,
	     "raise CONFIG_ADC_SILABS_IADC_CHANNELS to cover the sense and battery channels");
#endif

/*
 * Reset safe-off runs before Rust has an error channel. Preserve whether every
 * independently safe-off operation was acknowledged, so later self-test cannot turn
 * a best-effort reset path into a successful POST merely because a device is
 * now ready.  This is deliberately reset-local BSS: every reset must perform
 * and account for this pass again.
 */
static uint32_t owo_suit_reset_safe_off_failures;

#define RESET_SAFE_OFF_BRIDGE(i) BIT(i)
#define RESET_SAFE_OFF_DAC       BIT(4)
#define RESET_SAFE_OFF_SWITCH    BIT(5)
#define RESET_SAFE_OFF_BOOST     BIT(6)
#define RESET_SAFE_OFF_SUPPLY(i) BIT(8 + (i))

/*
 * Reset shutdown. This runs on every normal boot after the device
 * drivers have initialised but before APPLICATION main (and therefore before
 * Rust, Bluetooth, or the output backend). Do not rely on a pre-reset
 * callback: a watchdog or brown-out reset cannot run one. Every operation is
 * best effort so a missing peripheral cannot prevent the remaining independent
 * safe-off operations. Rust subsequently checks readiness and verifies a
 * forced known-off state before any safety-critical service may start.
 */
static int owo_suit_reset_safe_off(void)
{
	struct dac_channel_cfg dac_cfg = {
		.channel_id = DT_PHA_BY_NAME(SUIT, io_channels, amplitude, output),
		.resolution = DT_PROP(SUIT, amplitude_resolution),
	};
	uint32_t switch_mask = 0;
	size_t i;

	owo_suit_reset_safe_off_failures = 0;

	/* Same order as Stimulator::safe_off: bridge, amplitude, routing, supply. */
	for (i = 0; i < ARRAY_SIZE(bridge); ++i) {
		if (!gpio_is_ready_dt(&bridge[i]) ||
		    gpio_pin_configure_dt(&bridge[i],
			(i < 2) ? GPIO_OUTPUT_INACTIVE : GPIO_OUTPUT_ACTIVE) < 0) {
			owo_suit_reset_safe_off_failures |= RESET_SAFE_OFF_BRIDGE(i);
		}
	}
	if (device_is_ready(dac_dev)) {
		if (dac_channel_setup(dac_dev, &dac_cfg) < 0 ||
		    dac_write_value(dac_dev, dac_cfg.channel_id, 0) < 0) {
			owo_suit_reset_safe_off_failures |= RESET_SAFE_OFF_DAC;
		}
	} else {
		owo_suit_reset_safe_off_failures |= RESET_SAFE_OFF_DAC;
	}
	for (i = 0; i < ARRAY_SIZE(switch_routes); ++i) {
		switch_mask |= switch_routes[i];
	}
	if (device_is_ready(switch_dev)) {
		if (gpio_port_set_masked_raw(switch_dev, switch_mask, 0) < 0) {
			owo_suit_reset_safe_off_failures |= RESET_SAFE_OFF_SWITCH;
		}
	} else {
		owo_suit_reset_safe_off_failures |= RESET_SAFE_OFF_SWITCH;
	}
	if (device_is_ready(boost.dev)) {
		if (pwm_set_dt(&boost, boost.period, 0) < 0) {
			owo_suit_reset_safe_off_failures |= RESET_SAFE_OFF_BOOST;
		}
	} else {
		owo_suit_reset_safe_off_failures |= RESET_SAFE_OFF_BOOST;
	}
	for (i = 0; i < ARRAY_SIZE(supplies); ++i) {
		if (device_is_ready(supplies[i])) {
			if (regulator_disable(supplies[i]) < 0) {
				owo_suit_reset_safe_off_failures |= RESET_SAFE_OFF_SUPPLY(i);
			}
		} else {
			owo_suit_reset_safe_off_failures |= RESET_SAFE_OFF_SUPPLY(i);
		}
	}
	return 0;
}

SYS_INIT(owo_suit_reset_safe_off, APPLICATION, 0);

/*
 * Non-energising POST for the Rust bring-up barrier. This deliberately
 * checks every output-owning driver before the application accepts BLE work.
 * Rust still runs the full forced safe-off sequence even if this reports
 * failure, so a failed POST cannot skip recovery after a brown-out.
 */
int owo_suit_post(void)
{
	size_t i;

	/* A successful readiness check cannot retroactively confirm reset safe-off. */
	if (owo_suit_reset_safe_off_failures != 0) {
		return -EIO;
	}

	for (i = 0; i < ARRAY_SIZE(bridge); ++i) {
		if (!device_is_ready(bridge[i].port)) {
			return -ENODEV;
		}
	}
	if (!device_is_ready(switch_dev) || !device_is_ready(dac_dev) ||
	    !device_is_ready(boost.dev) || !device_is_ready(sense.dev) ||
	    !device_is_ready(battery.dev)) {
		return -ENODEV;
	}
	for (i = 0; i < ARRAY_SIZE(supplies); ++i) {
		if (!device_is_ready(supplies[i])) {
			return -ENODEV;
		}
	}
	return 0;
}

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
	case 10: return DT_PROP_BY_IDX(SUIT, battery_divider, 0);
	case 11: return DT_PROP_BY_IDX(SUIT, battery_divider, 1);
	case 12: return DT_NODE_HAS_PROP(SUIT, stop_gpios);
	default: return 0;
	}
}

const uint32_t *owo_suit_switch_routes(void)
{
	return switch_routes;
}

/*
 * ---- stop input (stop-gpios) ----
 *
 * The callback runs where the input's controller delivers it (for a PCF857x: the system work
 * queue, after it read the port on its /INT); the Rust side reads the level and decides.
 */
#if DT_NODE_HAS_PROP(SUIT, stop_gpios)
static const struct gpio_dt_spec stop_in = GPIO_DT_SPEC_GET(SUIT, stop_gpios);
static struct gpio_callback stop_cb;

extern void owo_suit_stop_input_changed(void);

static void stop_changed(const struct device *port, struct gpio_callback *cb,
			 gpio_port_pins_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);
	owo_suit_stop_input_changed();
}

int owo_suit_stop_input_setup(void)
{
	int r;

	if (!gpio_is_ready_dt(&stop_in)) {
		return -ENODEV;
	}
	r = gpio_pin_configure_dt(&stop_in, GPIO_INPUT);
	if (r < 0) {
		return r;
	}
	gpio_init_callback(&stop_cb, stop_changed, BIT(stop_in.pin));
	r = gpio_add_callback_dt(&stop_in, &stop_cb);
	if (r < 0) {
		return r;
	}
	return gpio_pin_interrupt_configure_dt(&stop_in, GPIO_INT_EDGE_BOTH);
}

int owo_suit_stop_input_get(void)
{
	return gpio_pin_get_dt(&stop_in);
}

/*
 * 1 while the stop input's controller asserts its interrupt line (a change not yet read), or
 * when it has none (only a read can tell). Reading the line is a plain GPIO read: no transfer
 * to the controller.
 */
#define STOP_CTLR DT_GPIO_CTLR(SUIT, stop_gpios)
#if DT_NODE_HAS_PROP(STOP_CTLR, int_gpios)
static const struct gpio_dt_spec stop_ctlr_int = GPIO_DT_SPEC_GET(STOP_CTLR, int_gpios);

int owo_suit_stop_input_pending(void)
{
	return gpio_pin_get_dt(&stop_ctlr_int) != 0;
}
#else
int owo_suit_stop_input_pending(void)
{
	return 1;
}
#endif
#else
int owo_suit_stop_input_setup(void)
{
	return -ENOTSUP;
}

int owo_suit_stop_input_get(void)
{
	return -ENOTSUP;
}

int owo_suit_stop_input_pending(void)
{
	return 0;
}
#endif

#if defined(CONFIG_SOC_FAMILY_SILABS_S2)
/*
 * ---- output safe state from the watchdog warning interrupt ----
 *
 * owo_suit_force_safe_from_isr() runs in the WDOG0 warning interrupt (src/watchdog.c): zero
 * latency, so a thread or driver may have been stopped anywhere, even inside a bus transfer or
 * an irq_lock() section. It makes plain register writes only (no driver, no kernel call), in
 * the order of Stimulator::safe_off, and never returns to the code it interrupted, so nothing
 * it leaves behind is ever used again: the watchdog reset follows.
 *  1. Bridge (GPIO DOUT through the SET/CLR aliases): both high sides off first, then both low
 *     sides on, the reset safe-off's levels: break before make, never a same-leg pair on.
 *  2. Boost PWM: compare buffer 0 (duty 0 from the next period, as pwm_set with pulse 0).
 *  3. Expander: one write of all its pins: supply enables inactive, the DAC's /CLR asserted
 *     (amplitude to zero scale), every other pin 1, the PCF857x's input/weak-high state, which
 *     is what the driver leaves on every pin it does not drive (the product drives none).
 *     The interrupted thread may own the bus: the transfer is aborted first. Every wait ends
 *     with the reset at the latest.
 */
#define BRIDGE_N DT_PROP_LEN(SUIT, bridge_gpios)
/* Binding order: high side A, high side B, low side A, low side B (owo_suit_reset_safe_off). */
#define BRIDGE_HIGH_SIDES 2
BUILD_ASSERT(BRIDGE_N == 2 * BRIDGE_HIGH_SIDES, "an H-bridge has two high and two low sides");

struct raw_pin {
	uintptr_t port; /* GPIO_PORT_TypeDef of the pin's port */
	uint8_t pin;
	uint8_t active_low;
};

#define BRIDGE_RAW(i, _)                                                                           \
	{                                                                                          \
		.port = DT_REG_ADDR(DT_GPIO_CTLR_BY_IDX(SUIT, bridge_gpios, i)),                   \
		.pin = DT_GPIO_PIN_BY_IDX(SUIT, bridge_gpios, i),                                  \
		.active_low = (DT_GPIO_FLAGS_BY_IDX(SUIT, bridge_gpios, i) & GPIO_ACTIVE_LOW) != 0, \
	}
static const struct raw_pin bridge_raw[BRIDGE_N] = {LISTIFY(BRIDGE_N, BRIDGE_RAW, (,), _)};

/* Drive a pin to its logical @active level through the port's SET or CLR alias. */
static inline void raw_pin_drive(const struct raw_pin *p, bool active)
{
	uintptr_t dout = (uintptr_t)&((GPIO_PORT_TypeDef *)p->port)->DOUT;

	dout += (active != (bool)p->active_low) ? PER_REG_BLOCK_SET_OFFSET : PER_REG_BLOCK_CLR_OFFSET;
	*(volatile uint32_t *)dout = BIT(p->pin);
}

static inline bool raw_pin_active(const struct raw_pin *p)
{
	return (((((GPIO_PORT_TypeDef *)p->port)->DOUT) >> p->pin) & 1U) != p->active_low;
}

/*
 * True while no high side of the bridge is driven on: no current path from the rail through
 * the pads. Read from the output latches (DOUT), which is what the pins drive.
 */
int owo_suit_output_quiescent(void)
{
	for (size_t i = 0; i < BRIDGE_HIGH_SIDES; ++i) {
		if (raw_pin_active(&bridge_raw[i])) {
			return 0;
		}
	}
	return 1;
}

static void bridge_safe_raw(void)
{
	for (size_t i = 0; i < BRIDGE_N; ++i) {
		raw_pin_drive(&bridge_raw[i], i >= BRIDGE_HIGH_SIDES);
	}
}

#define BOOST_PWM_NODE DT_PWMS_CTLR(SUIT)
#if DT_NODE_HAS_COMPAT(BOOST_PWM_NODE, silabs_timer_pwm)
BUILD_ASSERT((DT_PWMS_FLAGS(SUIT) & PWM_POLARITY_MASK) == PWM_POLARITY_NORMAL,
	     "compare value 0 is off only for a normal-polarity PWM");
static void boost_safe_raw(void)
{
	TIMER_TypeDef *const timer = (TIMER_TypeDef *)DT_REG_ADDR(DT_PARENT(BOOST_PWM_NODE));

	/* OCB is a SYNC register: written only while the timer is enabled (RM SS4.2.4.4.3). */
	if (timer->EN & TIMER_EN_EN) {
		timer->CC[DT_PWMS_CHANNEL(SUIT)].OCB = 0;
	}
}
#else
#error "owo_suit_force_safe_from_isr: no raw safe-off for this boost PWM controller"
#endif

#define SUPPLY_NODE(i) DT_PHANDLE_BY_IDX(SUIT, supplies, i)
#define EXPANDER DT_GPIO_CTLR(SUPPLY_NODE(0), enable_gpios)
#define SUPPLY_ON_EXPANDER(i, _) DT_SAME_NODE(DT_GPIO_CTLR(SUPPLY_NODE(i), enable_gpios), EXPANDER)
BUILD_ASSERT(LISTIFY(DT_PROP_LEN(SUIT, supplies), SUPPLY_ON_EXPANDER, (&&), _),
	     "every supply enable must be on the one expander");
BUILD_ASSERT(DT_NODE_HAS_COMPAT(EXPANDER, nxp_pcf857x), "raw write is a PCF857x port write");
BUILD_ASSERT(DT_NODE_HAS_COMPAT(DT_BUS(EXPANDER), silabs_i2c), "raw write is a Silabs I2C");
/* A PCF857x write is its ports' bytes, port 0 first (NXP PCF8575 datasheet, 8.3.1). */
#define EXPANDER_PORT_BITS 8
#define EXPANDER_PORTS (DT_PROP(EXPANDER, ngpios) / EXPANDER_PORT_BITS)
#define EXPANDER_PINS BIT_MASK(DT_PROP(EXPANDER, ngpios))

#define SUPPLY_PIN(i) BIT(DT_GPIO_PIN(SUPPLY_NODE(i), enable_gpios))
#define SUPPLY_LOW_ACTIVE(i) (DT_GPIO_FLAGS(SUPPLY_NODE(i), enable_gpios) & GPIO_ACTIVE_LOW)
#define SUPPLY_MASK(i, _) SUPPLY_PIN(i)
/* An enable is off at its inactive level: high only if it is active low. */
#define SUPPLY_OFF_HIGH(i, _) (SUPPLY_LOW_ACTIVE(i) ? SUPPLY_PIN(i) : 0)
#define SUPPLIES_MASK (LISTIFY(DT_PROP_LEN(SUIT, supplies), SUPPLY_MASK, (|), _))
#define SUPPLIES_OFF (LISTIFY(DT_PROP_LEN(SUIT, supplies), SUPPLY_OFF_HIGH, (|), _))

#define DAC_NODE DT_IO_CHANNELS_CTLR_BY_NAME(SUIT, amplitude)
#if DT_NODE_HAS_PROP(DAC_NODE, reset_gpios)
BUILD_ASSERT(DT_SAME_NODE(DT_GPIO_CTLR(DAC_NODE, reset_gpios), EXPANDER),
	     "the DAC's /CLR is expected on the supply expander");
#define DAC_CLR_PIN BIT(DT_GPIO_PIN(DAC_NODE, reset_gpios))
/* Asserted at its active level: high only if it is active high. */
#define DAC_CLR_ASSERTED                                                                           \
	((DT_GPIO_FLAGS(DAC_NODE, reset_gpios) & GPIO_ACTIVE_LOW) ? 0 : DAC_CLR_PIN)
#else
#define DAC_CLR_PIN 0
#define DAC_CLR_ASSERTED 0
#endif

#define EXPANDER_SAFE                                                                              \
	((EXPANDER_PINS & ~(SUPPLIES_MASK | DAC_CLR_PIN)) | SUPPLIES_OFF | DAC_CLR_ASSERTED)
/* 7-bit address and R/W = 0 (write) in the address byte (I2C-bus specification, 3.1.10). */
#define I2C_ADDRESS_BYTE_WRITE(addr) ((uint32_t)(addr) << 1)

/* No bound on a wait: the warning interrupt's waits end with the watchdog reset. */
#define I2C_RAW_UNBOUNDED 0U

/*
 * Wait for the follower's acknowledge of the byte on the bus. False on a NACK, a lost
 * arbitration or a bus error, and once @limit CPU cycles (DWT CYCCNT) have passed since @start
 * unless @limit is I2C_RAW_UNBOUNDED.
 */
static bool i2c_raw_acked(I2C_TypeDef *i2c, uint32_t start, uint32_t limit)
{
	const uint32_t done = I2C_IF_ACK | I2C_IF_NACK | I2C_IF_ARBLOST | I2C_IF_BUSERR;
	uint32_t f;

	do {
		f = i2c->IF & done;
		if (f == 0 && limit != I2C_RAW_UNBOUNDED && DWT->CYCCNT - start >= limit) {
			return false;
		}
	} while (f == 0);
	i2c->IF_CLR = f;
	return f == I2C_IF_ACK;
}

/* One port write of the expander's safe pin levels on an enabled leader. */
static void expander_write_safe(I2C_TypeDef *i2c, uint32_t limit)
{
	uint32_t pins = EXPANDER_SAFE;
	uint32_t start = DWT->CYCCNT;

	/* End whatever was on the bus (RM SS23.3.8). */
	i2c->CMD = I2C_CMD_ABORT | I2C_CMD_CLEARTX | I2C_CMD_CLEARPC;
	i2c->IF_CLR = _I2C_IF_MASK;
	i2c->TXDATA = I2C_ADDRESS_BYTE_WRITE(DT_REG_ADDR(EXPANDER));
	i2c->CMD = I2C_CMD_START;
	for (size_t i = 0; i < EXPANDER_PORTS && i2c_raw_acked(i2c, start, limit); ++i) {
		i2c->TXDATA = pins & BIT_MASK(EXPANDER_PORT_BITS);
		pins >>= EXPANDER_PORT_BITS;
	}
	(void)i2c_raw_acked(i2c, start, limit);
	i2c->CMD = I2C_CMD_STOP;
}

static void expander_safe_raw(void)
{
	I2C_TypeDef *const i2c = (I2C_TypeDef *)DT_REG_ADDR(DT_BUS(EXPANDER));

	if (!(i2c->EN & I2C_EN_EN)) {
		return; /* not up yet: owo_suit_early_safe_off() has written the safe levels at boot */
	}
	expander_write_safe(i2c, I2C_RAW_UNBOUNDED);
}

void owo_suit_force_safe_from_isr(void)
{
	bridge_safe_raw();
	boost_safe_raw();
	expander_safe_raw();
}

/*
 * ---- the earliest safe-off after a reset ----
 *
 * The expander (supply enables, the DAC's /CLR) and the DAC keep their state across an MCU
 * reset: after a reset with the stage live, the boost supply stayed enabled until the reset
 * safe-off at APPLICATION level (docs/stim-simulation.md). This runs at the first init level
 * (EARLY, before the SoC's clock setup and every driver), with register writes only:
 *  1. Bridge: the safe gate levels on the output latches, then the pins to push-pull (they float
 *     from the reset until here).
 *  2. Expander: the I2C leader brought up from its reset state (bus clocks, the I2C0 pinctrl
 *     state, standard-mode divider from the clock the bus actually has now) and the same port
 *     write as the warning interrupt's (supply enables off, DAC /CLR asserted, so the DAC goes
 *     to zero scale). The watchdog does not run yet, so every wait is bounded by the cycle
 *     counter: EARLY_I2C_BYTE_TIMES byte times of the divider actually programmed.
 *  3. The leader is reset again so the I2C driver starts from the state it expects.
 * The boost PWM pin is disabled from reset until its driver configures it (off).
 */
/* The I2C leader's default pin control state, as its driver applies it later. */
static const pinctrl_soc_pin_t early_i2c_pins[] =
	Z_PINCTRL_STATE_PINS_INIT(DT_BUS(EXPANDER), pinctrl_0);

/* I2C byte: 8 data bits and the acknowledge (I2C-bus specification, 3.1.6). */
#define I2C_BITS_PER_BYTE 9U
/*
 * The whole write is START + address + the port bytes + STOP; allow its byte count plus one
 * for the START/STOP conditions, twice over for a follower that stretches the clock (a PCF857x
 * does not; the factor only keeps a slow but working bus from being cut off).
 */
#define EARLY_I2C_BYTE_TIMES (2U * (1U + 1U + EXPANDER_PORTS + 1U))

static int owo_suit_early_safe_off(void)
{
	I2C_TypeDef *const i2c = (I2C_TypeDef *)DT_REG_ADDR(DT_BUS(EXPANDER));
	uint32_t i2c_clk, scl, cpu = SystemCoreClockGet();

	sl_clock_manager_enable_bus_clock(SL_BUS_CLOCK_GPIO);
	for (size_t i = 0; i < BRIDGE_N; ++i) {
		const sl_gpio_t pin = {
			.port = (bridge_raw[i].port - (uintptr_t)&GPIO->P[0]) / sizeof(GPIO->P[0]),
			.pin = bridge_raw[i].pin,
		};

		raw_pin_drive(&bridge_raw[i], i >= BRIDGE_HIGH_SIDES);
		sl_hal_gpio_set_pin_mode(&pin, SL_GPIO_MODE_PUSH_PULL,
					 (i >= BRIDGE_HIGH_SIDES) != bridge_raw[i].active_low);
	}

	sl_clock_manager_enable_bus_clock(SL_BUS_CLOCK_I2C0);
	if (sl_clock_manager_get_clock_branch_frequency(SL_CLOCK_BRANCH_LSPCLK, &i2c_clk) != 0 ||
	    pinctrl_configure_pins(early_i2c_pins, ARRAY_SIZE(early_i2c_pins),
				   PINCTRL_REG_NONE) < 0) {
		return 0; /* the reset safe-off and the warning still follow */
	}
	sl_hal_i2c_reset(i2c);
	sl_hal_i2c_init(i2c, SL_I2C_LEADER_MODE);
	sl_hal_i2c_set_clock_frequency(i2c, i2c_clk, SL_HAL_I2C_FREQ_STANDARD_MAX,
				       SL_I2C_CLK_HLR_STANDARD);
	scl = sl_hal_i2c_get_clock_frequency(i2c, i2c_clk);
	sl_hal_i2c_enable(i2c);

	DCB->DEMCR |= DCB_DEMCR_TRCENA_Msk;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
	if (scl != 0) {
		expander_write_safe(i2c, (uint32_t)((uint64_t)cpu * EARLY_I2C_BYTE_TIMES *
						    I2C_BITS_PER_BYTE / scl));
	}
	sl_hal_i2c_reset(i2c);
	sl_hal_i2c_disable(i2c);
	return 0;
}

SYS_INIT(owo_suit_early_safe_off, EARLY, 0);
#endif /* CONFIG_SOC_FAMILY_SILABS_S2 */

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
	case 8: return &fine_clock; /* the system clock: nothing to be ready */
	case 9: h = &battery; dev = battery.dev; break;
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

int owo_suit_post(void)
{
	return -ENODEV;
}

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

int owo_suit_stop_input_setup(void)
{
	return -ENOTSUP;
}

int owo_suit_stop_input_get(void)
{
	return -ENOTSUP;
}

int owo_suit_stop_input_pending(void)
{
	return 0;
}

/* No output stage: nothing to put in a safe state, nothing that can be energised. */
void owo_suit_force_safe_from_isr(void)
{
}

int owo_suit_output_quiescent(void)
{
	return 1;
}

#endif
