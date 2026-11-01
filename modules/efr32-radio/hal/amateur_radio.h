/*
 * Between the radio (hal/radio.c) and amateur operation (hal/amateur.c).
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef EFR32_RADIO_AMATEUR_RADIO_H_
#define EFR32_RADIO_AMATEUR_RADIO_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>

#include "amateur_rules.h"

/* hal/radio.c */
extern const struct efr32_amateur_hw efr32_radio_amateur_hw;
extern const uint32_t efr32_radio_amateur_half_bw_khz;
uint32_t efr32_radio_cw_symbol_rate_hz(void);
uint32_t efr32_radio_cw_symbols_per_unit(uint32_t unit_us);
uint32_t efr32_radio_cw_max_octets(void);
/* LLL context, radio idle: key @p bits (OOK, LSB first) on @p khz; @p done at the frame's end. */
void efr32_radio_cw_transmit(uint32_t khz, const uint8_t *bits, size_t octets,
			     void (*done)(void *param), void *param);
/* From @p done: the radio back to its Bluetooth configuration. */
void efr32_radio_cw_finish(void);
/*
 * Data identification (EFR32_RADIO_AMATEUR_ID_DATA): send @p pdu (header, length,
 * payload) on advertising channel @p chan (37..39, the plan's carrier) as an advertising PDU,
 * @p done at its end. @p first: LLL context, radio idle (the whole advertising setup, as
 * lll_adv.c's prepare); otherwise from the previous PDU's @p done (as lll_adv.c's isr_done).
 */
void efr32_radio_id_adv_transmit(uint8_t chan, const uint8_t *pdu, bool first,
				 void (*done)(void *param), void *param);
/* From the last PDU's @p done: no more identification PDUs. */
void efr32_radio_id_adv_finish(void);
/* TXEN to the first bit on air, us (Kconfig EFR32_RADIO_TX_READY_NS). */
uint32_t efr32_radio_id_tx_ready_us(void);
/* Safe shutdown (PA off, controller stopped) because a rule cannot be kept. */
void efr32_radio_policy_stop(const struct k_deadline_event *event, const char *why);

/* hal/amateur.c */
/* The carrier for RF channel @p rf_channel (2402 + 2 x rf MHz under Part 15): @p standard_khz
 * under Part 15, the plan's under amateur rules. */
uint32_t efr32_radio_amateur_carrier_khz(uint32_t rf_channel, uint32_t standard_khz);
/* A link-layer transmission is being prepared. */
void efr32_radio_amateur_tx_notify(void);
/* Whether link-layer encryption may run; when not, the radio has been stopped. */
bool efr32_radio_amateur_ccm_permitted(void);

#endif /* EFR32_RADIO_AMATEUR_RADIO_H_ */
