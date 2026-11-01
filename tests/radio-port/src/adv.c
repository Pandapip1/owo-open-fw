/*
 * Advertising options that depend on the build configuration, for src/lib.rs.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdint.h>
#include <zephyr/bluetooth/bluetooth.h>

uint32_t radio_port_adv_channels_disabled(void)
{
	return IS_ENABLED(CONFIG_RADIO_PORT_ADV_CHANNEL_37_ONLY) ?
	       (BT_LE_ADV_OPT_DISABLE_CHAN_38 | BT_LE_ADV_OPT_DISABLE_CHAN_39) : 0U;
}
