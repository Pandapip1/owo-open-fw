/*
 * Central connection workload: scan for the connectable advertiser, then create an LE link and
 * report the host's connected and disconnected callbacks.
 * SPDX-License-Identifier: Apache-2.0
 *
 * This is the QEMU-only proof that the ported lower link layer runs the connection exchange:
 * the central's CONNECT_IND reaches the advertiser, connection events follow, and Zephyr's host
 * on the central reports an established link. It is not a claim about RF airtime or silicon
 * timing. Written in C because the Rust bindings expose only <zephyr/bluetooth/bluetooth.h>,
 * which no longer declares the connection API.
 */
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>

/* Scan window 30 ms and interval 60 ms, Zephyr's BT_GAP_SCAN_FAST_* values. */
#define CENTRAL_SCAN_INTERVAL BT_GAP_SCAN_FAST_INTERVAL
#define CENTRAL_SCAN_WINDOW BT_GAP_SCAN_FAST_WINDOW

/*
 * Connection parameters: 7.5..50 ms interval (1.25 ms units), no peripheral latency, a 5 s
 * supervision timeout (10 ms units). The minimum interval is deliberate: a bounded QEMU run then
 * crosses many connection events after the link is up.
 */
#define CENTRAL_CONN_INTERVAL_MIN 0x0006
#define CENTRAL_CONN_INTERVAL_MAX 0x0028
#define CENTRAL_CONN_TIMEOUT 500

static bt_addr_le_t peer;
static bool create_issued;
static struct bt_conn *conn;
/* Given when the link is up: the GATT loop then starts. */
static K_SEM_DEFINE(link_up, 0, 1);

static void connected(struct bt_conn *c, uint8_t err)
{
	ARG_UNUSED(c);
	/* Zero: the host accepted the controller's connection complete event. */
	printk("central_connected: %u\n", err);
	if (err == 0U) {
		k_sem_give(&link_up);
	}
}

static void disconnected(struct bt_conn *c, uint8_t reason)
{
	ARG_UNUSED(c);
	printk("central_disconnected: 0x%02x at %lld ms\n", reason, k_uptime_get());
}

#if defined(CONFIG_RADIO_RX_PORT_PAIRING)
/* Given when the link reached security level 4 (LE Secure Connections, authenticated). */
static K_SEM_DEFINE(secured, 0, 1);
static int64_t security_requested_ms;

static void security_changed(struct bt_conn *c, bt_security_t level, enum bt_security_err err)
{
	ARG_UNUSED(c);
	printk("central_security: level %d err %d after %lld ms\n", level, err,
	       k_uptime_get() - security_requested_ms);
	if (err == BT_SECURITY_ERR_SUCCESS && level >= BT_SECURITY_L4) {
		k_sem_give(&secured);
	}
}

/* Keyboard-only: the Central enters the Peripheral's passkey. */
static void passkey_entry(struct bt_conn *c)
{
	printk("central_passkey_entry: %06u\n", CONFIG_RADIO_RX_PORT_PASSKEY);
	(void)bt_conn_auth_passkey_entry(c, CONFIG_RADIO_RX_PORT_PASSKEY);
}

static void auth_cancel(struct bt_conn *c)
{
	ARG_UNUSED(c);
	printk("central_pairing_cancelled\n");
}

static void pairing_complete(struct bt_conn *c, bool bonded)
{
	ARG_UNUSED(c);
	printk("central_paired: bonded %d\n", bonded);
}

static void pairing_failed(struct bt_conn *c, enum bt_security_err reason)
{
	ARG_UNUSED(c);
	printk("central_pairing_failed: %d\n", reason);
}

static const struct bt_conn_auth_cb auth_cb = {
	.passkey_entry = passkey_entry,
	.cancel = auth_cancel,
};

static struct bt_conn_auth_info_cb auth_info_cb = {
	.pairing_complete = pairing_complete,
	.pairing_failed = pairing_failed,
};
#endif /* CONFIG_RADIO_RX_PORT_PAIRING */

BT_CONN_CB_DEFINE(central_conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
#if defined(CONFIG_RADIO_RX_PORT_PAIRING)
	.security_changed = security_changed,
#endif
};

/*
 * The report's address is valid only during this callback, so the peer is copied before the
 * create request. Scanning stops first: the scan and initiate windows compete for the radio.
 */
static void scan_result(const bt_addr_le_t *addr, int8_t rssi, uint8_t adv_type,
			struct net_buf_simple *buf)
{
	const struct bt_conn_le_create_param create_param =
		BT_CONN_LE_CREATE_PARAM_INIT(0, CENTRAL_SCAN_INTERVAL, CENTRAL_SCAN_WINDOW);
	const struct bt_le_conn_param conn_param =
		BT_LE_CONN_PARAM_INIT(CENTRAL_CONN_INTERVAL_MIN, CENTRAL_CONN_INTERVAL_MAX, 0,
				      CENTRAL_CONN_TIMEOUT);
	int err;

	ARG_UNUSED(rssi);
	ARG_UNUSED(adv_type);
	ARG_UNUSED(buf);
	if (addr == NULL || create_issued || addr->type == BT_ADDR_LE_ANONYMOUS) {
		return;
	}
	bt_addr_le_copy(&peer, addr);
	create_issued = true;
	printk("scan_stop: %d\n", bt_le_scan_stop());
	err = bt_conn_le_create(&peer, &create_param, &conn_param, &conn);
	printk("conn_le_create: %d\n", err);
}

/*
 * GATT over the link: find the advertiser's test characteristic (tests/radio-port/src/gatt.c),
 * then once per CENTRAL_GATT_PERIOD_MS write a counter to it and read it back. Every result is
 * counted; a summary is printed every CENTRAL_GATT_REPORT_EVERY rounds.
 */
#define CENTRAL_GATT_PERIOD_MS 1000
#define CENTRAL_GATT_REPORT_EVERY 30
#define CENTRAL_GATT_TIMEOUT_MS 5000
#if defined(CONFIG_RADIO_RX_PORT_PEER_PRODUCT)
/* The product's "Test" characteristic (src/ble.rs U_TEST): it keeps a written 20-octet frame and
 * returns it on read, both only over an authenticated encrypted link.
 */
static const struct bt_uuid_128 test_chr_uuid = BT_UUID_INIT_128(
	BT_UUID_128_ENCODE(0xb1f5a059, 0xc717, 0x11e4, 0x8731, 0x1681e6b88ec1));
#define CENTRAL_VALUE_LEN 20U
#else
static const struct bt_uuid_128 test_chr_uuid = BT_UUID_INIT_128(
	BT_UUID_128_ENCODE(0x5b2a1c01, 0x6f3e, 0x4c1d, 0x9a7b, 0x0e1f2d3c4b5a));
#define CENTRAL_VALUE_LEN 4U
#endif
static K_SEM_DEFINE(gatt_done, 0, 1);
static uint16_t value_handle;
static uint8_t gatt_err;
static uint8_t read_back[CENTRAL_VALUE_LEN];
static uint16_t read_len;

static uint8_t discovered(struct bt_conn *c, const struct bt_gatt_attr *attr,
			  struct bt_gatt_discover_params *params)
{
	ARG_UNUSED(c);
	if (attr != NULL) {
		value_handle = ((const struct bt_gatt_chrc *)attr->user_data)->value_handle;
	}
	k_sem_give(&gatt_done);
	ARG_UNUSED(params);
	return BT_GATT_ITER_STOP;
}

static void written(struct bt_conn *c, uint8_t err, struct bt_gatt_write_params *params)
{
	ARG_UNUSED(c);
	ARG_UNUSED(params);
	gatt_err = err;
	k_sem_give(&gatt_done);
}

static uint8_t read_cb(struct bt_conn *c, uint8_t err, struct bt_gatt_read_params *params,
		       const void *data, uint16_t length)
{
	ARG_UNUSED(c);
	ARG_UNUSED(params);
	if (data != NULL) {
		read_len = MIN(length, sizeof(read_back));
		memcpy(read_back, data, read_len);
		return BT_GATT_ITER_CONTINUE;
	}
	gatt_err = err;
	k_sem_give(&gatt_done);
	return BT_GATT_ITER_STOP;
}

static int gatt_wait(void)
{
	return k_sem_take(&gatt_done, K_MSEC(CENTRAL_GATT_TIMEOUT_MS));
}

static void gatt_loop(void)
{
	struct bt_gatt_discover_params discover = {
		.uuid = &test_chr_uuid.uuid,
		.func = discovered,
		.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE,
		.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE,
		.type = BT_GATT_DISCOVER_CHARACTERISTIC,
	};
	uint32_t rounds = 0U;
	uint32_t ok = 0U;
	uint32_t failed = 0U;

	if (bt_gatt_discover(conn, &discover) != 0 || gatt_wait() != 0 || value_handle == 0U) {
		printk("gatt_discover: failed\n");
		return;
	}
	printk("gatt_discover: handle 0x%04x\n", value_handle);
	for (;;) {
		uint32_t counter = rounds + 1U;
		uint8_t value[CENTRAL_VALUE_LEN] = { 0 };
		struct bt_gatt_write_params write = {
			.func = written, .handle = value_handle, .data = value,
			.length = sizeof(value),
		};
		struct bt_gatt_read_params read = {
			.func = read_cb, .handle_count = 1, .single.handle = value_handle,
		};
		bool good;

		memcpy(value, &counter, sizeof(counter));
		k_msleep(CENTRAL_GATT_PERIOD_MS);
		gatt_err = 0U;
		read_len = 0U;
		good = bt_gatt_write(conn, &write) == 0 && gatt_wait() == 0 && gatt_err == 0U &&
		       bt_gatt_read(conn, &read) == 0 && gatt_wait() == 0 && gatt_err == 0U &&
		       read_len == sizeof(value) && memcmp(read_back, value, sizeof(value)) == 0;
		rounds++;
		if (good) {
			ok++;
		} else {
			failed++;
			printk("gatt_rw: round %u failed (err 0x%02x, read %u)\n", rounds, gatt_err,
			       read_len);
		}
		if (rounds % CENTRAL_GATT_REPORT_EVERY == 0U) {
			printk("gatt_rw: %u rounds, %u ok, %u failed\n", rounds, ok, failed);
		}
	}
}

void radio_rx_port_central_start(void)
{
	const struct bt_le_scan_param scan = {
		.type = BT_LE_SCAN_TYPE_ACTIVE,
		.interval = CENTRAL_SCAN_INTERVAL,
		.window = CENTRAL_SCAN_WINDOW,
	};
	int err = bt_enable(NULL);

	printk("bt_enable: %d\n", err);
	if (err != 0) {
		return;
	}
#if defined(CONFIG_RADIO_RX_PORT_PAIRING)
	printk("auth_register: %d %d\n", bt_conn_auth_cb_register(&auth_cb),
	       bt_conn_auth_info_cb_register(&auth_info_cb));
#endif
	printk("scan_start: %d\n", bt_le_scan_start(&scan, scan_result));
	k_sem_take(&link_up, K_FOREVER);
#if defined(CONFIG_RADIO_RX_PORT_PAIRING)
	/* Pair (LE Secure Connections, passkey entry) and encrypt before any GATT access. */
	security_requested_ms = k_uptime_get();
	printk("set_security: %d\n", bt_conn_set_security(conn, BT_SECURITY_L4));
	if (k_sem_take(&secured, K_SECONDS(120)) != 0) {
		printk("central_security: timeout\n");
		return;
	}
#endif
	gatt_loop();
}
