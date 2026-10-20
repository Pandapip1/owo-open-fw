/*
 * Software lifecycle for the OpenISA radio-abort callback.
 *
 * RAC has no abort-complete latch.  radio.c uses this state to turn a
 * requested RXDIS/TXDIS cleanup into one software IRQ dispatch, without
 * representing that dispatch as an FRC packet completion.
 */
#ifndef EFR32_RADIO_ABORT_LIFECYCLE_H_
#define EFR32_RADIO_ABORT_LIFECYCLE_H_

#include <stdbool.h>
#include <stddef.h>

typedef void (*efr32_radio_abort_cb_t)(void *param);

struct efr32_radio_abort_lifecycle {
	efr32_radio_abort_cb_t callback;
	void *param;
	bool pending;
};

/* Replacing a handler invalidates an abort queued for its predecessor. */
static inline void efr32_radio_abort_replace(struct efr32_radio_abort_lifecycle *state,
					     efr32_radio_abort_cb_t callback, void *param)
{
	state->callback = callback;
	state->param = param;
	state->pending = false;
}

/* Returns whether the caller must pend the shared FRC IRQ. */
static inline bool efr32_radio_abort_request(struct efr32_radio_abort_lifecycle *state)
{
	state->pending = state->callback != NULL;

	return state->pending;
}

/* Consume at most one abort, taking a stable callback/parameter pair. */
static inline bool efr32_radio_abort_take(struct efr32_radio_abort_lifecycle *state,
					  efr32_radio_abort_cb_t *callback, void **param)
{
	if (!state->pending) {
		return false;
	}

	state->pending = false;
	*callback = state->callback;
	*param = state->param;

	return *callback != NULL;
}

#endif /* EFR32_RADIO_ABORT_LIFECYCLE_H_ */
