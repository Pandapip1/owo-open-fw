/*
 * Event timing constants of the EFR32BG22 lower link layer. SPDX-License-Identifier: Apache-2.0
 *
 * Every value below is a PLACEHOLDER taken from Zephyr's OpenISA LLL
 * (subsys/bluetooth/controller/ll_sw/openisa/lll/lll_vendor.h) and is not measured on this chip.
 * They must be replaced by measured HFXO start-up, radio ramp-up and ISR latency figures
 * (docs/radio-tables.md, "timing").
 */

#define EVENT_OVERHEAD_XTAL_US        1500 /* [I] HFXO start-up: measure */
#define EVENT_OVERHEAD_PREEMPT_US     0    /* if <= min, then dynamic preempt */
#define EVENT_OVERHEAD_PREEMPT_MIN_US 150
#define EVENT_OVERHEAD_PREEMPT_MAX_US EVENT_OVERHEAD_XTAL_US
#define EVENT_OVERHEAD_START_US       300  /* [I] */
#define EVENT_OVERHEAD_END_US         40   /* [I] */
#define EVENT_JITTER_US               16
#define EVENT_TICKER_RES_MARGIN_US    32   /* about one tick of the 32768 Hz counter */

#define EVENT_RX_JITTER_US(phy) 16    /* [I] Radio Rx timing uncertainty */
#define EVENT_RX_TO_US(phy) ((((((phy)&0x03) + 4)<<3)/BIT((((phy)&0x3)>>1))) + \
			     EVENT_RX_JITTER_US(phy))

/* [I] software TIFS: RX->TX turn-around budget per PHY, microseconds */
#define EVENT_RX_TX_TURNAROUND(phy)  ((phy) == 1 ? 100 : ((phy) == 2 ? 80 : 900))
