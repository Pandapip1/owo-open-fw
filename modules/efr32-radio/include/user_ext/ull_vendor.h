/*
 * Zephyr controller user extensions (CONFIG_BT_CTLR_USER_EXT) for the EFR32 radio.
 *
 * Amateur operation (hal/amateur.c) uses only the extension's ticker nodes
 * (CONFIG_BT_CTLR_USER_TICKER_ID_RANGE) to reserve radio time for its Morse identification; it
 * adds no proprietary event or receive node, so these hooks have nothing to do.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef EFR32_RADIO_ULL_VENDOR_H_
#define EFR32_RADIO_ULL_VENDOR_H_

#if defined(CONFIG_BT_CTLR_USER_EXT)

static inline int ull_user_init(void)
{
	return 0;
}

static inline int rx_demux_rx_proprietary(memq_link_t *link, struct node_rx_hdr *rx,
					  memq_link_t *tail, memq_link_t **head)
{
	ARG_UNUSED(link);
	ARG_UNUSED(rx);
	ARG_UNUSED(tail);
	ARG_UNUSED(head);
	return 0;
}

static inline void ull_proprietary_done(struct node_rx_event_done *done)
{
	ARG_UNUSED(done);
}

#endif /* CONFIG_BT_CTLR_USER_EXT */

#endif /* EFR32_RADIO_ULL_VENDOR_H_ */
