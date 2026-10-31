/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Image descriptor for the Silicon Labs Gecko Bootloader in front of the app (0x16000).
 *
 * The bootloader reads a pointer to this descriptor from vector-table slot 13 of the app.
 * Without one, the suit's bootloader keeps AppLoader running instead of starting the app
 * (docs/ota.md). The layout and every value below match the descriptor the stock app
 * carries (read from the unit, 2026-10-31): structure version 1.1, no signature, type
 * Bluetooth app, version 1. Only the product ID differs, so stock and open images can be
 * told apart.
 */

#include <stddef.h>
#include <stdint.h>

#include <zephyr/toolchain.h>

/* Identifies the descriptor; the bootloader rejects anything else. */
#define GECKO_APP_PROPERTIES_MAGIC                                                         \
	{0x13U, 0xb7U, 0x79U, 0xfaU, 0xc9U, 0x25U, 0xddU, 0xb7U,                           \
	 0xadU, 0xf3U, 0xcfU, 0xe0U, 0xf1U, 0xb6U, 0x14U, 0xb8U}
/* Structure version 1.1: the layout that ends with the certificate pointer. */
#define GECKO_APP_PROPERTIES_STRUCT_VERSION 0x00000101U
#define GECKO_APP_SIGNATURE_NONE            0U
#define GECKO_APP_TYPE_BLUETOOTH_APP        0x00000020U

struct gecko_app_data {
	uint32_t type;
	uint32_t version;
	uint32_t capabilities;
	uint8_t product_id[16];
};

struct gecko_app_properties {
	uint8_t magic[16];
	uint32_t struct_version;
	uint32_t signature_type;
	uint32_t signature_location;
	struct gecko_app_data app;
	const void *certificate;
};

/*
 * Named for the vector slot it fills (arch/arm/core/cortex_m/vector_table.S), so the
 * linker resolves the slot to this descriptor's address.
 */
__used const struct gecko_app_properties z_arm_vector_reserved_13 = {
	.magic = GECKO_APP_PROPERTIES_MAGIC,
	.struct_version = GECKO_APP_PROPERTIES_STRUCT_VERSION,
	.signature_type = GECKO_APP_SIGNATURE_NONE,
	.signature_location = 0U,
	.app = {
		.type = GECKO_APP_TYPE_BLUETOOTH_APP,
		/*
		 * Same as stock: whether this bootloader refuses lower versions is not
		 * known, so neither direction (stock <-> open) may depend on it.
		 */
		.version = CONFIG_OWO_GECKO_APP_VERSION,
		.capabilities = 0U,
		/* owo-open-fw: 217c860a-3c51-4aaa-bdf4-a465bfce3eb9 */
		.product_id = {0x21U, 0x7cU, 0x86U, 0x0aU, 0x3cU, 0x51U, 0x4aU, 0xaaU,
			       0xbdU, 0xf4U, 0xa4U, 0x65U, 0xbfU, 0xceU, 0x3eU, 0xb9U},
	},
	.certificate = NULL,
};
