/*
 * Generation bookkeeping for a deferred radio completion callback.
 *
 * The FRC ISR may only publish work.  Replacing the controller callback
 * invalidates any already-published completion, so that a job thread never
 * invokes a predecessor's LLL state machine.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef EFR32_RADIO_COMPLETION_DISPATCH_H_
#define EFR32_RADIO_COMPLETION_DISPATCH_H_

#include <stdbool.h>
#include <stdint.h>

struct efr32_radio_completion_dispatch {
	uint32_t generation;
	uint32_t queued_generation;
	bool queued;
};

static inline void efr32_radio_completion_dispatch_init(
	struct efr32_radio_completion_dispatch *state)
{
	state->generation = 0U;
	state->queued_generation = 0U;
	state->queued = false;
}

/* A replacement has no right to run work published for its predecessor. */
static inline uint32_t efr32_radio_completion_dispatch_replace(
	struct efr32_radio_completion_dispatch *state)
{
	state->generation++;
	state->queued = false;
	return state->generation;
}

/* Coalesce repeated hardware assertions for the same controller callback. */
static inline bool efr32_radio_completion_dispatch_queue(
	struct efr32_radio_completion_dispatch *state, uint32_t generation)
{
	if (generation != state->generation) {
		return false;
	}

	state->queued_generation = generation;
	state->queued = true;
	return true;
}

/* Consume only work that still belongs to the installed callback. */
static inline bool efr32_radio_completion_dispatch_take(
	struct efr32_radio_completion_dispatch *state)
{
	if (!state->queued || state->queued_generation != state->generation) {
		return false;
	}

	state->queued = false;
	return true;
}

#endif /* EFR32_RADIO_COMPLETION_DISPATCH_H_ */
