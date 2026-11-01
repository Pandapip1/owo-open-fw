/* Hardware-independent checks of the amateur-radio rules (hal/amateur_rules.c). */
#include <assert.h>
#include <math.h>
#include <string.h>

#include "amateur_rules.h"

static const struct efr32_amateur_admin *us = &efr32_amateur_us_part97;

/* Datasheet tuning range only (BGM220P Table 4.5 FRANGE 2400-2483.5 MHz). */
static const struct efr32_amateur_hw hw_datasheet = { 2400000U, 2483500U, 8200 };
/* A radio that tunes the whole 13 cm allocation. */
static const struct efr32_amateur_hw hw_wide = { 2300000U, 2483500U, 8200 };

#define HALF_BW_KHZ 2000U

static void callsigns(void)
{
	char out[EFR32_AMATEUR_CALLSIGN_MAX + 1U];

	assert(efr32_amateur_callsign_normalize(us, "KD3APL", out));
	assert(strcmp(out, "KD3APL") == 0);
	assert(efr32_amateur_callsign_normalize(us, "kd3apl", out));
	assert(strcmp(out, "KD3APL") == 0);
	assert(efr32_amateur_callsign_normalize(us, "W1AW", out));
	assert(efr32_amateur_callsign_normalize(us, "K1A", out));
	assert(efr32_amateur_callsign_normalize(us, "AA1AA", out));
	assert(efr32_amateur_callsign_normalize(us, "AL7ZZZ", out));
	assert(efr32_amateur_callsign_normalize(us, "N0CALL", out) == false); /* 4-letter suffix */
	assert(efr32_amateur_callsign_normalize(us, "KD3APL/3", out));
	assert(efr32_amateur_callsign_normalize(us, "KD3APL/AG", out));
	assert(!efr32_amateur_callsign_normalize(us, NULL, out));
	assert(!efr32_amateur_callsign_normalize(us, "", out));
	assert(!efr32_amateur_callsign_normalize(us, "AM1A", out));   /* AM is Spain */
	assert(!efr32_amateur_callsign_normalize(us, "A1A", out));
	assert(!efr32_amateur_callsign_normalize(us, "G4ABC", out));  /* not a US prefix */
	assert(!efr32_amateur_callsign_normalize(us, "KD3", out));
	assert(!efr32_amateur_callsign_normalize(us, "KDD3A", out));
	assert(!efr32_amateur_callsign_normalize(us, "KD3APL/", out));
	assert(!efr32_amateur_callsign_normalize(us, "KD3APL/ABCDE", out));
	assert(!efr32_amateur_callsign_normalize(us, "KD3 APL", out));
	assert(!efr32_amateur_callsign_normalize(us, "KD3APL/3/3/3/3", out)); /* too long */
}

static void bands(void)
{
	/* 2390-2450: a 2 MHz guard keeps 2392..2448. */
	assert(efr32_amateur_in_band(us, 2392000U, HALF_BW_KHZ));
	assert(efr32_amateur_in_band(us, 2448000U, HALF_BW_KHZ));
	assert(!efr32_amateur_in_band(us, 2391000U, HALF_BW_KHZ));
	assert(!efr32_amateur_in_band(us, 2449000U, HALF_BW_KHZ));
	assert(!efr32_amateur_in_band(us, 2480000U, HALF_BW_KHZ));
	assert(efr32_amateur_in_band(us, 2305000U, HALF_BW_KHZ));
	assert(!efr32_amateur_in_band(us, 2350000U, HALF_BW_KHZ)); /* 2310-2390 is not amateur */
	assert(!efr32_amateur_in_band(us, 2301000U, HALF_BW_KHZ));
}

static void plans(void)
{
	uint32_t plan[EFR32_AMATEUR_CHANNELS];

	assert(efr32_amateur_ble_channel_khz(37) == 2402000U);
	assert(efr32_amateur_ble_channel_khz(38) == 2426000U);
	assert(efr32_amateur_ble_channel_khz(39) == 2480000U);
	assert(efr32_amateur_ble_channel_khz(0) == 2404000U);
	assert(efr32_amateur_ble_channel_khz(10) == 2424000U);
	assert(efr32_amateur_ble_channel_khz(11) == 2428000U);
	assert(efr32_amateur_ble_channel_khz(36) == 2478000U);

	/* The standard plan is not a valid amateur plan: channel 22 (2450 MHz) is the first out. */
	for (uint8_t ch = 0U; ch < EFR32_AMATEUR_CHANNELS; ch++) {
		plan[ch] = efr32_amateur_ble_channel_khz(ch);
	}
	assert(efr32_amateur_plan_check(us, &hw_wide, HALF_BW_KHZ, plan) == 22);

	/* Default plan, wide radio: identity in band, the rest on spare carriers. */
	assert(efr32_amateur_plan_default(us, &hw_wide, HALF_BW_KHZ, plan));
	assert(efr32_amateur_plan_check(us, &hw_wide, HALF_BW_KHZ, plan) == -1);
	assert(plan[37] == 2402000U && plan[38] == 2426000U);
	assert(plan[0] == 2404000U && plan[21] == 2448000U);
	assert(plan[22] == 2302000U); /* first spare: low band */
	assert(plan[39] != 2480000U);
	for (uint8_t ch = 0U; ch < EFR32_AMATEUR_CHANNELS; ch++) {
		assert(efr32_amateur_in_band(us, plan[ch], HALF_BW_KHZ));
	}

	/* Datasheet radio: spares only from 2400 MHz up. */
	assert(efr32_amateur_plan_default(us, &hw_datasheet, HALF_BW_KHZ, plan));
	assert(efr32_amateur_plan_check(us, &hw_datasheet, HALF_BW_KHZ, plan) == -1);
	assert(plan[22] == 2400000U);
	assert(plan[39] == 2400000U);
	/* An out-of-range carrier for this radio is refused even though it is in band. */
	plan[5] = 2395000U;
	assert(efr32_amateur_plan_check(us, &hw_datasheet, HALF_BW_KHZ, plan) == 5);
	assert(efr32_amateur_plan_check(us, &hw_wide, HALF_BW_KHZ, plan) == -1);
	/* An out-of-band carrier is refused. */
	plan[5] = 2451000U;
	assert(efr32_amateur_plan_check(us, &hw_wide, HALF_BW_KHZ, plan) == 5);
}

static void morse(void)
{
	uint8_t units[256];
	uint8_t bits[512];
	/* KD3APL: -.- -.. ...-- .- .--. .-.. (ITU-R M.1677-1). */
	static const char expect[] =
		"111010111" "000" "1110101" "000" "1010101110111" "000" "10111" "000"
		"10111011101" "000" "101110101";
	size_t n = efr32_amateur_morse_units("KD3APL", units, sizeof(units));

	assert(n == strlen(expect));
	assert(n == 9U + 7U + 13U + 5U + 11U + 9U + 5U * 3U); /* 69 units */
	for (size_t i = 0U; i < n; i++) {
		assert(units[i] == (uint8_t)(expect[i] - '0'));
	}
	/* Word space: 7 units between words. */
	n = efr32_amateur_morse_units("E E", units, sizeof(units));
	assert(n == 1U + 7U + 1U);
	assert(units[0] == 1U && units[8] == 1U);
	for (size_t i = 1U; i < 8U; i++) {
		assert(units[i] == 0U);
	}
	assert(efr32_amateur_morse_units("KD3APL", units, 10U) == 0U);
	assert(efr32_amateur_morse_units("K#", units, sizeof(units)) == 0U);

	/* 20 WPM PARIS: 60 ms a unit; 12 symbols a unit at 200 Bd. */
	assert(efr32_amateur_cw_unit_us(20U) == 60000U);
	n = efr32_amateur_morse_units("KD3APL", units, sizeof(units));
	assert(efr32_amateur_cw_bits(units, n, 12U, bits, sizeof(bits)) == (69U * 12U + 7U) / 8U);
	/* K starts with a dash: 36 symbols on, then 12 off. LSB first. */
	assert(bits[0] == 0xFFU && bits[1] == 0xFFU && bits[2] == 0xFFU && bits[3] == 0xFFU);
	assert(bits[4] == 0x0FU);
	assert(bits[5] == 0x00U);
	assert(efr32_amateur_cw_bits(units, n, 12U, bits, 10U) == 0U);
}

static void id_schedule(void)
{
	struct efr32_amateur_id_state s;
	struct efr32_amateur_id_due due;
	const uint32_t idle = 32000U;
	const uint32_t lead = 30000U;
	const uint32_t air = 5000U;

	efr32_amateur_id_init(&s, 1000U);
	assert(!efr32_amateur_id_next(&s, us, idle, lead, air, &due));
	/* First transmission inside the idle window: same communication as the enable. */
	assert(efr32_amateur_id_tx(&s, 2000U, idle));
	assert(!efr32_amateur_id_tx(&s, 3000U, idle));
	assert(efr32_amateur_id_next(&s, us, idle, lead, air, &due));
	/* Ends by ref + 600 s: starts by 1000 + 600000 - 5000. End of communication at 35 s. */
	assert(due.latest_start_ms == 596000U);
	assert(due.end_of_communication);
	assert(due.release_ms == 3000U + idle);
	/* Transmitting continuously: the periodic release, lead before the latest start. */
	for (uint64_t t = 3000U; t < 590000U; t += 1000U) {
		efr32_amateur_id_tx(&s, t, idle);
	}
	assert(efr32_amateur_id_next(&s, us, idle, lead, air, &due));
	assert(!due.end_of_communication);
	assert(due.release_ms == 596000U - lead);
	assert(due.latest_start_ms - due.release_ms == lead);
	/* Identified at 580 s: nothing due until the next transmission. */
	efr32_amateur_id_done(&s, 580000U);
	assert(!efr32_amateur_id_next(&s, us, idle, lead, air, &due));
	/* A transmission 20 s later continues the communication: deadline from the ID. */
	assert(efr32_amateur_id_tx(&s, 600000U, idle));
	assert(efr32_amateur_id_next(&s, us, idle, lead, air, &due));
	assert(due.latest_start_ms == 580000U + 600000U - air);
	/* After a long silence a transmission starts a new communication. */
	efr32_amateur_id_done(&s, 700000U);
	assert(efr32_amateur_id_tx(&s, 2000000U, idle));
	assert(efr32_amateur_id_next(&s, us, idle, lead, air, &due));
	assert(due.latest_start_ms == 2000000U + 600000U - air);
	/* The interval never exceeds the rule: ID end to next ID end is at most 600 s. */
	assert(due.latest_start_ms + air - 2000000U <= us->id_interval_ms);
}

/*
 * hal/radio.c's OOK MODEM.MODINDEX (EFR32_CW_MODINDEXM/E = 20, -7), replayed from Silicon Labs'
 * configurator (calc_modulator.py calc_modindex_value, calc_shaping.py, calc_demodulator.py
 * calc_interpolation_gain_actual, calc_utilities.py frac2exp).
 */
static void ook_modindex(void)
{
	/* 1M delta table MODEM.SHAPING0/1 = 0x0E050200, 0x53432F1C: COEFF0..7. */
	const unsigned int coeff[8] = { 0x00, 0x02, 0x05, 0x0E, 0x1C, 0x2F, 0x43, 0x53 };
	const unsigned int txbrnum = 24000U; /* 38.4 MHz / (8 x 200 Bd) */
	unsigned int shaping_gain = 0U;
	double interpolation_gain;
	double modindex;
	double best_diff = 1e99;
	int best_m = 0;
	int best_e = 0;

	for (int i = 0; i < 8; i++) {
		shaping_gain = coeff[i] > shaping_gain ? coeff[i] : shaping_gain; /* SHAPING=3 */
	}
	assert(shaping_gain == 83U);
	/* txbrnum >= 16384: / 128 */
	interpolation_gain = txbrnum / 128.0;
	modindex = 150.0 * 16.0 / (shaping_gain * interpolation_gain);
	for (int m = 31; m > 0; m--) {
		int e = (int)lround(log2(modindex / m));
		double diff = fabs(modindex - m * ldexp(1.0, e));

		if (diff < best_diff) {
			best_diff = diff;
			best_m = m;
			best_e = e;
		}
	}
	assert(best_m == 20 && best_e == -7);
}

/*
 * The data identification against BLE-ASI v1 (docs/ble-amateur-station-id.md): the octets of
 * its section 9 example, written out here from the document.
 */
static const unsigned char asi_example_kd3apl[25] = {
	0x18, 0x21, 0x73, 0x00, 0x60, 0x51, 0x11, 0xCA, 0xE0, 0xB1, 0xE0, 0x4D, 0x19,
	0x01, 0x97, 0x00, 0xCF, 0x47, 0x10, 0x4B, 0x44, 0x33, 0x41, 0x50, 0x4C,
};

static void data_id(void)
{
	uint8_t ad[EFR32_AMATEUR_DATA_ID_AD_MAX];
	uint8_t pdu[EFR32_AMATEUR_DATA_ID_PDU_MAX];
	const uint8_t addr[EFR32_AMATEUR_BDADDR_SIZE] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0xC6 };
	char call[EFR32_AMATEUR_CALLSIGN_MAX + 1U];
	bool final = true;

	/* Section 9: AdvData 25 octets, AD length 24, header 0x10. */
	assert(efr32_amateur_data_id_ad("KD3APL", false, ad, sizeof(ad)) == 25U);
	assert(memcmp(ad, asi_example_kd3apl, sizeof(asi_example_kd3apl)) == 0);
	/* Section 4: FINAL is header 0x11, nothing else changes. */
	assert(efr32_amateur_data_id_ad("KD3APL", true, ad, sizeof(ad)) == 25U);
	assert(ad[18] == 0x11);
	assert(memcmp(ad, asi_example_kd3apl, 18) == 0 && memcmp(&ad[19], &asi_example_kd3apl[19], 6) == 0);

	/* Section 3: 19 + N octets; N = 12 fills the 31-octet legacy AdvData. */
	assert(efr32_amateur_data_id_ad("W1AW/KH6", false, ad, sizeof(ad)) == 19U + 8U);
	assert(efr32_amateur_data_id_ad("KD3APL/AB12C", false, ad, sizeof(ad)) == 31U);
	/* Section 5: 1-12 of A-Z, 0-9, '/'; nothing else, and the buffer must hold it. */
	assert(efr32_amateur_data_id_ad("", false, ad, sizeof(ad)) == 0U);
	assert(efr32_amateur_data_id_ad("KD3APL/AB12CD", false, ad, sizeof(ad)) == 0U);
	assert(efr32_amateur_data_id_ad("kd3apl", false, ad, sizeof(ad)) == 0U);
	assert(efr32_amateur_data_id_ad("KD3APL-1", false, ad, sizeof(ad)) == 0U);
	assert(efr32_amateur_data_id_ad("KD3APL", false, ad, 24U) == 0U);

	/* Section 6: the dedicated ADV_NONCONN_IND, AdvData only the structure. */
	assert(efr32_amateur_data_id_pdu("KD3APL", false, addr, true, pdu, sizeof(pdu)) == 33U);
	assert(pdu[0] == 0x42); /* PDU Type 0b0010 ADV_NONCONN_IND, TxAdd (random) */
	assert(pdu[1] == 31U);  /* AdvA 6 + AdvData 25 */
	assert(memcmp(&pdu[2], addr, sizeof(addr)) == 0);
	assert(memcmp(&pdu[8], asi_example_kd3apl, sizeof(asi_example_kd3apl)) == 0);
	assert(efr32_amateur_data_id_pdu("KD3APL", false, addr, false, pdu, sizeof(pdu)) == 33U);
	assert(pdu[0] == 0x02); /* public address: TxAdd clear */
	/* Section 9: 41 octets on LE 1M with preamble, access address and CRC: 328 us. */
	assert(efr32_amateur_le1m_airtime_us(33U) == 328U);

	/* Section 8: a receiver finds it by type and UUID and reads the call sign and FINAL. */
	assert(efr32_amateur_data_id_parse(pdu, 33U, call, &final));
	assert(strcmp(call, "KD3APL") == 0 && !final);
	assert(efr32_amateur_data_id_pdu("KD3APL", true, addr, true, pdu, sizeof(pdu)) == 33U);
	assert(efr32_amateur_data_id_parse(pdu, 33U, call, &final) && final);
	/* Behind another AD structure (Flags), still found. */
	{
		uint8_t two[3 + 25];

		two[0] = 2U;
		two[1] = 0x01U;
		two[2] = 0x06U;
		memcpy(&two[3], asi_example_kd3apl, sizeof(asi_example_kd3apl));
		assert(efr32_amateur_data_id_parse_ad(two, sizeof(two), call, &final));
		assert(strcmp(call, "KD3APL") == 0 && !final);
	}
	/* Another version, another UUID, a bad character: not an identification. */
	memcpy(ad, asi_example_kd3apl, sizeof(asi_example_kd3apl));
	ad[18] = 0x20;
	assert(!efr32_amateur_data_id_parse_ad(ad, 25U, call, &final));
	memcpy(ad, asi_example_kd3apl, sizeof(asi_example_kd3apl));
	ad[2] ^= 0x01;
	assert(!efr32_amateur_data_id_parse_ad(ad, 25U, call, &final));
	memcpy(ad, asi_example_kd3apl, sizeof(asi_example_kd3apl));
	ad[19] = 'k';
	assert(!efr32_amateur_data_id_parse_ad(ad, 25U, call, &final));
	/* A truncated structure. */
	assert(!efr32_amateur_data_id_parse_ad(asi_example_kd3apl, 24U, call, &final));
	/* Not an ADV_NONCONN_IND. */
	assert(efr32_amateur_data_id_pdu("KD3APL", false, addr, true, pdu, sizeof(pdu)) == 33U);
	pdu[0] = 0x40; /* ADV_IND */
	assert(!efr32_amateur_data_id_parse(pdu, 33U, call, &final));
}

/* The identification's own address (BLE-ASI 6): a static random address. */
static void static_addr(void)
{
	const uint8_t draw[6] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };
	const uint8_t zeros[6] = { 0 };
	const uint8_t ones[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x3F };
	const uint8_t ones_type[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
	uint8_t addr[6];

	assert(efr32_amateur_static_addr(draw, addr));
	assert(addr[5] == 0xC6 && memcmp(addr, draw, 5) == 0); /* two MSBs 0b11 */
	/* The 46 random bits must be neither all 0 nor all 1 (Core v6.1 Vol 6 Part B 1.3.2.1). */
	assert(!efr32_amateur_static_addr(zeros, addr));
	assert(!efr32_amateur_static_addr(ones, addr));
	assert(!efr32_amateur_static_addr(ones_type, addr));
}

/*
 * The identification methods (Kconfig EFR32_RADIO_AMATEUR_ID_DATA, EFR32_RADIO_AMATEUR_ID_CW):
 * every combination's span, from the first TXEN to the end of the last frame.
 */
static void id_methods(void)
{
	const struct efr32_amateur_id_timing t = {
		.tx_ready_us = 100U, /* placeholder figures; only the arithmetic is checked */
		.spacing_us = 10000U,
		.data_pdu_us = 328U,
		.cw_us = 4320000U,
	};
	struct efr32_amateur_id_timing tight = t;

	/* CW only (the default): the Morse frame after its TX ramp. */
	assert(efr32_amateur_id_span_us(EFR32_AMATEUR_ID_METHOD_CW, &t) == 100U + 4320000U);
	/* Data only: three PDUs, each within the spacing of the previous TXEN. */
	assert(efr32_amateur_id_span_us(EFR32_AMATEUR_ID_METHOD_DATA, &t) ==
	       2U * 10000U + 100U + 328U);
	/* Both: the three PDUs, then the Morse frame within the spacing of the last. */
	assert(efr32_amateur_id_span_us(EFR32_AMATEUR_ID_METHODS_ALL, &t) ==
	       3U * 10000U + 100U + 4320000U);
	/* None, or an unknown method: refused. */
	assert(efr32_amateur_id_span_us(0U, &t) == 0U);
	assert(efr32_amateur_id_span_us(EFR32_AMATEUR_ID_METHODS_ALL + 1U, &t) == 0U);
	/* A data PDU that does not fit its spacing cannot be followed: refused. */
	tight.spacing_us = 400U;
	assert(efr32_amateur_id_span_us(EFR32_AMATEUR_ID_METHOD_DATA, &tight) == 0U);
}

int main(void)
{
	data_id();
	static_addr();
	id_methods();
	ook_modindex();
	callsigns();
	bands();
	plans();
	morse();
	id_schedule();
	return 0;
}
