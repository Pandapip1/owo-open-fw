/* Hardware-independent regression coverage for radio_disable() callback handoff. */
#include <assert.h>
#include <stddef.h>

#include "abort_lifecycle.h"

static unsigned int old_calls;
static unsigned int new_calls;
static void *old_param;
static void *new_param;

static void old_callback(void *param)
{
	old_calls++;
	old_param = param;
}

static void new_callback(void *param)
{
	new_calls++;
	new_param = param;
}

int main(void)
{
	struct efr32_radio_abort_lifecycle state = { 0 };
	efr32_radio_abort_cb_t callback;
	void *param;
	int old_token;
	int new_token;

	/* An abort has no handler until OpenISA installs its cleanup callback. */
	assert(!efr32_radio_abort_request(&state));

	efr32_radio_abort_replace(&state, old_callback, &old_token);
	assert(efr32_radio_abort_request(&state));

	/* A later event replaces the callback before the pended IRQ can run. */
	efr32_radio_abort_replace(&state, new_callback, &new_token);
	assert(!efr32_radio_abort_take(&state, &callback, &param));
	assert(old_calls == 0U);
	assert(new_calls == 0U);

	/* The replacement receives only its own subsequent abort, once. */
	assert(efr32_radio_abort_request(&state));
	assert(efr32_radio_abort_take(&state, &callback, &param));
	callback(param);
	assert(new_calls == 1U);
	assert(new_param == &new_token);
	assert(!efr32_radio_abort_take(&state, &callback, &param));
	efr32_radio_abort_finish(&state);

	/*
	 * lll_scan's abort callback defensively calls radio_disable() while the
	 * first abort dispatch is in progress. It must quiesce hardware without
	 * queueing that same callback again.
	 */
	assert(efr32_radio_abort_request(&state));
	assert(efr32_radio_abort_take(&state, &callback, &param));
	assert(!efr32_radio_abort_request(&state));
	callback(param);
	efr32_radio_abort_finish(&state);
	assert(new_calls == 2U);

	/* A later operation can still request an abort after dispatch completes. */
	assert(efr32_radio_abort_request(&state));
	assert(efr32_radio_abort_take(&state, &callback, &param));
	efr32_radio_abort_finish(&state);

	/* Reset drops both a queued abort and its stale LLL callback. */
	efr32_radio_abort_replace(&state, old_callback, &old_token);
	assert(efr32_radio_abort_request(&state));
	efr32_radio_abort_cancel(&state);
	assert(!efr32_radio_abort_take(&state, &callback, &param));
	assert(state.callback == NULL);
	assert(state.param == NULL);

	/* Removing the handler also makes a later disable a no-op. */
	efr32_radio_abort_replace(&state, NULL, NULL);
	assert(!efr32_radio_abort_request(&state));
	assert(old_param == NULL);

	return 0;
}
