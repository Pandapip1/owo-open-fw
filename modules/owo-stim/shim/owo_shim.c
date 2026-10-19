/*
 * C side of the Rust crates: plain functions over Zephyr APIs that are static inline or
 * syscall wrappers (and so invisible to bindgen), plus the devicetree lookups for the
 * "owo,suit-output" node. Handles are opaque pointers to static specs/devices.
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
#include <zephyr/drivers/spi.h>

int owo_gpio_configure(const void *g, int active)
{
	return gpio_pin_configure_dt((const struct gpio_dt_spec *)g,
				     active ? GPIO_OUTPUT_ACTIVE : GPIO_OUTPUT_INACTIVE);
}

int owo_gpio_set(const void *g, int active)
{
	return gpio_pin_set_dt((const struct gpio_dt_spec *)g, active);
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

/* Channel 0 of the sense ADC is the only one used; 12 bit, no oversampling beyond the driver's 2x. */
static uint16_t adc_buf;

static struct adc_sequence adc_seq(const struct adc_dt_spec *a)
{
	return (struct adc_sequence){
		.channels = BIT(a->channel_id),
		.buffer = &adc_buf,
		.buffer_size = sizeof(adc_buf),
		.resolution = a->resolution,
	};
}

int owo_adc_prime(const void *p)
{
	const struct adc_dt_spec *a = p;
	struct adc_sequence seq = adc_seq(a);
	int r = adc_channel_setup_dt(a);

	if (r < 0) {
		return r;
	}
	return adc_read_dt(a, &seq);
}

int owo_adc_read_polled(const void *p, uint32_t timeout_us, uint16_t *out)
{
	const struct adc_dt_spec *a = p;
	struct adc_sequence seq = adc_seq(a);
	int r = adc_read_polled(a->dev, &seq, timeout_us);

	if (r == 0) {
		*out = adc_buf;
	}
	return r;
}

void owo_busy_wait_us(uint32_t us)
{
	k_busy_wait(us);
}

uint32_t owo_irq_lock(void)
{
	return (uint32_t)irq_lock();
}

void owo_irq_unlock(uint32_t key)
{
	irq_unlock((unsigned int)key);
}

#define SUIT DT_COMPAT_GET_ANY_STATUS_OKAY(owo_suit_output)

#if DT_HAS_COMPAT_STATUS_OKAY(owo_suit_output)

static const struct gpio_dt_spec bridge[4] = {
	GPIO_DT_SPEC_GET_BY_IDX(SUIT, bridge_gpios, 0), GPIO_DT_SPEC_GET_BY_IDX(SUIT, bridge_gpios, 1),
	GPIO_DT_SPEC_GET_BY_IDX(SUIT, bridge_gpios, 2), GPIO_DT_SPEC_GET_BY_IDX(SUIT, bridge_gpios, 3),
};
static const struct gpio_dt_spec latch = GPIO_DT_SPEC_GET(SUIT, latch_gpios);
static const struct gpio_dt_spec supply[2] = {
	GPIO_DT_SPEC_GET_BY_IDX(SUIT, supply_gpios, 0),
	GPIO_DT_SPEC_GET_BY_IDX(SUIT, supply_gpios, 1),
};
static const struct spi_dt_spec switch_spi =
	SPI_DT_SPEC_GET(DT_PHANDLE(SUIT, switch), SPI_OP_MODE_MASTER | SPI_WORD_SET(8) | SPI_TRANSFER_MSB);
static const struct device *const dac_dev = DEVICE_DT_GET(DT_PHANDLE(SUIT, dac));
static const struct pwm_dt_spec boost = PWM_DT_SPEC_GET_BY_IDX(SUIT, 0);
static const struct adc_dt_spec sense = ADC_DT_SPEC_GET_BY_IDX(SUIT, 0);

const void *owo_suit_handle(int kind)
{
	const void *h = NULL;
	const struct device *dev = NULL;

	switch (kind) {
	case 0: case 1: case 2: case 3:
		h = &bridge[kind]; dev = bridge[kind].port; break;
	case 4: h = &latch; dev = latch.port; break;
	case 5: case 6:
		h = &supply[kind - 5]; dev = supply[kind - 5].port; break;
	case 7: h = &switch_spi; dev = switch_spi.bus; break;
	case 8: h = dac_dev; dev = dac_dev; break;
	case 9: h = &boost; dev = boost.dev; break;
	case 10: h = &sense; dev = sense.dev; break;
	default: return NULL;
	}
	return (dev != NULL && device_is_ready(dev)) ? h : NULL;
}

#else

const void *owo_suit_handle(int kind)
{
	ARG_UNUSED(kind);
	return NULL;
}

#endif
