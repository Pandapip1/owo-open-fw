/*
 * Connectable GATT peripheral: one primary service with one characteristic
 * that a client can read, write and write without response.  Every write is
 * echoed on the console so a host-side test can check it arrived.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/sys/printk.h>

/* Random 128-bit UUIDs; the service's is the characteristic's with the
 * first group incremented. */
#define TEST_SERVICE_UUID_VAL                                                                      \
	BT_UUID_128_ENCODE(0x8f1c0001U, 0x6d41U, 0x4b3aU, 0x9b6eU, 0x0c2f9a4d7e10ULL)
#define TEST_VALUE_UUID_VAL                                                                        \
	BT_UUID_128_ENCODE(0x8f1c0002U, 0x6d41U, 0x4b3aU, 0x9b6eU, 0x0c2f9a4d7e10ULL)

static const struct bt_uuid_128 test_service_uuid = BT_UUID_INIT_128(TEST_SERVICE_UUID_VAL);
static const struct bt_uuid_128 test_value_uuid = BT_UUID_INIT_128(TEST_VALUE_UUID_VAL);

/* The characteristic's value: the last write, initially a fixed greeting. */
#define TEST_VALUE_MAX_LEN 20U
static uint8_t test_value[TEST_VALUE_MAX_LEN] = "efr32";
static uint16_t test_value_len = sizeof("efr32") - 1U;

static ssize_t read_value(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			  uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset, test_value, test_value_len);
}

static ssize_t write_value(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			   const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	if ((offset + len) > sizeof(test_value)) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}
	memcpy(&test_value[offset], buf, len);
	test_value_len = offset + len;
	printk("gatt_write: %u bytes at %u\n", len, offset);
	return len;
}

BT_GATT_SERVICE_DEFINE(test_service, BT_GATT_PRIMARY_SERVICE(&test_service_uuid),
		       BT_GATT_CHARACTERISTIC(&test_value_uuid.uuid,
					      BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE |
						      BT_GATT_CHRC_WRITE_WITHOUT_RESP,
					      BT_GATT_PERM_READ | BT_GATT_PERM_WRITE, read_value,
					      write_value, NULL));

static void connected(struct bt_conn *conn, uint8_t err)
{
	printk("connected: 0x%02x\n", err);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	printk("disconnected: 0x%02x\n", reason);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
};

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, TEST_SERVICE_UUID_VAL),
};

static const struct bt_data sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

int main(void)
{
	int err = bt_enable(NULL);

	printk("bt_enable: %d\n", err);
	if (err != 0) {
		return 0;
	}
	err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_2, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	printk("adv_start: %d\n", err);
	return 0;
}
