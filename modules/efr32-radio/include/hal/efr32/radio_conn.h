/*
 * Connection-event facts the derived OpenISA LLL hands the radio HAL, so its jobs get start-by
 * deadlines from the Bluetooth specification (hal/efr32/ll_deadlines.h, docs/radio-tables.md
 * section 7.79). Called from configure-time patches of lll_peripheral.c and lll_central.c
 * (CMakeLists.txt); radio_reset() forgets both. SPDX-License-Identifier: Apache-2.0
 */
#ifndef EFR32_RADIO_CONN_H_
#define EFR32_RADIO_CONN_H_

#include <stdbool.h>
#include <stdint.h>

#include "hal/efr32/seq_abi.h"

/*
 * The event being prepared is a connection event with connInterval @p interval_us: the work that
 * closes it serves the next connection event (Vol 6 Part B 4.5.1).
 */
void efr32_radio_conn_event_set(uint32_t interval_us);

/*
 * The next RX launch opens a Peripheral's receive window. ull_peripheral.c and ull_conn.c place
 * its start EVENT_TICKER_RES_MARGIN_US + EVENT_JITTER_US + window_widening_event_us + the ready
 * delay before the window's scheduled start (anchor point, or transmit window start), and
 * windowWidening is window_widening_event_us + EVENT_JITTER_US (J = 16 us, sleep clock; Vol 6
 * Part B 4.2.4, 4.5.7): the receiver must be searching @p tolerance_us
 * (= EVENT_TICKER_RES_MARGIN_US) after its planned ready time at the latest.
 */
void efr32_radio_rx_window_set(uint32_t tolerance_us);

/*
 * Connection replies staged before the frame ends (docs/radio-tables.md 7.82,
 * hal/efr32/seq_abi.h). lll/lll_conn_stage.c, built into the derived lll_conn.c, describes each
 * candidate reply; the HAL builds its image (encrypting the payload with @p ccm at @p counter
 * when @p ccm is set) and hands both to the sequencer.
 */
struct ccm;
struct lll_conn;

struct efr32_radio_conn_reply {
	uint8_t header; /* LLID and MD; the sequencer sets SN and NESN */
	uint8_t len;    /* payload octets before the MIC */
	const uint8_t *payload;
	const struct ccm *ccm;
	uint64_t counter;
};

void efr32_radio_conn_stage(bool central, bool rx_free, bool crc_closes, uint8_t sn, uint8_t nesn,
			    uint8_t rx_max,
			    const struct efr32_radio_conn_reply *cur,
			    const struct efr32_radio_conn_reply *next);
/*
 * The sequencer's decision on the frame just received: 0 when the receive was not staged, 1 with
 * @p outcome (EFR32_SEQ_CONN_OUT_*), -1 when staged but no decision was taken (nothing was sent).
 */
int efr32_radio_conn_outcome(uint32_t *outcome);
/* Whether the reply just committed went out and the sequencer armed the next receive. */
bool efr32_radio_conn_continues(void);
/* Stage the replies to a connection event's first receive (the prepares). */
void efr32_conn_stage_prepare(struct lll_conn *lll);

#endif /* EFR32_RADIO_CONN_H_ */
