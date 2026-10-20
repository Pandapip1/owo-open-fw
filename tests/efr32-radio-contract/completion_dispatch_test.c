#include <assert.h>

#include "completion_dispatch.h"

int main(void)
{
	struct efr32_radio_completion_dispatch dispatch;
	uint32_t first;
	uint32_t second;

	efr32_radio_completion_dispatch_init(&dispatch);
	first = efr32_radio_completion_dispatch_replace(&dispatch);
	assert(efr32_radio_completion_dispatch_queue(&dispatch, first));
	/* A level-sensitive completion may publish again, but remains one job. */
	assert(efr32_radio_completion_dispatch_queue(&dispatch, first));
	assert(efr32_radio_completion_dispatch_take(&dispatch));
	assert(!efr32_radio_completion_dispatch_take(&dispatch));

	assert(efr32_radio_completion_dispatch_queue(&dispatch, first));
	second = efr32_radio_completion_dispatch_replace(&dispatch);
	assert(second != first);
	/* Callback replacement drops queued predecessor work. */
	assert(!efr32_radio_completion_dispatch_take(&dispatch));
	assert(!efr32_radio_completion_dispatch_queue(&dispatch, first));
	assert(efr32_radio_completion_dispatch_queue(&dispatch, second));
	assert(efr32_radio_completion_dispatch_take(&dispatch));

	return 0;
}
