/*
 * Amateur-radio workload for modules/efr32-radio (docs/amateur-radio.md).
 *
 * The advertiser checks the amateur API (call-sign validation, out-of-band refusal, the switch
 * itself and the encryption policy), switches to amateur rules with a non-standard plan and
 * advertises its call sign as its Complete Local Name; after AMATEUR_PORT_ADV_SECONDS it stops
 * and returns to Part 15, which sends the closing Morse identification. The scanner switches to
 * the same plan and prints the names it receives.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "hal/efr32/amateur.h"

/* include/ll.h (BT_CTLR_LE_ENC_POLICY), implemented by the radio module. */
bool ll_enc_policy_permitted(void);

#define KHZ_PER_MHZ 1000U
/* Core v6.1 Vol 6 Part B 1.4.1: RF channel k at 2402 + 2k MHz; advertising at k = 0, 12, 39. */
#define RF0_KHZ (2402U * KHZ_PER_MHZ)
#define RF_SPACING_KHZ (2U * KHZ_PER_MHZ)
#define ADV37 37U
#define DATA_BELOW_ADV38 11U
#define RF_ADV38 12U
#define RF_ADV39 39U

static uint32_t ble_khz(uint32_t ch)
{
	uint32_t rf = ch == ADV37 ? 0U :
		      ch == ADV37 + 1U ? RF_ADV38 :
		      ch == ADV37 + 2U ? RF_ADV39 :
		      ch < DATA_BELOW_ADV38 ? ch + 1U : ch + 2U;

	return RF0_KHZ + rf * RF_SPACING_KHZ;
}

/*
 * The odd-MHz test plan: standard + 1 MHz up to 2447 MHz, else 47 MHz lower (2401-2433 MHz).
 * 2447 MHz is the highest carrier a 2 MHz guard keeps inside 2450 MHz with 1 MHz to spare.
 */
#define ODD_OFFSET_KHZ (1U * KHZ_PER_MHZ)
#define ODD_TOP_KHZ (2447U * KHZ_PER_MHZ)
#define ODD_FOLD_KHZ (47U * KHZ_PER_MHZ)
static uint32_t plan[EFR32_AMATEUR_PLAN_CHANNELS];

static void plan_build(void)
{
	for (uint32_t ch = 0U; ch < EFR32_AMATEUR_PLAN_CHANNELS; ch++) {
		uint32_t khz = ble_khz(ch) + ODD_OFFSET_KHZ;

		plan[ch] = khz <= ODD_TOP_KHZ ? khz : ble_khz(ch) - ODD_FOLD_KHZ;
	}
}

static const uint32_t *test_plan(void)
{
#if defined(CONFIG_AMATEUR_PORT_PLAN_SINGLE)
	static uint32_t single[EFR32_AMATEUR_PLAN_CHANNELS];

	for (uint32_t ch = 0U; ch < EFR32_AMATEUR_PLAN_CHANNELS; ch++) {
		single[ch] = CONFIG_AMATEUR_PORT_SINGLE_KHZ;
	}
	return single;
#else
	return IS_ENABLED(CONFIG_AMATEUR_PORT_PLAN_ODD) ? plan : NULL;
#endif
}

static int checks;
static int failures;

#define CHECK(cond)                                                                       \
	do {                                                                              \
		checks++;                                                                 \
		if (!(cond)) {                                                            \
			failures++;                                                       \
			printk("amateur-selftest: FAIL line %d: %s\n", __LINE__, #cond);  \
		}                                                                         \
	} while (0)

static int enable_with(const char *call, const uint32_t *p)
{
	struct efr32_amateur_config config = {
		.callsign = call,
		.plan_khz = p,
	};

	return efr32_amateur_enable(&config);
}

static void selftest(void)
{
	uint32_t bad[EFR32_AMATEUR_PLAN_CHANNELS];

	/* Part 15 until switched. */
	CHECK(!efr32_amateur_active());
	CHECK(efr32_amateur_callsign() == NULL);
	CHECK(ll_enc_policy_permitted());
	CHECK(efr32_amateur_disable() == -EALREADY);
	CHECK(efr32_amateur_end_of_communication() == -EALREADY);
	CHECK(efr32_amateur_enable(NULL) == -EINVAL);

	/* Call signs: empty, missing, malformed and foreign are refused. */
	CHECK(enable_with(NULL, test_plan()) == -EINVAL);
	CHECK(enable_with("", test_plan()) == -EINVAL);
	CHECK(enable_with("G4ABC", test_plan()) == -EINVAL);
	CHECK(enable_with("KD3APL!", test_plan()) == -EINVAL);
	CHECK(enable_with("KD3APLX/ABCDE", test_plan()) == -EINVAL);
	CHECK(!efr32_amateur_active());

	/* The standard plan: 2450 MHz (channel 22) is the first carrier outside 13 cm. */
	for (uint32_t ch = 0U; ch < EFR32_AMATEUR_PLAN_CHANNELS; ch++) {
		bad[ch] = ble_khz(ch);
	}
	CHECK(enable_with(CONFIG_AMATEUR_PORT_CALLSIGN, bad) == -ERANGE);
	CHECK(efr32_amateur_last_error_channel() == 22);
	/* 2350 MHz is between the two 13 cm segments. */
	memcpy(bad, plan, sizeof(bad));
	bad[ADV37] = 2350U * KHZ_PER_MHZ;
	CHECK(enable_with(CONFIG_AMATEUR_PORT_CALLSIGN, bad) == -ERANGE);
	CHECK(efr32_amateur_last_error_channel() == (int)ADV37);
	/* 2449 MHz: in band, but its 2 MHz guard crosses 2450 MHz. */
	bad[ADV37] = 2449U * KHZ_PER_MHZ;
	CHECK(enable_with(CONFIG_AMATEUR_PORT_CALLSIGN, bad) == -ERANGE);
	CHECK(!efr32_amateur_active());

	/* The switch. */
	CHECK(enable_with(CONFIG_AMATEUR_PORT_CALLSIGN, test_plan()) == 0);
	CHECK(efr32_amateur_active());
	CHECK(efr32_amateur_callsign() != NULL &&
	      strcmp(efr32_amateur_callsign(), CONFIG_AMATEUR_PORT_CALLSIGN) == 0);
	CHECK(enable_with(CONFIG_AMATEUR_PORT_CALLSIGN, test_plan()) == -EALREADY);
	/* 47 CFR 97.113(a)(4): the controller refuses encryption under amateur rules. */
	CHECK(!ll_enc_policy_permitted());

	/* Nothing transmitted yet: back to Part 15 without an identification. */
	CHECK(efr32_amateur_disable() == 0);
	CHECK(!efr32_amateur_active());
	CHECK(ll_enc_policy_permitted());
	CHECK(efr32_amateur_id_count() == 0U);

	printk("amateur-selftest: %s %d/%d\n", failures == 0 ? "PASS" : "FAIL", checks - failures,
	       checks);
}

static void amateur_on(void)
{
	int err;

	if (!IS_ENABLED(CONFIG_AMATEUR_PORT_AMATEUR)) {
		printk("amateur: Part 15 rules\n");
		return;
	}
	err = enable_with(CONFIG_AMATEUR_PORT_CALLSIGN, test_plan());
	const uint32_t *p = test_plan();

	printk("amateur: enable %d (ch37 %u kHz, ch38 %u kHz, ch39 %u kHz)\n", err,
	       p != NULL ? p[ADV37] : 0U, p != NULL ? p[ADV37 + 1U] : 0U,
	       p != NULL ? p[ADV37 + 2U] : 0U);
}

#if defined(CONFIG_AMATEUR_PORT_SYNTH_PROBE) || defined(CONFIG_AMATEUR_PORT_SYNTH_MONITOR)
/*
 * Synthesizer probe and monitor (docs/radio-tables.md 7.85). Register map: Silicon Labs' public
 * xG22 register model (SYNTH_S_register.py / _field.py; RAC as in hal/radio.c), non-secure
 * aliases. The probe samples in RXSEARCH (receive only), the monitor in TX.
 */
#define RAC_STATUS 0xB802000CUL
#define RAC_STATUS_STATE_SHIFT 24U
#define RAC_STATUS_STATE_MASK 0xFU
#define RAC_STATE_RXSEARCH 2U
#define RAC_STATE_TX 9U
#define SYNTH_STATUS 0xB8018008UL
#define SYNTH_STATUS_INLOCK BIT(0)
#define SYNTH_STATUS_CAPCALERROR BIT(14)
#define SYNTH_FREQ 0xB8018034UL
#define SYNTH_FREQ_MASK 0x0FFFFFFFU
#define SYNTH_VCOTUNING 0xB801804CUL
#define SYNTH_VCOTUNING_MASK 0xFFFFU
/* SYNTH.FREQ unit: f_HFXO / 2^19 (hal/radio.c). */
#define HFXO_HZ 38400000ULL
#define SYNTH_FREQ_FRACTION_BITS 19U
#define HZ_PER_KHZ 1000U
#define PROBE_SAMPLE_US 500U
#define PROBE_PER_BLOCK 3U

struct probe_point {
	uint32_t khz;
	uint32_t samples;
	uint32_t inlock;
	uint32_t capcal_error;
	uint16_t tune_min;
	uint16_t tune_max;
};

static struct probe_point probe_block[PROBE_PER_BLOCK];
static uint32_t probe_state = IS_ENABLED(CONFIG_AMATEUR_PORT_SYNTH_PROBE) ? RAC_STATE_RXSEARCH :
									      RAC_STATE_TX;

static uint32_t reg(uintptr_t address)
{
	return *(volatile uint32_t *)address;
}

static void probe_sample(struct k_timer *timer)
{
	uint32_t state = (reg(RAC_STATUS) >> RAC_STATUS_STATE_SHIFT) & RAC_STATUS_STATE_MASK;
	uint64_t word = reg(SYNTH_FREQ) & SYNTH_FREQ_MASK;
	uint32_t khz = (uint32_t)(((word * HFXO_HZ) >> SYNTH_FREQ_FRACTION_BITS) / HZ_PER_KHZ);

	ARG_UNUSED(timer);
	if (state != probe_state) {
		return;
	}
	for (uint32_t i = 0U; i < PROBE_PER_BLOCK; i++) {
		struct probe_point *p = &probe_block[i];
		/* FREQ rounds to 73 Hz: match to the kHz. */
		if (p->khz != 0U && (khz + 1U >= p->khz && khz <= p->khz + 1U)) {
			uint32_t status = reg(SYNTH_STATUS);
			uint16_t tune = (uint16_t)(reg(SYNTH_VCOTUNING) & SYNTH_VCOTUNING_MASK);

			p->samples++;
			p->inlock += (status & SYNTH_STATUS_INLOCK) != 0U;
			p->capcal_error += (status & SYNTH_STATUS_CAPCALERROR) != 0U;
			p->tune_min = p->samples == 1U ? tune : MIN(p->tune_min, tune);
			p->tune_max = p->samples == 1U ? tune : MAX(p->tune_max, tune);
			break;
		}
	}
}

static K_TIMER_DEFINE(probe_timer, probe_sample, NULL);

static void probe_print(const char *what, int err)
{
	for (uint32_t i = 0U; i < PROBE_PER_BLOCK; i++) {
		const struct probe_point *q = &probe_block[i];

		if (q->khz == 0U || (i > 0U && q->khz == probe_block[0].khz)) {
			continue;
		}
		printk("%s khz=%u err=%d n=%u inlock=%u capcalerr=%u tune=0x%04x..0x%04x\n", what,
		       q->khz, err, q->samples, q->inlock, q->capcal_error, q->tune_min,
		       q->tune_max);
	}
}
#endif

#if defined(CONFIG_AMATEUR_PORT_ADVERTISER)
static void advertise(void)
{
	static const char name[] = CONFIG_AMATEUR_PORT_CALLSIGN;
	/* The call sign as the Complete Local Name (Core Specification Supplement A 1.2). */
	const struct bt_data ad[] = {
		BT_DATA(BT_DATA_NAME_COMPLETE, name, sizeof(name) - 1U),
	};
	/* 100 ms, non-connectable, identity address. */
	const struct bt_le_adv_param param =
		BT_LE_ADV_PARAM_INIT(BT_LE_ADV_OPT_USE_IDENTITY, BT_GAP_ADV_FAST_INT_MIN_2,
				     BT_GAP_ADV_FAST_INT_MIN_2 + 8U, NULL);
	int err = bt_le_adv_start(&param, ad, ARRAY_SIZE(ad), NULL, 0);

	printk("adv_start: %d\n", err);
	if (err != 0 || CONFIG_AMATEUR_PORT_ADV_SECONDS == 0) {
		return;
	}
#if defined(CONFIG_AMATEUR_PORT_SYNTH_MONITOR)
	/* The synthesizer while transmitting on the single carrier, identification included. */
	probe_block[0].khz = CONFIG_AMATEUR_PORT_SINGLE_KHZ;
	k_timer_start(&probe_timer, K_USEC(PROBE_SAMPLE_US), K_USEC(PROBE_SAMPLE_US));
#endif
	k_sleep(K_SECONDS(CONFIG_AMATEUR_PORT_ADV_SECONDS));
	err = bt_le_adv_stop();
	printk("adv_stop: %d\n", err);
	if (efr32_amateur_active()) {
		/* The end of the communication: identify, then Part 15. */
		err = efr32_amateur_disable();
		printk("amateur: disable %d, identifications %u, active %d\n", err,
		       efr32_amateur_id_count(), efr32_amateur_active());
	}
#if defined(CONFIG_AMATEUR_PORT_SYNTH_MONITOR)
	k_timer_stop(&probe_timer);
	probe_print("tx-monitor", err);
#endif
}
#endif

#if defined(CONFIG_AMATEUR_PORT_SCANNER)
static bool print_name(struct bt_data *data, void *user_data)
{
	ARG_UNUSED(user_data);
	if (data->type == BT_DATA_NAME_COMPLETE || data->type == BT_DATA_NAME_SHORTENED) {
		printk("rx_name: %.*s\n", data->data_len, (const char *)data->data);
		return false;
	}
	return true;
}

static void scan_recv(const struct bt_le_scan_recv_info *info, struct net_buf_simple *buf)
{
	ARG_UNUSED(info);
	bt_data_parse(buf, print_name, NULL);
}

static struct bt_le_scan_cb scan_callbacks = {
	.recv = scan_recv,
};

#if defined(CONFIG_AMATEUR_PORT_SYNTH_PROBE)
static void probe(void)
{
	const struct bt_le_scan_param param = {
		.type = BT_LE_SCAN_TYPE_PASSIVE,
		.interval = BT_GAP_SCAN_FAST_INTERVAL_MIN,
		.window = BT_GAP_SCAN_FAST_INTERVAL_MIN,
	};
	uint32_t khz = CONFIG_AMATEUR_PORT_PROBE_MIN_KHZ;

	printk("probe: step %u kHz, %u..%u kHz, receive only\n", CONFIG_AMATEUR_PORT_PROBE_STEP_KHZ,
	       CONFIG_AMATEUR_PORT_PROBE_MIN_KHZ, CONFIG_AMATEUR_PORT_PROBE_MAX_KHZ);
	k_timer_start(&probe_timer, K_USEC(PROBE_SAMPLE_US), K_USEC(PROBE_SAMPLE_US));
	while (khz <= CONFIG_AMATEUR_PORT_PROBE_MAX_KHZ) {
		uint32_t p[EFR32_AMATEUR_PLAN_CHANNELS];
		int err;

		memset(probe_block, 0, sizeof(probe_block));
		for (uint32_t i = 0U; i < PROBE_PER_BLOCK; i++) {
			uint32_t f = khz + i * CONFIG_AMATEUR_PORT_PROBE_STEP_KHZ;

			probe_block[i].khz = f <= CONFIG_AMATEUR_PORT_PROBE_MAX_KHZ ? f : khz;
		}
		for (uint32_t ch = 0U; ch < EFR32_AMATEUR_PLAN_CHANNELS; ch++) {
			p[ch] = probe_block[0].khz;
		}
		for (uint32_t i = 0U; i < PROBE_PER_BLOCK; i++) {
			p[ADV37 + i] = probe_block[i].khz;
		}
		err = enable_with(CONFIG_AMATEUR_PORT_CALLSIGN, p);
		if (err == 0) {
			err = bt_le_scan_start(&param, NULL);
			k_sleep(K_MSEC(CONFIG_AMATEUR_PORT_PROBE_DWELL_MS));
			(void)bt_le_scan_stop();
			(void)efr32_amateur_disable();
		}
		probe_print("probe", err);
		khz += PROBE_PER_BLOCK * CONFIG_AMATEUR_PORT_PROBE_STEP_KHZ;
	}
	k_timer_stop(&probe_timer);
	printk("probe: done\n");
}
#endif

static void scan(void)
{
#if defined(CONFIG_AMATEUR_PORT_SYNTH_PROBE)
	probe();
	return;
#endif
	/* Continuous passive scanning: 100 ms window and interval. */
	const struct bt_le_scan_param param = {
		.type = BT_LE_SCAN_TYPE_PASSIVE,
		.interval = BT_GAP_SCAN_FAST_INTERVAL_MIN,
		.window = BT_GAP_SCAN_FAST_INTERVAL_MIN,
	};

	bt_le_scan_cb_register(&scan_callbacks);
	printk("scan_start: %d\n", bt_le_scan_start(&param, NULL));
}
#endif

int main(void)
{
	int err;

	plan_build();
	err = bt_enable(NULL);
	printk("bt_enable: %d\n", err);
	if (err != 0) {
		return 0;
	}
	if (IS_ENABLED(CONFIG_AMATEUR_PORT_SELFTEST)) {
		selftest();
	}
	if (!IS_ENABLED(CONFIG_AMATEUR_PORT_SYNTH_PROBE)) {
		amateur_on();
	}
#if defined(CONFIG_AMATEUR_PORT_ADVERTISER)
	advertise();
#else
	scan();
#endif
	return 0;
}
