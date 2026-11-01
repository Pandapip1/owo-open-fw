/*
 * Test GATT service of the connection profile: one 4-octet characteristic the Central writes
 * and reads back over the link (tests/radio-rx-port/src/central.c). QEMU workload only.
 * With RADIO_PORT_PAIRING_TEST it needs an authenticated encrypted link, as the product's
 * characteristics do, and the passkey comes from the app_passkey callback, as the product's
 * does (src/ble.rs).
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/sys/printk.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>

/* Random 128-bit UUIDs owned by this test. */
#define RADIO_PORT_SVC_UUID BT_UUID_128_ENCODE(0x5b2a1c00, 0x6f3e, 0x4c1d, 0x9a7b, 0x0e1f2d3c4b5a)
#define RADIO_PORT_CHR_UUID BT_UUID_128_ENCODE(0x5b2a1c01, 0x6f3e, 0x4c1d, 0x9a7b, 0x0e1f2d3c4b5a)

#if defined(CONFIG_RADIO_PORT_PAIRING_TEST)
#define RADIO_PORT_PERM (BT_GATT_PERM_READ_AUTHEN | BT_GATT_PERM_WRITE_AUTHEN)
#else
#define RADIO_PORT_PERM (BT_GATT_PERM_READ | BT_GATT_PERM_WRITE)
#endif

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
			       RADIO_PORT_PERM, value_read, value_write, NULL));

#if defined(CONFIG_RADIO_PORT_PAIRING_TEST)
/* DisplayOnly through app_passkey (subsys/bluetooth/host/smp.c get_io_capa()), so a keyboard
 * Central gets authenticated passkey entry.
 */
static uint32_t app_passkey(struct bt_conn *conn)
{
	ARG_UNUSED(conn);
	return CONFIG_RADIO_PORT_PASSKEY;
}

static void pairing_complete(struct bt_conn *conn, bool bonded)
{
	ARG_UNUSED(conn);
	printk("peripheral_paired: bonded %d\n", bonded);
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	ARG_UNUSED(conn);
	printk("peripheral_pairing_failed: %d\n", reason);
}

static const struct bt_conn_auth_cb auth_cb = {
	.app_passkey = app_passkey,
};

static struct bt_conn_auth_info_cb auth_info_cb = {
	.pairing_complete = pairing_complete,
	.pairing_failed = pairing_failed,
};

static int radio_port_pairing_init(void)
{
	int err = bt_conn_auth_cb_register(&auth_cb);

	if (err == 0) {
		err = bt_conn_auth_info_cb_register(&auth_info_cb);
	}
	return err;
}

SYS_INIT(radio_port_pairing_init, APPLICATION, 0);
#endif /* CONFIG_RADIO_PORT_PAIRING_TEST */
