/*
 * Pure packet-stage ownership bookkeeping for the EFR32 radio port.
 *
 * A matching FRC completion remains visible to the LLL callback.  The LLL
 * consumes that status and then calls radio_status_reset(); only that latter
 * boundary releases BUFC staging ownership for a following operation.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef EFR32_RADIO_PACKET_STAGE_LIFECYCLE_H_
#define EFR32_RADIO_PACKET_STAGE_LIFECYCLE_H_

#include <stdbool.h>

enum efr32_radio_packet_direction {
	EFR32_RADIO_PACKET_IDLE,
	EFR32_RADIO_PACKET_RX,
	EFR32_RADIO_PACKET_TX,
};

struct efr32_radio_packet_lifecycle {
	enum efr32_radio_packet_direction active_direction;
	enum efr32_radio_packet_direction completion_direction;
};

static inline void efr32_packet_lifecycle_reset(struct efr32_radio_packet_lifecycle *state)
{
	state->active_direction = EFR32_RADIO_PACKET_IDLE;
	state->completion_direction = EFR32_RADIO_PACKET_IDLE;
}

static inline void efr32_packet_lifecycle_arm(struct efr32_radio_packet_lifecycle *state,
					       enum efr32_radio_packet_direction direction)
{
	state->active_direction = direction;
	state->completion_direction = EFR32_RADIO_PACKET_IDLE;
}

/* Returns true only when the IRQ completed the direction that is still armed. */
static inline bool efr32_packet_lifecycle_latch_completion(
	struct efr32_radio_packet_lifecycle *state,
	enum efr32_radio_packet_direction direction)
{
	if (direction == EFR32_RADIO_PACKET_IDLE || state->active_direction != direction) {
		return false;
	}

	state->completion_direction = direction;
	return true;
}

/*
 * The LLL has sampled radio_is_done()/radio_crc_is_valid() before this call.
 * Retire staging only for that same attributed completion; a stale completion
 * must never release a newer operation's FIFO ownership.
 */
static inline bool efr32_packet_lifecycle_status_reset(struct efr32_radio_packet_lifecycle *state)
{
	bool completed = state->completion_direction != EFR32_RADIO_PACKET_IDLE &&
		state->completion_direction == state->active_direction;

	state->completion_direction = EFR32_RADIO_PACKET_IDLE;
	if (completed) {
		state->active_direction = EFR32_RADIO_PACKET_IDLE;
	}

	return completed;
}

/* A callback replacement cannot consume a completion belonging to its predecessor. */
static inline void efr32_packet_lifecycle_forget_completion(
	struct efr32_radio_packet_lifecycle *state)
{
	state->completion_direction = EFR32_RADIO_PACKET_IDLE;
}

#endif /* EFR32_RADIO_PACKET_STAGE_LIFECYCLE_H_ */
