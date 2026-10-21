/*
 * Contract for the still-unimplemented packet-complete TIFS switch.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <assert.h>

#include "timer_state.h"

static bool tifs_switch_inputs_ready(const struct efr32_radio_timer_state *state)
{
	/*
	 * OpenISA RV32M1 radio.c:937-945 selects the next command, while its
	 * packet-complete path arms the compare from the captured end timestamp,
	 * TIFS, and warm-up margin.  A request alone is not a captured timestamp.
	 */
	return state->end_capture_requested && state->end_tick_valid &&
	       state->tifs_us != 0U;
}

int main(void)
{
	struct efr32_radio_timer_state state;

	efr32_radio_timer_state_init(&state);
	efr32_radio_timer_state_tifs_set(&state, 150U);
	efr32_radio_timer_state_end_capture_request(&state);

	/*
	 * This is the current EFR32 boundary: radio_tmr_end_capture() records
	 * only a request.  There is no source-backed end timestamp to consume,
	 * so a TIFS switch must not be armed from the BURTC start alarm.
	 */
	assert(state.end_capture_requested);
	assert(!state.end_tick_valid);
	assert(!tifs_switch_inputs_ready(&state));

	efr32_radio_timer_state_status_reset(&state);
	assert(!state.end_capture_requested);
	assert(!state.end_tick_valid);
	assert(!tifs_switch_inputs_ready(&state));

	return 0;
}
