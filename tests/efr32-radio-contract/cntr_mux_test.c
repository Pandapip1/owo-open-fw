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

	/* A QEMU TIFS wake must not overwrite the normal radio launch client. */
	m.radio_deadline = 30;
	m.radio_generation = 12;
	m.radio_armed = true;
	m.qemu_tifs_deadline = 25;
	m.qemu_tifs_generation = 13;
	m.qemu_tifs_armed = true;
	assert(efr32_cntr_mux_next(&m, &next) && next == 25);
	d = efr32_cntr_mux_take_due(&m, 25);
	assert(!d.ticker && !d.radio && d.qemu_tifs);
	assert(d.qemu_tifs_generation == 13);
	assert(efr32_cntr_mux_next(&m, &next) && next == 30);

	/* Equal deadlines publish every due client from one CC0 interrupt. */
	m.ticker_deadline = 30;
	m.ticker_armed = true;
	m.qemu_tifs_deadline = 30;
	m.qemu_tifs_generation = 14;
	m.qemu_tifs_armed = true;
	d = efr32_cntr_mux_take_due(&m, 30);
	assert(d.ticker && d.radio && d.qemu_tifs);
	assert(d.radio_generation == 12 && d.qemu_tifs_generation == 14);
	assert(!efr32_cntr_mux_next(&m, &next));
	return 0;
}
