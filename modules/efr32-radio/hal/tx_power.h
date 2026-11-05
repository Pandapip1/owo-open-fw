/*
 * Transmit power of the EFR32BG22's 6 dBm power amplifier: the controller's dBm requests mapped
 * to the PA's power level, which hal/radio.c programs into MODEM.RAMPLEV.
 *
 * Hardware independent (no Zephyr, no MMIO): tests/efr32-radio-contract checks it on the host.
 * Register evidence, the dBm figures and what is unverified: docs/tx-power.md.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef EFR32_RADIO_TX_POWER_H_
#define EFR32_RADIO_TX_POWER_H_

#include <stdint.h>

/*
 * The 6 dBm PA's power levels: 0 to 127 (Silicon Labs rail_chip_specific.h,
 * RAIL_TX_POWER_LEVEL_2P4_HP_MIN/MAX for Series 2 configuration 2, the xG22).
 */
#define EFR32_PA6_LEVEL_MIN 0U
#define EFR32_PA6_LEVEL_MAX 127U

/* No ceiling: the hardware's highest level is available (efr32_tx_power_limit_set()). */
#define EFR32_TX_POWER_NO_LIMIT INT8_MAX

/* A PA level and the output Silicon Labs publishes for it, in tenths of a dBm. */
struct efr32_tx_power_step {
	uint8_t pa_level;
	int16_t ddbm;
};

/* The PA levels with a published output, ascending in level and in output. */
extern const struct efr32_tx_power_step efr32_tx_power_steps[];
extern const uint8_t efr32_tx_power_step_count;

/*
 * The step for a request in dBm, after the ceiling: the highest one whose published output does
 * not exceed it, or the lowest step when none is that low.
 */
const struct efr32_tx_power_step *efr32_tx_power_step(int8_t dbm);

/* A step's output as the controller reports it: whole dBm, rounded up. */
int8_t efr32_tx_power_step_dbm(const struct efr32_tx_power_step *step);

/* The PA level for a request in dBm (efr32_tx_power_step()). */
uint8_t efr32_tx_power_level(int8_t dbm);

/* The highest level available: the ceiling's step, or without one EFR32_PA6_LEVEL_MAX. */
uint8_t efr32_tx_power_max_level(void);

/* Lowest and highest outputs the controller reports, whole dBm, before the ceiling. */
int8_t efr32_tx_power_min_dbm(void);
int8_t efr32_tx_power_max_dbm(void);

/* The dBm the controller reports for a request (lll_radio_tx_pwr_floor()). */
int8_t efr32_tx_power_floor_dbm(int8_t dbm);

/*
 * A ceiling on every later transmission (amateur operation: 47 CFR 97.313(a)); returns the dBm
 * of the step it selects. EFR32_TX_POWER_NO_LIMIT removes it.
 */
int8_t efr32_tx_power_limit_set(int8_t dbm);

#endif /* EFR32_RADIO_TX_POWER_H_ */
