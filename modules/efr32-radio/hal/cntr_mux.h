/* Pure two-client arbitration for the shared BURTC CC0. SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

struct efr32_cntr_mux {
	uint32_t ticker_deadline;
	uint32_t radio_deadline;
	uint32_t radio_generation;
	uint8_t radio_trx;
	bool ticker_armed;
	bool radio_armed;
};

struct efr32_cntr_mux_due {
	bool ticker;
	bool radio;
	uint8_t radio_trx;
	uint32_t radio_generation;
};

static inline bool efr32_cntr_mux_next(const struct efr32_cntr_mux *mux, uint32_t *deadline)
{
	if (!mux->ticker_armed && !mux->radio_armed) {
		return false;
	}
	if (!mux->ticker_armed || (mux->radio_armed &&
		(int32_t)(mux->radio_deadline - mux->ticker_deadline) < 0)) {
		*deadline = mux->radio_deadline;
	} else {
		*deadline = mux->ticker_deadline;
	}
	return true;
}

static inline struct efr32_cntr_mux_due efr32_cntr_mux_take_due(struct efr32_cntr_mux *mux,
								 uint32_t now)
{
	struct efr32_cntr_mux_due due = { 0 };

	due.ticker = mux->ticker_armed && (int32_t)(mux->ticker_deadline - now) <= 0;
	due.radio = mux->radio_armed && (int32_t)(mux->radio_deadline - now) <= 0;
	due.radio_trx = mux->radio_trx;
	due.radio_generation = mux->radio_generation;
	if (due.ticker) {
		mux->ticker_armed = false;
	}
	if (due.radio) {
		mux->radio_armed = false;
	}
	return due;
}
