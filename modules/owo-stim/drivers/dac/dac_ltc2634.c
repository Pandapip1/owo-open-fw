/*
 * Zephyr DAC API glue for the LTC262x-family DAC. Only the device object and the devicetree
 * config live here; the driver logic is Rust (modules/owo-stim/rust/crates/ltc2634).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#define DT_DRV_COMPAT lltc_ltc2634

#include <zephyr/device.h>
#include <zephyr/drivers/dac.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>

extern int ltc2634_rs_channel_setup(uint8_t channel, uint8_t resolution, bool internal);
extern int ltc2634_rs_init(const void *bus, const void *reset);
extern int ltc2634_rs_write_value(const void *bus, uint8_t channel, uint32_t value);

struct ltc2634_config {
	struct spi_dt_spec bus;
	struct gpio_dt_spec reset;
};

static int ltc2634_channel_setup(const struct device *dev, const struct dac_channel_cfg *cfg)
{
	ARG_UNUSED(dev);
	return ltc2634_rs_channel_setup(cfg->channel_id, cfg->resolution, cfg->internal);
}

static int ltc2634_write_value(const struct device *dev, uint8_t channel, uint32_t value)
{
	const struct ltc2634_config *c = dev->config;

	return ltc2634_rs_write_value(&c->bus, channel, value);
}

static int ltc2634_init(const struct device *dev)
{
	const struct ltc2634_config *c = dev->config;
	int r;

	if (!spi_is_ready_dt(&c->bus)) {
		return -ENODEV;
	}
	if (c->reset.port != NULL) {
		if (!gpio_is_ready_dt(&c->reset)) {
			return -ENODEV;
		}
		r = gpio_pin_configure_dt(&c->reset, GPIO_OUTPUT_INACTIVE);
		if (r < 0) {
			return r;
		}
	}
	return ltc2634_rs_init(&c->bus, c->reset.port != NULL ? &c->reset : NULL);
}

static DEVICE_API(dac, ltc2634_api) = {
	.channel_setup = ltc2634_channel_setup,
	.write_value = ltc2634_write_value,
};

#define LTC2634_INST(i)                                                                            \
	static const struct ltc2634_config ltc2634_cfg_##i = {                                     \
		.bus = SPI_DT_SPEC_INST_GET(i, SPI_OP_MODE_MASTER | SPI_WORD_SET(8) |              \
						       SPI_TRANSFER_MSB),                          \
		.reset = GPIO_DT_SPEC_INST_GET_OR(i, reset_gpios, {0}),                            \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(i, ltc2634_init, NULL, NULL, &ltc2634_cfg_##i, POST_KERNEL,          \
			      CONFIG_DAC_LTC2634_INIT_PRIORITY, &ltc2634_api);

DT_INST_FOREACH_STATUS_OKAY(LTC2634_INST)
