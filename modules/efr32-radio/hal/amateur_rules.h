/*
 * Amateur-radio operating rules for the EFR32 radio: band tables, call-sign
 * format, channel plan validation, Morse identification and the station
 * identification schedule.
 *
 * Hardware independent (no Zephyr, no MMIO): hal/amateur.c binds it to the
 * controller and hal/radio.c to the radio; tests/efr32-radio-contract checks it on
 * the host. Rule text and the reasoning behind every constant: docs/amateur-radio.md.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef EFR32_RADIO_AMATEUR_RULES_H_
#define EFR32_RADIO_AMATEUR_RULES_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Bluetooth LE physical channels (Core v6.1 Vol 6 Part B 1.4.1): 0..36 data, 37..39 advertising. */
#define EFR32_AMATEUR_CHANNELS 40U
#define EFR32_AMATEUR_ADV_CHANNEL_FIRST 37U

/* A call sign with an optional "/indicator" (47 CFR 97.119(c)), as stored. */
#define EFR32_AMATEUR_CALLSIGN_MAX 12U

/* One frequency segment an administration allocates to the amateur service. */
struct efr32_amateur_band {
	uint32_t low_khz;
	uint32_t high_khz;
	/* Rule that allocates it, for reports. */
	const char *rule;
};

/*
 * One administration's rules, as far as this firmware enforces them. A table, so another
 * administration is another instance; only US Part 97 is provided.
 */
struct efr32_amateur_admin {
	const char *name;
	const struct efr32_amateur_band *bands;
	size_t band_count;
	/* Longest time between identifications during a communication. */
	uint32_t id_interval_ms;
	/* Fastest Morse speed for an automatic identification. */
	uint8_t cw_max_wpm;
	/* Transmitter power ceiling, milli-dBm (dBm x 1000). */
	int32_t max_power_mdbm;
	/* The administration forbids encryption that obscures meaning. */
	bool encryption_prohibited;
	/* Call-sign format check on an upper-case string. */
	bool (*callsign_valid)(const char *callsign);
};

extern const struct efr32_amateur_admin efr32_amateur_us_part97;

/* What the radio can do, from its own measurements (hal/radio.c). */
struct efr32_amateur_hw {
	/* Carriers at which the synthesizer was measured to calibrate and lock. */
	uint32_t synth_min_khz;
	uint32_t synth_max_khz;
	/* Highest transmitter output power the radio can produce, milli-dBm. */
	int32_t max_power_mdbm;
};

/*
 * Copy @p in to @p out (EFR32_AMATEUR_CALLSIGN_MAX + 1 bytes) in upper case and check its
 * format against @p admin. Returns false for NULL, empty, too long or malformed input.
 */
bool efr32_amateur_callsign_normalize(const struct efr32_amateur_admin *admin, const char *in,
				      char *out);

/* Whether a carrier at @p centre_khz keeps @p half_bw_khz either side inside one band. */
bool efr32_amateur_in_band(const struct efr32_amateur_admin *admin, uint32_t centre_khz,
			   uint32_t half_bw_khz);

/*
 * Check a channel plan (EFR32_AMATEUR_CHANNELS carriers, kHz, by physical channel index):
 * every carrier in a band with @p half_bw_khz either side and inside the radio's measured
 * synthesizer range. Returns -1 when valid, else the first offending channel index.
 */
int efr32_amateur_plan_check(const struct efr32_amateur_admin *admin,
			     const struct efr32_amateur_hw *hw, uint32_t half_bw_khz,
			     const uint32_t *plan_khz);

/* The Bluetooth LE carrier of physical channel @p index (Core v6.1 Vol 6 Part B 1.4.1), kHz. */
uint32_t efr32_amateur_ble_channel_khz(uint8_t index);

/*
 * The default plan: a channel whose standard carrier is valid keeps it; every other channel
 * takes the valid carriers on the 2 MHz raster that no channel keeps, in turn. Returns
 * false when no valid carrier exists.
 */
bool efr32_amateur_plan_default(const struct efr32_amateur_admin *admin,
				const struct efr32_amateur_hw *hw, uint32_t half_bw_khz,
				uint32_t *plan_khz);

/*
 * International Morse code (ITU-R M.1677-1): the key pattern of @p text in dot units, one
 * bit per unit, 1 = key down. A dot is one unit on, a dash three, one unit off between
 * elements, three between characters, seven at a space. Returns the number of units, or 0
 * when a character has no Morse code or the pattern exceeds @p max_units.
 */
size_t efr32_amateur_morse_units(const char *text, uint8_t *units, size_t max_units);

/*
 * The over-the-air bit stream of a Morse pattern: each unit repeated @p symbols_per_unit
 * times, packed least significant bit first (the FRC's BLE bit order). Returns the number of
 * octets, or 0 when @p out is too small.
 */
size_t efr32_amateur_cw_bits(const uint8_t *units, size_t unit_count, uint32_t symbols_per_unit,
			     uint8_t *out, size_t out_size);

/* Duration of one Morse unit at @p wpm words per minute, PARIS standard (50 units a word). */
uint32_t efr32_amateur_cw_unit_us(uint32_t wpm);

/*
 * Station identification schedule (47 CFR 97.119(a)): an identification at the end of each
 * communication and at least every id_interval_ms during one. Times are milliseconds of a
 * monotonic clock.
 */
struct efr32_amateur_id_state {
	/* End of the last identification, or the start of the current communication. */
	uint64_t ref_ms;
	/* Last transmission other than an identification. */
	uint64_t last_tx_ms;
	/* Something was transmitted since the last identification. */
	bool unidentified;
};

/* An identification is needed: release it at release_ms, start it by latest_start_ms. */
struct efr32_amateur_id_due {
	uint64_t release_ms;
	uint64_t latest_start_ms;
	/* The release is the end of a communication, not the periodic one. */
	bool end_of_communication;
};

void efr32_amateur_id_init(struct efr32_amateur_id_state *state, uint64_t now_ms);

/*
 * A transmission other than an identification at @p now_ms. A transmission after
 * @p end_idle_ms of silence starts a new communication. Returns true when this is the first
 * transmission since the last identification (the schedule changed).
 */
bool efr32_amateur_id_tx(struct efr32_amateur_id_state *state, uint64_t now_ms,
			 uint32_t end_idle_ms);

/* An identification ended at @p now_ms. */
void efr32_amateur_id_done(struct efr32_amateur_id_state *state, uint64_t now_ms);

/*
 * The next identification, if one is needed: the periodic one released @p lead_ms before its
 * latest start, or the end-of-communication one @p end_idle_ms after the last transmission,
 * whichever is earlier. @p airtime_ms is the identification's own duration: it must end by
 * the interval. Returns false when nothing unidentified was transmitted.
 */
bool efr32_amateur_id_next(const struct efr32_amateur_id_state *state,
			   const struct efr32_amateur_admin *admin, uint32_t end_idle_ms,
			   uint32_t lead_ms, uint32_t airtime_ms, struct efr32_amateur_id_due *due);

#endif /* EFR32_RADIO_AMATEUR_RULES_H_ */
