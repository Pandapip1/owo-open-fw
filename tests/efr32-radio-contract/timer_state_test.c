/* Hardware-independent regression coverage for OpenISA timer bookkeeping. */
#include <assert.h>

#include "timer_state.h"

int main(void)
{
	struct efr32_radio_timer_state state;

	efr32_radio_timer_state_init(&state);
	assert(state.start_tick == 0U);
	assert(state.tifs_us == 0U);
	assert(state.saved_aa == 0U);
	assert(!state.aa_capture_requested);
	assert(!state.end_capture_requested);

	/* OpenISA RV32M1 radio.c:1021-1024 keeps the most recent TIFS value. */
	efr32_radio_timer_state_tifs_set(&state, 150U);
	assert(state.tifs_us == 150U);

	/* OpenISA RV32M1 radio.c:1033-1037 records the controller start tick. */
	efr32_radio_timer_state_start_set(&state, 0x12345678U);
	assert(state.start_tick == 0x12345678U);

	efr32_radio_timer_state_aa_save(&state, 37U);
	efr32_radio_timer_state_aa_capture_request(&state);
	efr32_radio_timer_state_end_capture_request(&state);
	assert(state.aa_capture_requested);
	assert(state.end_capture_requested);

	/* OpenISA RV32M1 radio.c:1015-1019 clears requests, not saved AA. */
	efr32_radio_timer_state_status_reset(&state);
	assert(!state.aa_capture_requested);
	assert(!state.end_capture_requested);
	assert(state.saved_aa == 37U);
	assert(state.tifs_us == 150U);
	assert(state.start_tick == 0x12345678U);

	return 0;
}
