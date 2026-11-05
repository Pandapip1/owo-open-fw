/* SPDX-License-Identifier: Apache-2.0 */
#include "tx_power.h"

/* Tenths of a dBm per dBm. */
#define EFR32_DDBM_PER_DBM 10

/*
 * The 6 dBm PA's levels with a published output: every distinct level of Silicon Labs' xG22
 * 6 dBm dBm-to-power-setting table and its "act_deci-dBm" column
 * (sl_rail_util_pa_dbm_powersetting_mapping_table_6dbm.h, Zlib), measured by Silicon Labs on
 * their reference design, not on these boards: unverified here (docs/tx-power.md lists the
 * measurements). Levels above the last step have no published output.
 */
const struct efr32_tx_power_step efr32_tx_power_steps[] = {
	{ 0U, -275 }, { 1U, -212 }, { 2U, -160 }, { 3U, -127 }, { 4U, -104 }, { 5U, -86 },
	{ 6U, -71 },  { 7U, -59 },  { 8U, -48 },  { 9U, -39 },  { 10U, -31 }, { 12U, -17 },
	{ 14U, -6 },  { 16U, 3 },   { 19U, 15 },  { 22U, 24 },  { 26U, 34 },  { 33U, 45 },
	{ 40U, 54 },  { 52U, 65 },  { 73U, 75 },  { 121U, 85 },
};

const uint8_t efr32_tx_power_step_count =
	(uint8_t)(sizeof(efr32_tx_power_steps) / sizeof(efr32_tx_power_steps[0]));

/* Written by the application's thread, read by the link layer's: one byte, so never torn. */
static volatile int8_t efr32_tx_power_limit = EFR32_TX_POWER_NO_LIMIT;

static const struct efr32_tx_power_step *efr32_tx_power_step_raw(int8_t dbm)
{
	const struct efr32_tx_power_step *step = &efr32_tx_power_steps[0];
	int32_t ddbm = (int32_t)dbm * EFR32_DDBM_PER_DBM;

	for (uint8_t i = 1U; i < efr32_tx_power_step_count; i++) {
		if (efr32_tx_power_steps[i].ddbm > ddbm) {
			break;
		}
		step = &efr32_tx_power_steps[i];
	}

	return step;
}

const struct efr32_tx_power_step *efr32_tx_power_step(int8_t dbm)
{
	int8_t limit = efr32_tx_power_limit;

	return efr32_tx_power_step_raw(dbm < limit ? dbm : limit);
}

int8_t efr32_tx_power_step_dbm(const struct efr32_tx_power_step *step)
{
	int32_t ddbm = step->ddbm;

	/* Rounded up: never below the output, and a request of it selects the same step. */
	if (ddbm > 0) {
		ddbm += EFR32_DDBM_PER_DBM - 1;
	}

	return (int8_t)(ddbm / EFR32_DDBM_PER_DBM);
}

uint8_t efr32_tx_power_level(int8_t dbm)
{
	return efr32_tx_power_step(dbm)->pa_level;
}

uint8_t efr32_tx_power_max_level(void)
{
	int8_t limit = efr32_tx_power_limit;

	if (limit == EFR32_TX_POWER_NO_LIMIT) {
		return EFR32_PA6_LEVEL_MAX;
	}

	return efr32_tx_power_step_raw(limit)->pa_level;
}

int8_t efr32_tx_power_min_dbm(void)
{
	return efr32_tx_power_step_dbm(&efr32_tx_power_steps[0]);
}

int8_t efr32_tx_power_max_dbm(void)
{
	return efr32_tx_power_step_dbm(&efr32_tx_power_steps[efr32_tx_power_step_count - 1U]);
}

int8_t efr32_tx_power_floor_dbm(int8_t dbm)
{
	return efr32_tx_power_step_dbm(efr32_tx_power_step(dbm));
}

int8_t efr32_tx_power_limit_set(int8_t dbm)
{
	efr32_tx_power_limit = dbm;
	if (dbm == EFR32_TX_POWER_NO_LIMIT) {
		return dbm;
	}

	return efr32_tx_power_step_dbm(efr32_tx_power_step_raw(dbm));
}
