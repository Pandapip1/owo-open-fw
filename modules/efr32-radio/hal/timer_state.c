/* SPDX-License-Identifier: Apache-2.0 */
#include <string.h>

#include "timer_state.h"

void efr32_radio_timer_state_init(struct efr32_radio_timer_state *state)
{
	memset(state, 0, sizeof(*state));
}

void efr32_radio_timer_state_status_reset(struct efr32_radio_timer_state *state)
{
	/*
	 * OpenISA RV32M1 radio.c:1015-1019 clears only the two capture-save
	 * requests.  In particular, it does not discard the AA value saved by
	 * radio_tmr_aa_save(), which connection setup may restore later.
	 */
	state->aa_capture_requested = false;
	state->end_capture_requested = false;
}

void efr32_radio_timer_state_tifs_set(struct efr32_radio_timer_state *state,
				      uint32_t tifs_us)
{
	/* OpenISA RV32M1 radio.c:1021-1024 retains this for ISR TIFS arithmetic. */
	state->tifs_us = tifs_us;
}

void efr32_radio_timer_state_start_set(struct efr32_radio_timer_state *state,
				       uint32_t start_tick)
{
	/* OpenISA RV32M1 radio.c:1033-1037 stores ticks_start before arming HW. */
	state->start_tick = start_tick;
}

void efr32_radio_timer_state_aa_capture_request(struct efr32_radio_timer_state *state)
{
	/* OpenISA RV32M1 radio.c:1176-1179 sets the capture-save request. */
	state->aa_capture_requested = true;
}

void efr32_radio_timer_state_end_capture_request(struct efr32_radio_timer_state *state)
{
	/* OpenISA RV32M1 radio.c:1203-1206 sets the capture-save request. */
	state->end_capture_requested = true;
}

void efr32_radio_timer_state_aa_save(struct efr32_radio_timer_state *state,
				     uint32_t aa)
{
	/* OpenISA RV32M1 radio.c:1186-1195 keeps this separately from captures. */
	state->saved_aa = aa;
}
