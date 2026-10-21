/*
 * Contract for the scheduler-owned HCTO boundary.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <assert.h>

#include "timer_state.h"

int main(void)
{
	struct efr32_radio_timer_state state;

	efr32_radio_timer_state_init(&state);
	assert(state.hcto_us == 0U);
	assert(!state.hcto_requested);

	/*
	 * The LLL's hcto is relative to OpenISA's EVENT_TMR.  Retaining it is
	 * useful evidence for the eventual mapping, but without a proven event
	 * epoch it must not be mistaken for an armed BURTC deadline.
	 */
	efr32_radio_timer_state_hcto_set(&state, 227U);
	assert(state.hcto_requested);
	assert(state.hcto_us == 227U);
	assert(state.start_tick == 0U);

	/* No API currently establishes an EVENT_TMR-to-BURTC epoch. */
	efr32_radio_timer_state_status_reset(&state);
	assert(!state.hcto_requested);
	assert(state.hcto_us == 227U);

	return 0;
}
