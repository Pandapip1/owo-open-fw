/*
 * Timing constants shared by the open radio sequencer (sequencer/radio_seq.S, M0+) and the
 * M33 HAL. Preprocessor definitions only: the sequencer source includes this file too.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef EFR32_RADIO_SEQ_ABI_H_
#define EFR32_RADIO_SEQ_ABI_H_

/* RAC.STIMER runs at ~4.8 ticks/us (measured against BURTC, docs/radio-tables.md SS7.66). */
#define EFR32_SEQ_RAC_STIMER_TICKS_PER_10US 48
/*
 * RX2TX hold: the sequencer acknowledges RX2TX, and TX starts, once RAC.STIMER (counting from
 * the received frame's end) reaches this. Matched to the stock stack on silicon (SS7.71): with
 * a 140 us hold our SCAN_RSP sync word went out 185.2 us after the received frame's MODEM EOF,
 * stock's 180.8 us, both PRS-timestamped in TIMER1.
 */
#define EFR32_SEQ_RX2TX_TX_AT_100NS 1356
#define EFR32_SEQ_RX2TX_TX_AT_STIMER \
	(EFR32_SEQ_RX2TX_TX_AT_100NS * EFR32_SEQ_RAC_STIMER_TICKS_PER_10US / 100)

#endif /* EFR32_RADIO_SEQ_ABI_H_ */
