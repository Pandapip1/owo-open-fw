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
	bool dispatching;
};

/* Replacing a handler invalidates an abort queued for its predecessor. */
static inline void efr32_radio_abort_replace(struct efr32_radio_abort_lifecycle *state,
					     efr32_radio_abort_cb_t callback, void *param)
{
	state->callback = callback;
	state->param = param;
	state->pending = false;
	state->dispatching = false;
}

/* A reset has no successor event: neither a queued abort nor its handler survives it. */
static inline void efr32_radio_abort_cancel(struct efr32_radio_abort_lifecycle *state)
{
	efr32_radio_abort_replace(state, NULL, NULL);
}

/* Returns whether the caller must pend the shared FRC IRQ. */
static inline bool efr32_radio_abort_request(struct efr32_radio_abort_lifecycle *state)
{
	/*
	 * Some OpenISA abort callbacks call radio_disable() defensively while
	 * they run.  That command must quiesce RAC, but must not queue the same
	 * cleanup callback a second time.
	 */
	state->pending = state->callback != NULL && !state->dispatching;

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
	state->dispatching = *callback != NULL;

	return state->dispatching;
}

/* Mark completion of the callback obtained by efr32_radio_abort_take(). */
static inline void efr32_radio_abort_finish(struct efr32_radio_abort_lifecycle *state)
{
	state->dispatching = false;
}

#endif /* EFR32_RADIO_ABORT_LIFECYCLE_H_ */
