/* Host contract for shared BURTC ticker/radio arbitration. SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>

#include "cntr_mux.h"

int main(void)
{
	struct efr32_cntr_mux m = {
		.ticker_deadline = 20,
		.radio_deadline = 10,
		.radio_generation = 7,
		.radio_trx = 1,
		.ticker_armed = true,
		.radio_armed = true,
	};
	uint32_t next;
	struct efr32_cntr_mux_due d;

	assert(efr32_cntr_mux_next(&m, &next) && next == 10);
	d = efr32_cntr_mux_take_due(&m, 10);
	assert(!d.ticker && d.radio && d.radio_trx == 1 && d.radio_generation == 7);
	assert(efr32_cntr_mux_next(&m, &next) && next == 20);

	m.radio_deadline = 20;
	m.radio_generation = 9;
	m.radio_armed = true;
	d = efr32_cntr_mux_take_due(&m, 20);
	assert(d.ticker && d.radio && d.radio_generation == 9);

	m.radio_armed = true;
	m.radio_generation = 11;
	m.radio_armed = false;
	assert(!efr32_cntr_mux_next(&m, &next));
	return 0;
}
