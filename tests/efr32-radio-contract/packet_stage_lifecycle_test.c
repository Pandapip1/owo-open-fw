/* Host contract for completion attribution and staging ownership. SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>

#include "packet_stage_lifecycle.h"

int main(void)
{
	struct efr32_radio_packet_lifecycle state;

	efr32_packet_lifecycle_reset(&state);
	efr32_packet_lifecycle_arm(&state, EFR32_RADIO_PACKET_RX);

	/* RXDONE stays attributable while its LLL callback reads status. */
	assert(efr32_packet_lifecycle_latch_completion(&state, EFR32_RADIO_PACKET_RX));
	assert(state.active_direction == EFR32_RADIO_PACKET_RX);

	/* lll_isr_status_reset() is the release boundary before an RX-to-TX turn. */
	assert(efr32_packet_lifecycle_status_reset(&state));
	assert(state.active_direction == EFR32_RADIO_PACKET_IDLE);
	efr32_packet_lifecycle_arm(&state, EFR32_RADIO_PACKET_TX);
	assert(state.active_direction == EFR32_RADIO_PACKET_TX);

	/* An RXDONE while TX is armed cannot release the TX staging FIFO. */
	assert(!efr32_packet_lifecycle_latch_completion(&state, EFR32_RADIO_PACKET_RX));
	assert(!efr32_packet_lifecycle_status_reset(&state));
	assert(state.active_direction == EFR32_RADIO_PACKET_TX);

	/* Replacing a callback drops an otherwise matching predecessor completion. */
	assert(efr32_packet_lifecycle_latch_completion(&state, EFR32_RADIO_PACKET_TX));
	efr32_packet_lifecycle_forget_completion(&state);
	assert(!efr32_packet_lifecycle_status_reset(&state));
	assert(state.active_direction == EFR32_RADIO_PACKET_TX);

	assert(efr32_packet_lifecycle_latch_completion(&state, EFR32_RADIO_PACKET_TX));
	assert(efr32_packet_lifecycle_status_reset(&state));
	assert(state.active_direction == EFR32_RADIO_PACKET_IDLE);
	return 0;
}
