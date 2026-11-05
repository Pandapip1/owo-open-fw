/* Hardware-independent checks of the dBm to PA level mapping (hal/tx_power.c). */
#include <assert.h>
#include <stdint.h>

#include "tx_power.h"

static void table_is_monotonic(void)
{
	assert(efr32_tx_power_step_count > 1U);
	for (uint8_t i = 1U; i < efr32_tx_power_step_count; i++) {
		assert(efr32_tx_power_steps[i].pa_level > efr32_tx_power_steps[i - 1U].pa_level);
		assert(efr32_tx_power_steps[i].ddbm > efr32_tx_power_steps[i - 1U].ddbm);
		assert(efr32_tx_power_steps[i].pa_level <= EFR32_PA6_LEVEL_MAX);
	}
	assert(efr32_tx_power_steps[0].pa_level == EFR32_PA6_LEVEL_MIN);
}

static void floors(void)
{
	/* Published outputs: level 16 is 0.3 dBm, level 14 -0.6 dBm, level 121 8.5 dBm. */
	assert(efr32_tx_power_level(0) == 14U);
	assert(efr32_tx_power_floor_dbm(0) == 0);
	assert(efr32_tx_power_level(1) == 16U);
	assert(efr32_tx_power_floor_dbm(1) == 1);
	assert(efr32_tx_power_level(8) == 73U);
	assert(efr32_tx_power_floor_dbm(8) == 8);
	assert(efr32_tx_power_level(9) == 121U);
	assert(efr32_tx_power_floor_dbm(9) == 9);
	assert(efr32_tx_power_level(INT8_MAX) == 121U);
	/* -3.1 dBm (level 10) is the highest not above -3; levels 9 (-3.9) and 8 (-4.8) follow. */
	assert(efr32_tx_power_level(-3) == 10U);
	assert(efr32_tx_power_floor_dbm(-3) == -3);
	assert(efr32_tx_power_level(-4) == 8U);
	assert(efr32_tx_power_floor_dbm(-4) == -4);
	/* Below the lowest published output, -27.5 dBm: the lowest level. */
	assert(efr32_tx_power_level(-40) == 0U);
	assert(efr32_tx_power_level(INT8_MIN) == 0U);
	assert(efr32_tx_power_floor_dbm(-40) == -27);
	assert(efr32_tx_power_min_dbm() == -27);
	assert(efr32_tx_power_max_dbm() == 9);

	/* Every reported floor is selectable again as itself and is not above its request. */
	for (int dbm = INT8_MIN; dbm <= INT8_MAX; dbm++) {
		int8_t floor = efr32_tx_power_floor_dbm((int8_t)dbm);

		assert(floor <= dbm || dbm < efr32_tx_power_min_dbm());
		assert(efr32_tx_power_floor_dbm(floor) == floor);
		assert(efr32_tx_power_level(floor) == efr32_tx_power_level((int8_t)dbm));
	}
}

static void ceiling(void)
{
	assert(efr32_tx_power_max_level() == EFR32_PA6_LEVEL_MAX);
	assert(efr32_tx_power_limit_set(3) == 3);
	assert(efr32_tx_power_max_level() == 22U);
	assert(efr32_tx_power_level(8) == 22U);
	assert(efr32_tx_power_level(-8) == 5U);
	assert(efr32_tx_power_floor_dbm(8) == 3);
	/* The range LE Read Transmit Power reports is the radio's, not the station's. */
	assert(efr32_tx_power_max_dbm() == 9);
	assert(efr32_tx_power_limit_set(-50) == -27);
	assert(efr32_tx_power_max_level() == 0U);
	assert(efr32_tx_power_limit_set(EFR32_TX_POWER_NO_LIMIT) == EFR32_TX_POWER_NO_LIMIT);
	assert(efr32_tx_power_max_level() == EFR32_PA6_LEVEL_MAX);
	assert(efr32_tx_power_level(8) == 73U);
}

int main(void)
{
	table_is_monotonic();
	floors();
	ceiling();
	return 0;
}
