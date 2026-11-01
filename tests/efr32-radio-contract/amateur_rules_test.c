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

int main(void)
{
	ook_modindex();
	callsigns();
	bands();
	plans();
	morse();
	id_schedule();
	return 0;
}
