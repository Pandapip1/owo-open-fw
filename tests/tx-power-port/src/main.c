/*
 * Transmit power control of the EFR32 lower link layer (docs/tx-power.md), on one QEMU machine:
 * the HCI commands that read and set it, the power amplifier level each request programs
 * (MODEM.RAMPLEV as the radio's next event leaves it), and the amateur station's ceiling, which
 * caps both the advertiser and the identification.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <errno.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/hci_vs.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/sys_io.h>

#include "hal/efr32/amateur.h"

/*
 * MODEM.RAMPLEV, non-secure alias (public register model MODEM_S_register.py, offset 0x0CC):
 * three 8-bit fields, each the 6 dBm PA's level plus 32 (docs/tx-power.md 2).
 */
#define MODEM_RAMPLEV 0xB80140CCUL
#define RAMPLEV_OFFSET 32U
#define RAMPLEV_FIELDS 0x00010101U
#define RAMPLEV(level) (((level) + RAMPLEV_OFFSET) * RAMPLEV_FIELDS)

/* PA levels of the published outputs (hal/tx_power.c) the requests below select. */
#define LEVEL_PLUS_4 26U   /* 3.4 dBm: the highest not above +4 */
#define LEVEL_MINUS_10 4U  /* -10.4 dBm */
#define LEVEL_LOWEST 0U    /* -27.5 dBm */
#define LEVEL_ZERO 14U     /* -0.6 dBm: the highest not above 0 */
#define LEVEL_TOP 121U     /* 8.5 dBm: the highest published output */
#define LEVEL_RESET 127U   /* the reset value's: the highest level */

/* Several advertising events at the default 100 ms interval. */
#define EVENTS_MS 400

#define HANDLE_TYPE_UNKNOWN 0x05U

/*
 * The amateur plan: every channel on channel 37's carrier (Core v6.1 Vol 6 Part B 1.4.1:
 * 2402 MHz), so the test, run on silicon too, sends its frames and its on-off keyed
 * identifications only there.
 */
#define CH37_KHZ 2402000U

static const uint32_t *ch37_plan(void)
{
	static uint32_t plan[EFR32_AMATEUR_PLAN_CHANNELS];

	for (uint32_t ch = 0U; ch < EFR32_AMATEUR_PLAN_CHANNELS; ch++) {
		plan[ch] = CH37_KHZ;
	}
	return plan;
}

static int checks;
static int failures;

#define CHECK(cond)                                                                       \
	do {                                                                              \
		checks++;                                                                 \
		if (!(cond)) {                                                            \
			failures++;                                                       \
			printk("tx-power: FAIL line %d: %s\n", __LINE__, #cond);          \
		}                                                                         \
	} while (0)

static int le_read_tx_power(int8_t *min, int8_t *max)
{
	struct bt_hci_rp_le_read_tx_power *rp;
	struct net_buf *rsp;
	int err;

	err = bt_hci_cmd_send_sync(BT_HCI_OP_LE_READ_TX_POWER, NULL, &rsp);
	if (err != 0) {
		return err;
	}
	rp = (void *)rsp->data;
	*min = rp->min_tx_power;
	*max = rp->max_tx_power;
	net_buf_unref(rsp);

	return 0;
}

static int vs_write(uint8_t handle_type, uint16_t handle, int8_t dbm, int8_t *selected)
{
	struct bt_hci_cp_vs_write_tx_power_level *cp;
	struct bt_hci_rp_vs_write_tx_power_level *rp;
	struct net_buf *buf;
	struct net_buf *rsp;
	int err;

	buf = bt_hci_cmd_alloc(K_FOREVER);
	if (buf == NULL) {
		return -ENOBUFS;
	}
	cp = net_buf_add(buf, sizeof(*cp));
	cp->handle_type = handle_type;
	cp->handle = sys_cpu_to_le16(handle);
	cp->tx_power_level = dbm;
	err = bt_hci_cmd_send_sync(BT_HCI_OP_VS_WRITE_TX_POWER_LEVEL, buf, &rsp);
	if (err != 0) {
		return err;
	}
	rp = (void *)rsp->data;
	*selected = rp->selected_tx_power;
	net_buf_unref(rsp);

	return 0;
}

static int vs_read(uint8_t handle_type, uint16_t handle, int8_t *dbm)
{
	struct bt_hci_cp_vs_read_tx_power_level *cp;
	struct bt_hci_rp_vs_read_tx_power_level *rp;
	struct net_buf *buf;
	struct net_buf *rsp;
	int err;

	buf = bt_hci_cmd_alloc(K_FOREVER);
	if (buf == NULL) {
		return -ENOBUFS;
	}
	cp = net_buf_add(buf, sizeof(*cp));
	cp->handle_type = handle_type;
	cp->handle = sys_cpu_to_le16(handle);
	err = bt_hci_cmd_send_sync(BT_HCI_OP_VS_READ_TX_POWER_LEVEL, buf, &rsp);
	if (err != 0) {
		return err;
	}
	rp = (void *)rsp->data;
	*dbm = rp->tx_power_level;
	net_buf_unref(rsp);

	return 0;
}

static uint32_t ramplev_after_events(void)
{
	k_sleep(K_MSEC(EVENTS_MS));

	return sys_read32(MODEM_RAMPLEV);
}

static void report_ramplev(const char *what, uint32_t ramplev)
{
	printk("tx-power: %s RAMPLEV 0x%06x\n", what, ramplev);
}

int main(void)
{
	static const struct bt_le_adv_param adv_param = BT_LE_ADV_PARAM_INIT(
		0, BT_GAP_ADV_FAST_INT_MIN_2, BT_GAP_ADV_FAST_INT_MAX_2, NULL);
	struct efr32_amateur_config amateur = { .callsign = "KD3APL",
						.plan_khz = ch37_plan() };
	int8_t min = 0;
	int8_t max = 0;
	int8_t dbm = 0;
	uint32_t ramplev;
	int err;

	err = bt_enable(NULL);
	printk("bt_enable: %d\n", err);
	if (err != 0) {
		return 0;
	}

	/* Nothing has transmitted: RAMPLEV holds its reset value. */
	CHECK(sys_read32(MODEM_RAMPLEV) == RAMPLEV(LEVEL_RESET));

	/* LE Read Transmit Power: the published outputs' range, rounded up. */
	CHECK(le_read_tx_power(&min, &max) == 0);
	printk("tx-power: LE Read Transmit Power %d..%d dBm\n", min, max);
	CHECK(min == -27 && max == 9);

	/* The advertiser's level before advertising: the BT_CTLR_TX_PWR choice, +4 dBm. */
	CHECK(vs_read(BT_HCI_VS_LL_HANDLE_TYPE_ADV, 0U, &dbm) == 0 && dbm == 4);

	CHECK(bt_le_adv_start(&adv_param, NULL, 0, NULL, 0) == 0);
	ramplev = ramplev_after_events();
	report_ramplev("default", ramplev);
	CHECK(ramplev == RAMPLEV(LEVEL_PLUS_4));

	/* Vendor Write Tx Power Level while advertising: floored, applied from the next event. */
	CHECK(vs_write(BT_HCI_VS_LL_HANDLE_TYPE_ADV, 0U, -10, &dbm) == 0 && dbm == -10);
	CHECK(vs_read(BT_HCI_VS_LL_HANDLE_TYPE_ADV, 0U, &dbm) == 0 && dbm == -10);
	ramplev = ramplev_after_events();
	report_ramplev("-10 dBm", ramplev);
	CHECK(ramplev == RAMPLEV(LEVEL_MINUS_10));

	CHECK(vs_write(BT_HCI_VS_LL_HANDLE_TYPE_ADV, 0U, -100, &dbm) == 0 && dbm == -27);
	ramplev = ramplev_after_events();
	report_ramplev("-100 dBm", ramplev);
	CHECK(ramplev == RAMPLEV(LEVEL_LOWEST));

	/* No preference: back to the default. */
	CHECK(vs_write(BT_HCI_VS_LL_HANDLE_TYPE_ADV, 0U, BT_HCI_VS_LL_TX_POWER_LEVEL_NO_PREF,
		       &dbm) == 0 && dbm == 4);
	ramplev = ramplev_after_events();
	report_ramplev("no preference", ramplev);
	CHECK(ramplev == RAMPLEV(LEVEL_PLUS_4));

	/* The scanner's level is kept for when it scans; no connection, an unknown type. */
	CHECK(vs_write(BT_HCI_VS_LL_HANDLE_TYPE_SCAN, 0U, -8, &dbm) == 0 && dbm == -8);
	CHECK(vs_read(BT_HCI_VS_LL_HANDLE_TYPE_SCAN, 0U, &dbm) == 0 && dbm == -8);
	CHECK(vs_write(BT_HCI_VS_LL_HANDLE_TYPE_CONN, 0U, 0, &dbm) == -EIO);
	CHECK(vs_read(BT_HCI_VS_LL_HANDLE_TYPE_CONN, 0U, &dbm) == -EIO);
	CHECK(vs_write(HANDLE_TYPE_UNKNOWN, 0U, 0, &dbm) == -EIO);
	CHECK(bt_le_adv_stop() == 0);

	/*
	 * Amateur operation (47 CFR 97.313(a)): the operator's 0 dBm caps the advertiser's +4 dBm
	 * and sets the identification's power, which is otherwise the PA's highest level.
	 */
	CHECK(efr32_amateur_tx_power_set(EFR32_AMATEUR_TX_POWER_MAX) == EFR32_AMATEUR_TX_POWER_MAX);
	CHECK(efr32_amateur_tx_power_set(-50) == -27);
	CHECK(efr32_amateur_tx_power_set(0) == 0);
	CHECK(efr32_amateur_enable(&amateur) == 0);
	CHECK(bt_le_adv_start(&adv_param, NULL, 0, NULL, 0) == 0);
	ramplev = ramplev_after_events();
	report_ramplev("amateur +4 dBm, ceiling 0 dBm", ramplev);
	CHECK(ramplev == RAMPLEV(LEVEL_ZERO));
	/* Below the ceiling a role keeps its own level. */
	CHECK(vs_write(BT_HCI_VS_LL_HANDLE_TYPE_ADV, 0U, -10, &dbm) == 0 && dbm == -10);
	ramplev = ramplev_after_events();
	report_ramplev("amateur -10 dBm, ceiling 0 dBm", ramplev);
	CHECK(ramplev == RAMPLEV(LEVEL_MINUS_10));
	CHECK(bt_le_adv_stop() == 0);
	/* The end of the communication: the identification, at the ceiling, is the last frame. */
	CHECK(efr32_amateur_disable() == 0);
	CHECK(efr32_amateur_id_count() == 1U);
	ramplev = sys_read32(MODEM_RAMPLEV);
	report_ramplev("identification", ramplev);
	CHECK(ramplev == RAMPLEV(LEVEL_ZERO));

	/* Again at the lowest ceiling: the advertiser and the identification at level 0. */
	CHECK(efr32_amateur_tx_power_set(-27) == -27);
	CHECK(efr32_amateur_enable(&amateur) == 0);
	CHECK(bt_le_adv_start(&adv_param, NULL, 0, NULL, 0) == 0);
	ramplev = ramplev_after_events();
	report_ramplev("amateur -10 dBm, ceiling -27 dBm", ramplev);
	CHECK(ramplev == RAMPLEV(LEVEL_LOWEST));
	CHECK(bt_le_adv_stop() == 0);
	CHECK(efr32_amateur_disable() == 0);
	CHECK(efr32_amateur_id_count() == 2U);
	ramplev = sys_read32(MODEM_RAMPLEV);
	report_ramplev("identification, ceiling -27 dBm", ramplev);
	CHECK(ramplev == RAMPLEV(LEVEL_LOWEST));

	/* Part 15 again: no ceiling. */
	CHECK(vs_write(BT_HCI_VS_LL_HANDLE_TYPE_ADV, 0U, 9, &dbm) == 0 && dbm == 9);
	CHECK(bt_le_adv_start(&adv_param, NULL, 0, NULL, 0) == 0);
	ramplev = ramplev_after_events();
	report_ramplev("Part 15 +9 dBm", ramplev);
	CHECK(ramplev == RAMPLEV(LEVEL_TOP));
	CHECK(bt_le_adv_stop() == 0);

	printk("tx-power: %s %d/%d\n", failures == 0 ? "PASS" : "FAIL", checks - failures, checks);

	return 0;
}
