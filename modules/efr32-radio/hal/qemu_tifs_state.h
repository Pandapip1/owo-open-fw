/*
 * Host-testable lifecycle bookkeeping for the QEMU-only TIFS job.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef EFR32_RADIO_QEMU_TIFS_STATE_H_
#define EFR32_RADIO_QEMU_TIFS_STATE_H_

#include <stdbool.h>
#include <stdint.h>

enum efr32_qemu_tifs_phase {
	EFR32_QEMU_TIFS_IDLE,
	EFR32_QEMU_TIFS_WAIT_RX,
	EFR32_QEMU_TIFS_WAIT_HCTO,
};

struct efr32_qemu_tifs_state {
	enum efr32_qemu_tifs_phase phase;
	uint32_t generation;
	uint32_t hcto_us;
};

static inline void efr32_qemu_tifs_state_init(struct efr32_qemu_tifs_state *state)
{
	state->phase = EFR32_QEMU_TIFS_IDLE;
	state->generation = 0U;
	state->hcto_us = 0U;
}

static inline uint32_t efr32_qemu_tifs_state_arm(
	struct efr32_qemu_tifs_state *state, uint32_t hcto_us)
{
	state->phase = EFR32_QEMU_TIFS_WAIT_RX;
	state->hcto_us = hcto_us;
	return ++state->generation;
}

static inline void efr32_qemu_tifs_state_cancel(struct efr32_qemu_tifs_state *state)
{
	state->phase = EFR32_QEMU_TIFS_IDLE;
	state->generation++;
}

static inline bool efr32_qemu_tifs_state_advance_to_hcto(
	struct efr32_qemu_tifs_state *state, uint32_t generation)
{
	if (state->phase != EFR32_QEMU_TIFS_WAIT_RX ||
	    state->generation != generation) {
		return false;
	}

	state->phase = EFR32_QEMU_TIFS_WAIT_HCTO;
	return true;
}

static inline bool efr32_qemu_tifs_state_begin_timeout(
	struct efr32_qemu_tifs_state *state, uint32_t generation)
{
	if (state->phase != EFR32_QEMU_TIFS_WAIT_HCTO ||
	    state->generation != generation) {
		return false;
	}

	state->phase = EFR32_QEMU_TIFS_IDLE;
	return true;
}

#endif /* EFR32_RADIO_QEMU_TIFS_STATE_H_ */
