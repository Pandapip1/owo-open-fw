/*
 * Amateur-radio operating rules (see amateur_rules.h and docs/amateur-radio.md).
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>

#include "amateur_rules.h"

#define KHZ_PER_MHZ 1000U

/*
 * 47 CFR 97.301(a), ITU Region 2, 13 cm band: 2300-2310 MHz and 2390-2450 MHz, open to
 * Technician, General, Advanced and Amateur Extra licensees (eCFR, current as of 2026-11-01;
 * last amended 91 FR 1430, 2026-01-14). Sharing: 97.303(d), (e), (p).
 */
#define EFR32_AMATEUR_US_13CM_LOW_LOW_KHZ (2300U * KHZ_PER_MHZ)
#define EFR32_AMATEUR_US_13CM_LOW_HIGH_KHZ (2310U * KHZ_PER_MHZ)
#define EFR32_AMATEUR_US_13CM_HIGH_LOW_KHZ (2390U * KHZ_PER_MHZ)
#define EFR32_AMATEUR_US_13CM_HIGH_HIGH_KHZ (2450U * KHZ_PER_MHZ)

/* 47 CFR 97.119(a): "at least every 10 minutes during a communication". */
#define EFR32_AMATEUR_US_ID_INTERVAL_MS (10U * 60U * 1000U)
/* 47 CFR 97.119(b)(1): an automatic CW identification "must not exceed 20 words per minute". */
#define EFR32_AMATEUR_US_CW_MAX_WPM 20U
/* 47 CFR 97.313(b): 1.5 kW PEP, 10 log10(1 500 000 mW) = 61.761 dBm. */
#define EFR32_AMATEUR_US_MAX_POWER_MDBM 61761

static const struct efr32_amateur_band efr32_amateur_us_bands[] = {
	{ EFR32_AMATEUR_US_13CM_LOW_LOW_KHZ, EFR32_AMATEUR_US_13CM_LOW_HIGH_KHZ,
	  "47 CFR 97.301(a) 13 cm" },
	{ EFR32_AMATEUR_US_13CM_HIGH_LOW_KHZ, EFR32_AMATEUR_US_13CM_HIGH_HIGH_KHZ,
	  "47 CFR 97.301(a) 13 cm" },
};

static bool is_upper(char c)
{
	return c >= 'A' && c <= 'Z';
}

static bool is_digit(char c)
{
	return c >= '0' && c <= '9';
}

/*
 * US amateur call signs (FCC sequential call sign system; 47 CFR 2.302 and the ITU prefixes
 * AA-AL, K, N, W allocated to the US): a prefix of one letter K, N or W, or two letters
 * starting with K, N or W, or AA through AL; one numeral; a suffix of one to three letters.
 * Optionally "/" and an indicator of one to four letters or numerals (97.119(c)).
 */
#define US_SUFFIX_MAX 3U
#define US_INDICATOR_MAX 4U

static bool us_callsign_valid(const char *c)
{
	size_t i = 0U;
	size_t n;

	if (c[0] == 'K' || c[0] == 'N' || c[0] == 'W') {
		i = 1U;
		if (is_upper(c[1])) {
			i = 2U;
		}
	} else if (c[0] == 'A' && c[1] >= 'A' && c[1] <= 'L') {
		i = 2U;
	} else {
		return false;
	}
	if (!is_digit(c[i])) {
		return false;
	}
	i++;
	for (n = 0U; is_upper(c[i]); n++) {
		i++;
	}
	if (n == 0U || n > US_SUFFIX_MAX) {
		return false;
	}
	if (c[i] == '\0') {
		return true;
	}
	if (c[i] != '/') {
		return false;
	}
	i++;
	for (n = 0U; is_upper(c[i]) || is_digit(c[i]); n++) {
		i++;
	}
	return n > 0U && n <= US_INDICATOR_MAX && c[i] == '\0';
}

const struct efr32_amateur_admin efr32_amateur_us_part97 = {
	.name = "US 47 CFR Part 97",
	.bands = efr32_amateur_us_bands,
	.band_count = sizeof(efr32_amateur_us_bands) / sizeof(efr32_amateur_us_bands[0]),
	.id_interval_ms = EFR32_AMATEUR_US_ID_INTERVAL_MS,
	.cw_max_wpm = EFR32_AMATEUR_US_CW_MAX_WPM,
	.max_power_mdbm = EFR32_AMATEUR_US_MAX_POWER_MDBM,
	/* 47 CFR 97.113(a)(4): no "messages encoded for the purpose of obscuring their meaning". */
	.encryption_prohibited = true,
	.callsign_valid = us_callsign_valid,
};

bool efr32_amateur_callsign_normalize(const struct efr32_amateur_admin *admin, const char *in,
				      char *out)
{
	size_t i;

	if (in == NULL || in[0] == '\0') {
		return false;
	}
	for (i = 0U; in[i] != '\0'; i++) {
		char c = in[i];

		if (i >= EFR32_AMATEUR_CALLSIGN_MAX) {
			return false;
		}
		if (c >= 'a' && c <= 'z') {
			c = (char)(c - 'a' + 'A');
		}
		out[i] = c;
	}
	out[i] = '\0';
	return admin->callsign_valid(out);
}

bool efr32_amateur_in_band(const struct efr32_amateur_admin *admin, uint32_t centre_khz,
			   uint32_t half_bw_khz)
{
	for (size_t b = 0U; b < admin->band_count; b++) {
		const struct efr32_amateur_band *band = &admin->bands[b];

		if (centre_khz >= band->low_khz + half_bw_khz &&
		    centre_khz + half_bw_khz <= band->high_khz) {
			return true;
		}
	}
	return false;
}

static bool carrier_valid(const struct efr32_amateur_admin *admin,
			  const struct efr32_amateur_hw *hw, uint32_t half_bw_khz, uint32_t khz)
{
	return efr32_amateur_in_band(admin, khz, half_bw_khz) && khz >= hw->synth_min_khz &&
	       khz <= hw->synth_max_khz;
}

int efr32_amateur_plan_check(const struct efr32_amateur_admin *admin,
			     const struct efr32_amateur_hw *hw, uint32_t half_bw_khz,
			     const uint32_t *plan_khz)
{
	for (uint32_t ch = 0U; ch < EFR32_AMATEUR_CHANNELS; ch++) {
		if (!carrier_valid(admin, hw, half_bw_khz, plan_khz[ch])) {
			return (int)ch;
		}
	}
	return -1;
}

/* Core v6.1 Vol 6 Part B 1.4.1: f = 2402 + 2k MHz, k the RF channel; advertising at k = 0, 12, 39. */
#define BLE_RF_CHANNEL0_KHZ (2402U * KHZ_PER_MHZ)
#define BLE_RF_CHANNEL_SPACING_KHZ (2U * KHZ_PER_MHZ)
#define BLE_ADV37_RF_CHANNEL 0U
#define BLE_ADV38_RF_CHANNEL 12U
#define BLE_ADV39_RF_CHANNEL 39U
#define BLE_DATA_CHANNELS_BELOW_ADV38 11U

uint32_t efr32_amateur_ble_channel_khz(uint8_t index)
{
	uint32_t rf;

	if (index == EFR32_AMATEUR_ADV_CHANNEL_FIRST) {
		rf = BLE_ADV37_RF_CHANNEL;
	} else if (index == EFR32_AMATEUR_ADV_CHANNEL_FIRST + 1U) {
		rf = BLE_ADV38_RF_CHANNEL;
	} else if (index == EFR32_AMATEUR_ADV_CHANNEL_FIRST + 2U) {
		rf = BLE_ADV39_RF_CHANNEL;
	} else if (index < BLE_DATA_CHANNELS_BELOW_ADV38) {
		rf = index + 1U;
	} else {
		rf = index + 2U;
	}
	return BLE_RF_CHANNEL0_KHZ + rf * BLE_RF_CHANNEL_SPACING_KHZ;
}

bool efr32_amateur_plan_default(const struct efr32_amateur_admin *admin,
				const struct efr32_amateur_hw *hw, uint32_t half_bw_khz,
				uint32_t *plan_khz)
{
	bool kept[EFR32_AMATEUR_CHANNELS];
	bool any = false;

	for (uint32_t ch = 0U; ch < EFR32_AMATEUR_CHANNELS; ch++) {
		uint32_t khz = efr32_amateur_ble_channel_khz((uint8_t)ch);

		kept[ch] = carrier_valid(admin, hw, half_bw_khz, khz);
		plan_khz[ch] = kept[ch] ? khz : 0U;
		any |= kept[ch];
	}
	/*
	 * Spare carriers: the 2 MHz raster through each band, skipping the standard carriers
	 * already kept, so a moved channel shares its frequency with no kept one where possible.
	 */
	uint32_t spare[EFR32_AMATEUR_CHANNELS];
	size_t spares = 0U;

	for (size_t b = 0U; b < admin->band_count && spares < EFR32_AMATEUR_CHANNELS; b++) {
		const struct efr32_amateur_band *band = &admin->bands[b];
		/* Even MHz from the band's low edge plus the half bandwidth. */
		uint32_t khz = band->low_khz + half_bw_khz;

		khz += (BLE_RF_CHANNEL_SPACING_KHZ - khz % BLE_RF_CHANNEL_SPACING_KHZ) %
		       BLE_RF_CHANNEL_SPACING_KHZ;
		for (; khz + half_bw_khz <= band->high_khz && spares < EFR32_AMATEUR_CHANNELS;
		     khz += BLE_RF_CHANNEL_SPACING_KHZ) {
			bool used = false;

			for (uint32_t ch = 0U; ch < EFR32_AMATEUR_CHANNELS; ch++) {
				used |= kept[ch] && plan_khz[ch] == khz;
			}
			if (!used && carrier_valid(admin, hw, half_bw_khz, khz)) {
				spare[spares++] = khz;
			}
		}
	}
	if (spares == 0U) {
		/* Every valid carrier is a kept standard one: reuse those. */
		for (uint32_t ch = 0U; ch < EFR32_AMATEUR_CHANNELS; ch++) {
			if (kept[ch]) {
				spare[spares++] = plan_khz[ch];
			}
		}
	}
	if (!any && spares == 0U) {
		return false;
	}
	for (uint32_t ch = 0U, next = 0U; ch < EFR32_AMATEUR_CHANNELS; ch++) {
		if (!kept[ch]) {
			plan_khz[ch] = spare[next % spares];
			next++;
		}
	}
	return true;
}

/* ITU-R M.1677-1 Part I, 1.1: letters, figures and the fraction bar. */
struct morse {
	char c;
	const char *code;
};

static const struct morse morse_table[] = {
	{ 'A', ".-" },    { 'B', "-..." },  { 'C', "-.-." },  { 'D', "-.." },   { 'E', "." },
	{ 'F', "..-." },  { 'G', "--." },   { 'H', "...." },  { 'I', ".." },    { 'J', ".---" },
	{ 'K', "-.-" },   { 'L', ".-.." },  { 'M', "--" },    { 'N', "-." },    { 'O', "---" },
	{ 'P', ".--." },  { 'Q', "--.-" },  { 'R', ".-." },   { 'S', "..." },   { 'T', "-" },
	{ 'U', "..-" },   { 'V', "...-" },  { 'W', ".--" },   { 'X', "-..-" },  { 'Y', "-.--" },
	{ 'Z', "--.." },  { '0', "-----" }, { '1', ".----" }, { '2', "..---" }, { '3', "...--" },
	{ '4', "....-" }, { '5', "....." }, { '6', "-...." }, { '7', "--..." }, { '8', "---.." },
	{ '9', "----." }, { '/', "-..-." },
};

/* ITU-R M.1677-1 Part I, 2: dash = 3 dots, element space 1, letter space 3, word space 7. */
#define MORSE_DOT_UNITS 1U
#define MORSE_DASH_UNITS 3U
#define MORSE_ELEMENT_GAP_UNITS 1U
#define MORSE_LETTER_GAP_UNITS 3U
#define MORSE_WORD_GAP_UNITS 7U

static const char *morse_code(char c)
{
	for (size_t i = 0U; i < sizeof(morse_table) / sizeof(morse_table[0]); i++) {
		if (morse_table[i].c == c) {
			return morse_table[i].code;
		}
	}
	return NULL;
}

static bool put_units(uint8_t *units, size_t *n, size_t max, uint32_t count, bool on)
{
	for (uint32_t i = 0U; i < count; i++) {
		if (*n >= max) {
			return false;
		}
		units[*n] = on ? 1U : 0U;
		(*n)++;
	}
	return true;
}

size_t efr32_amateur_morse_units(const char *text, uint8_t *units, size_t max_units)
{
	size_t n = 0U;
	bool after_char = false;

	for (size_t i = 0U; text[i] != '\0'; i++) {
		const char *code;

		if (text[i] == ' ') {
			/* The word space replaces the letter space that would follow. */
			if (after_char && !put_units(units, &n, max_units,
						     MORSE_WORD_GAP_UNITS - MORSE_LETTER_GAP_UNITS,
						     false)) {
				return 0U;
			}
			after_char = false;
			continue;
		}
		code = morse_code(text[i]);
		if (code == NULL) {
			return 0U;
		}
		/* After a word space this completes its seven units. */
		if (n > 0U && !put_units(units, &n, max_units, MORSE_LETTER_GAP_UNITS, false)) {
			return 0U;
		}
		for (size_t e = 0U; code[e] != '\0'; e++) {
			if (e > 0U &&
			    !put_units(units, &n, max_units, MORSE_ELEMENT_GAP_UNITS, false)) {
				return 0U;
			}
			if (!put_units(units, &n, max_units,
				       code[e] == '.' ? MORSE_DOT_UNITS : MORSE_DASH_UNITS, true)) {
				return 0U;
			}
		}
		after_char = true;
	}
	return n;
}

size_t efr32_amateur_cw_bits(const uint8_t *units, size_t unit_count, uint32_t symbols_per_unit,
			     uint8_t *out, size_t out_size)
{
	size_t bits = unit_count * symbols_per_unit;
	size_t octets = (bits + 7U) / 8U;

	if (symbols_per_unit == 0U || octets > out_size) {
		return 0U;
	}
	memset(out, 0, octets);
	for (size_t b = 0U; b < bits; b++) {
		if (units[b / symbols_per_unit] != 0U) {
			out[b / 8U] |= (uint8_t)(1U << (b % 8U));
		}
	}
	return octets;
}

/* PARIS: 50 units a word, so one unit lasts 60 s / (50 x wpm) = 1.2 s / wpm. */
#define PARIS_UNITS_PER_WORD 50U
#define US_PER_MINUTE 60000000U

uint32_t efr32_amateur_cw_unit_us(uint32_t wpm)
{
	return US_PER_MINUTE / (PARIS_UNITS_PER_WORD * wpm);
}

void efr32_amateur_id_init(struct efr32_amateur_id_state *state, uint64_t now_ms)
{
	state->ref_ms = now_ms;
	state->last_tx_ms = now_ms;
	state->unidentified = false;
}

bool efr32_amateur_id_tx(struct efr32_amateur_id_state *state, uint64_t now_ms,
			 uint32_t end_idle_ms)
{
	bool first = !state->unidentified;

	if (first && now_ms - state->ref_ms >= end_idle_ms) {
		/* A new communication: its interval counts from its first transmission. */
		state->ref_ms = now_ms;
	}
	state->unidentified = true;
	state->last_tx_ms = now_ms;
	return first;
}

void efr32_amateur_id_done(struct efr32_amateur_id_state *state, uint64_t now_ms)
{
	state->ref_ms = now_ms;
	state->unidentified = false;
}

bool efr32_amateur_id_next(const struct efr32_amateur_id_state *state,
			   const struct efr32_amateur_admin *admin, uint32_t end_idle_ms,
			   uint32_t lead_ms, uint32_t airtime_ms, struct efr32_amateur_id_due *due)
{
	uint64_t latest;
	uint64_t periodic;
	uint64_t end;

	if (!state->unidentified) {
		return false;
	}
	latest = state->ref_ms + admin->id_interval_ms - airtime_ms;
	periodic = latest > lead_ms ? latest - lead_ms : 0U;
	end = state->last_tx_ms + end_idle_ms;
	due->latest_start_ms = latest;
	due->end_of_communication = end < periodic;
	due->release_ms = due->end_of_communication ? end : periodic;
	return true;
}
