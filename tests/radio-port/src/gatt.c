/*
 * Test GATT service of the connection profile: one 4-octet characteristic the Central writes
 * and reads back over the link (tests/radio-rx-port/src/central.c in QEMU, a host Central on
 * silicon).
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/sys/printk.h>

/* Random 128-bit UUIDs owned by this test. */
#define RADIO_PORT_SVC_UUID BT_UUID_128_ENCODE(0x5b2a1c00, 0x6f3e, 0x4c1d, 0x9a7b, 0x0e1f2d3c4b5a)
#define RADIO_PORT_CHR_UUID BT_UUID_128_ENCODE(0x5b2a1c01, 0x6f3e, 0x4c1d, 0x9a7b, 0x0e1f2d3c4b5a)

static uint8_t value[4];

static ssize_t value_read(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			  uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset, value, sizeof(value));
}

static ssize_t value_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			   const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(flags);
	if (offset != 0U || len != sizeof(value)) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}
	memcpy(value, buf, sizeof(value));
	return len;
}

BT_GATT_SERVICE_DEFINE(radio_port_svc,
	BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_128(RADIO_PORT_SVC_UUID)),
	BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(RADIO_PORT_CHR_UUID),
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_READ | BT_GATT_PERM_WRITE, value_read, value_write,
			       NULL));

/* Link events on the console, for a hardware run against a host Central. */
static void radio_port_connected(struct bt_conn *conn, uint8_t err)
{
	ARG_UNUSED(conn);
	printk("peripheral_connected: %u\n", err);
}

static void radio_port_disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(conn);
	printk("peripheral_disconnected: 0x%02x\n", reason);
}

BT_CONN_CB_DEFINE(radio_port_conn_cb) = {
	.connected = radio_port_connected,
	.disconnected = radio_port_disconnected,
};
