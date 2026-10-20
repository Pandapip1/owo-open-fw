/*
 * Pure controller-visible timer bookkeeping for the EFR32 radio port.
 *
 * This deliberately has no MMIO knowledge.  PROTIMER's compare, capture,
 * and interrupt contract still needs silicon evidence; this layer only
 * retains state which the OpenISA reference keeps in software.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef EFR32_RADIO_TIMER_STATE_H_
#define EFR32_RADIO_TIMER_STATE_H_

#include <stdbool.h>
#include <stdint.h>

struct efr32_radio_timer_state {
	uint32_t start_tick;
	uint32_t tifs_us;
	uint32_t saved_aa;
	bool aa_capture_requested;
	bool end_capture_requested;
};

void efr32_radio_timer_state_init(struct efr32_radio_timer_state *state);
void efr32_radio_timer_state_status_reset(struct efr32_radio_timer_state *state);
void efr32_radio_timer_state_tifs_set(struct efr32_radio_timer_state *state,
				      uint32_t tifs_us);
void efr32_radio_timer_state_start_set(struct efr32_radio_timer_state *state,
				       uint32_t start_tick);
void efr32_radio_timer_state_aa_capture_request(struct efr32_radio_timer_state *state);
void efr32_radio_timer_state_end_capture_request(struct efr32_radio_timer_state *state);
void efr32_radio_timer_state_aa_save(struct efr32_radio_timer_state *state,
				     uint32_t aa);

#endif /* EFR32_RADIO_TIMER_STATE_H_ */
