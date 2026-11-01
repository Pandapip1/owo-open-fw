/*
 * An advertiser's scan response staged before the SCAN_REQ ends (docs/radio-tables.md 7.82).
 * Included at the end of the derived lll_adv.c (CMakeLists.txt); isr_tx() calls it once the
 * receive after an ADV_IND is set up. The sequencer answers a SCAN_REQ for this advertiser with
 * the SCAN_RSP on its own at T_IFS by isr_rx_pdu()'s rules (hal/efr32/seq_abi.h); isr_rx() then
 * commits the same answer (the HAL checks it). Only where those rules need no state the sequencer
 * lacks: without a scan-request filter, privacy or scan-request reports, and not for a directed
 * advertisement (which takes no SCAN_REQ); elsewhere the M33 still decides at the frame's end.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "hal/efr32/radio_conn.h"

static void efr32_adv_stage(struct lll_adv *lll)
{
#if defined(CONFIG_EFR32_RADIO_CONN_STAGED) && !defined(CONFIG_BT_CTLR_PRIVACY) && \
	!defined(CONFIG_BT_CTLR_SCAN_REQ_NOTIFY)
	struct pdu_adv *adv = lll_adv_data_curr_get(lll);
	struct pdu_adv *sr = lll_adv_scan_rsp_curr_get(lll);

	if (adv->type == PDU_ADV_TYPE_DIRECT_IND ||
	    (lll->filter_policy & BT_LE_ADV_FP_FILTER_SCAN_REQ) != 0U) {
		return;
	}
	efr32_radio_adv_stage((const uint8_t *)sr, adv->adv_ind.addr, adv->tx_addr != 0U);
#else
	ARG_UNUSED(lll);
#endif
}
