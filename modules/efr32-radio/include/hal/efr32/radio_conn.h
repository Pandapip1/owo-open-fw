/*
 * Connection-event facts the derived OpenISA LLL hands the radio HAL, so its jobs get start-by
 * deadlines from the Bluetooth specification (hal/efr32/ll_deadlines.h, docs/radio-tables.md
 * section 7.78). Called from configure-time patches of lll_peripheral.c and lll_central.c
 * (CMakeLists.txt); radio_reset() forgets both. SPDX-License-Identifier: Apache-2.0
 */
#ifndef EFR32_RADIO_CONN_H_
#define EFR32_RADIO_CONN_H_

#include <stdint.h>

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

#endif /* EFR32_RADIO_CONN_H_ */
