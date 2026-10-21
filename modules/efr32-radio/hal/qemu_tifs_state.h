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
	/* A received packet had pre-armed a QEMU-only RX-to-TX turn-around. */
	EFR32_QEMU_TIFS_WAIT_TX,
	EFR32_QEMU_TIFS_WAIT_HCTO,
};

/*
 * OpenISA records the radio operation that should follow the operation it is
 * currently preparing.  Keep that request distinct from the scheduler phase:
 * on RXDONE a TX request is claimed before the LLL callback, while the
 * callback itself may clear the *next* sequencer command after the claim.
 */
enum efr32_qemu_tifs_next_action {
	EFR32_QEMU_TIFS_NEXT_NONE,
	EFR32_QEMU_TIFS_NEXT_RX,
	EFR32_QEMU_TIFS_NEXT_TX,
};

static inline void efr32_qemu_tifs_next_action_set(
	enum efr32_qemu_tifs_next_action *next,
	enum efr32_qemu_tifs_next_action action)
{
	*next = action;
}

static inline bool efr32_qemu_tifs_next_action_claim(
	enum efr32_qemu_tifs_next_action *next,
	enum efr32_qemu_tifs_next_action action)
{
	if (*next != action) {
		return false;
	}

	*next = EFR32_QEMU_TIFS_NEXT_NONE;
	return true;
}

static inline void efr32_qemu_tifs_next_action_clear(
	enum efr32_qemu_tifs_next_action *next)
{
	*next = EFR32_QEMU_TIFS_NEXT_NONE;
}

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

/*
 * Unlike WAIT_RX, this transition has no header-completion timeout.  The
 * completion worker has already stopped RX and the LLL callback stages the
 * response PDU while this bounded TIFS timer runs.
 */
static inline uint32_t efr32_qemu_tifs_state_arm_tx(
	struct efr32_qemu_tifs_state *state)
{
	state->phase = EFR32_QEMU_TIFS_WAIT_TX;
	state->hcto_us = 0U;
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

/*
 * Cancellation point immediately before the worker can issue RXEN.  Keep
 * this separate from the post-command transition: a cancelled generation
 * must never reach a radio side effect.
 */
static inline bool efr32_qemu_tifs_state_rx_permit(
	struct efr32_qemu_tifs_state *state, uint32_t generation)
{
	return state->phase == EFR32_QEMU_TIFS_WAIT_RX &&
		state->generation == generation;
}

/* Reserve the RX side effect while the caller holds the operation lock. */
static inline bool efr32_qemu_tifs_state_claim_rx(
	struct efr32_qemu_tifs_state *state, uint32_t generation)
{
	if (!efr32_qemu_tifs_state_rx_permit(state, generation)) {
		return false;
	}
	state->phase = EFR32_QEMU_TIFS_WAIT_HCTO;
	return true;
}

/* Reserve the TX side effect while the caller holds the operation lock. */
static inline bool efr32_qemu_tifs_state_claim_tx(
	struct efr32_qemu_tifs_state *state, uint32_t generation)
{
	if (state->phase != EFR32_QEMU_TIFS_WAIT_TX ||
	    state->generation != generation) {
		return false;
	}
	state->phase = EFR32_QEMU_TIFS_IDLE;
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
