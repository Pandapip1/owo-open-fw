/*
 * Amateur-radio operation of the EFR32 Bluetooth radio (CONFIG_EFR32_RADIO_AMATEUR).
 *
 * The radio starts, and stays, under the unlicensed rules (US 47 CFR Part 15: the Bluetooth
 * channels, 2402-2480 MHz). efr32_amateur_enable() switches it to an amateur administration's
 * rules for a licensed station: the carrier follows a channel plan inside that administration's
 * bands, encryption is refused, and the station's call sign is sent at least every
 * identification interval during a communication and at its end: in Morse code by keying the
 * carrier (the default), and/or (CONFIG_EFR32_RADIO_AMATEUR_ID_DATA, legal basis unconfirmed) as
 * a BLE Amateur Station Identification frame (docs/ble-amateur-station-id.md).
 * efr32_amateur_disable() identifies a last time and returns to Part 15. Rules, rule text and
 * what is and is not measured: docs/amateur-radio.md.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef EFR32_RADIO_AMATEUR_H_
#define EFR32_RADIO_AMATEUR_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct efr32_amateur_admin;

/* United States, 47 CFR Part 97, 13 cm band. */
extern const struct efr32_amateur_admin efr32_amateur_us_part97;

/* Bluetooth LE physical channels a plan maps: data 0..36, advertising 37..39. */
#define EFR32_AMATEUR_PLAN_CHANNELS 40U

struct efr32_amateur_config {
	/* The licensee's call sign, optionally "/indicator"; case-insensitive. */
	const char *callsign;
	/* Rules to operate under; NULL selects &efr32_amateur_us_part97. */
	const struct efr32_amateur_admin *admin;
	/*
	 * Carrier of each physical channel in kHz, indexed by channel (0..39); NULL selects the
	 * default plan: a channel whose Bluetooth carrier is inside the bands keeps it, the others
	 * move to spare in-band carriers.
	 */
	const uint32_t *plan_khz;
};

/**
 * @brief Operate under amateur rules.
 *
 * @retval 0 Amateur operation; the radio uses the plan from its next event.
 * @retval -EINVAL Call sign missing or malformed for the administration.
 * @retval -ERANGE A carrier is outside the administration's bands (with the emission's
 *         bandwidth) or outside the radio's measured synthesizer range;
 *         efr32_amateur_last_error_channel() names the channel.
 * @retval -EBUSY A connection exists; switch rules with no connection.
 * @retval -EADDRNOTAVAIL The data identification (CONFIG_EFR32_RADIO_AMATEUR_ID_DATA) is to be
 *         sent from the station's address and the controller has none.
 * @retval -EALREADY Already under amateur rules.
 */
int efr32_amateur_enable(const struct efr32_amateur_config *config);

/**
 * @brief Return to Part 15, after identifying if anything was transmitted since the last
 *        identification (the end of the communication).
 *
 * Blocks until that identification has been sent.
 *
 * @retval 0 Part 15.
 * @retval -EBUSY A connection exists.
 * @retval -EALREADY Not under amateur rules.
 */
int efr32_amateur_disable(void);

/** @brief Whether the radio operates under amateur rules. */
bool efr32_amateur_active(void);

/** @brief The call sign in use (upper case), or NULL under Part 15. */
const char *efr32_amateur_callsign(void);

/** @brief The channel a failed efr32_amateur_enable() refused, or -1. */
int efr32_amateur_last_error_channel(void);

/**
 * @brief A communication has ended: identify now rather than after the idle timeout.
 *
 * @retval 0 Identification requested, or none needed.
 * @retval -EALREADY Not under amateur rules.
 */
int efr32_amateur_end_of_communication(void);

/** @brief Number of identifications sent since boot. */
uint32_t efr32_amateur_id_count(void);

/* efr32_amateur_tx_power_set(): no ceiling, the radio's highest power level. */
#define EFR32_AMATEUR_TX_POWER_MAX INT8_MAX

/**
 * @brief Set the station's transmitter power for amateur operation (47 CFR 97.313(a)).
 * While amateur rules are in force, every transmission from the radio's next event is capped
 * at it: each Bluetooth role's own level (BT_CTLR_TX_PWR, the vendor Write Tx Power Level
 * command) and the identifications, which otherwise use the radio's highest level. Kept across
 * efr32_amateur_disable() and efr32_amateur_enable(); EFR32_AMATEUR_TX_POWER_MAX until set.
 * The dBm are Silicon Labs' published outputs of the power amplifier's levels, not measured on
 * this hardware (docs/tx-power.md).
 * @param dbm Highest output in dBm, or EFR32_AMATEUR_TX_POWER_MAX.
 * @return The output of the level selected: the highest not above @p dbm (the lowest level if
 *         none is), or EFR32_AMATEUR_TX_POWER_MAX.
 */
int8_t efr32_amateur_tx_power_set(int8_t dbm);

#ifdef __cplusplus
}
#endif

#endif /* EFR32_RADIO_AMATEUR_H_ */
