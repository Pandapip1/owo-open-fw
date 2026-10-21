/*
 * Generation/cancellation contract for the QEMU-only TIFS scheduler seam.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <assert.h>

#include "qemu_tifs_state.h"

int main(void)
{
	struct efr32_qemu_tifs_state state;
	uint32_t generation;

	efr32_qemu_tifs_state_init(&state);
	assert(state.phase == EFR32_QEMU_TIFS_IDLE);

	generation = efr32_qemu_tifs_state_arm(&state, 227U);
	assert(state.phase == EFR32_QEMU_TIFS_WAIT_RX);
	assert(state.hcto_us == 227U);
	assert(efr32_qemu_tifs_state_advance_to_hcto(&state, generation));
	assert(state.phase == EFR32_QEMU_TIFS_WAIT_HCTO);
	assert(efr32_qemu_tifs_state_begin_timeout(&state, generation));
	assert(state.phase == EFR32_QEMU_TIFS_IDLE);

	generation = efr32_qemu_tifs_state_arm(&state, 227U);
	efr32_qemu_tifs_state_cancel(&state);
	assert(state.phase == EFR32_QEMU_TIFS_IDLE);
	/* Models a worker that dequeued WAIT_RX before cancellation. */
	assert(!efr32_qemu_tifs_state_rx_permit(&state, generation));
	assert(!efr32_qemu_tifs_state_advance_to_hcto(&state, generation));
	assert(!efr32_qemu_tifs_state_begin_timeout(&state, generation));

	return 0;
}
