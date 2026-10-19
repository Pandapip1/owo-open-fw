/*
 * Radio HAL of the EFR32BG22 lower link layer: the API contract of Zephyr's OpenISA LLL
 * (subsys/bluetooth/controller/ll_sw/openisa/hal/RV32M1/radio/radio.h, 78 functions).
 * SPDX-License-Identifier: Apache-2.0
 *
 * *** UNVALIDATED, UNFINISHED: the radio register map is not public. ***
 * Every function below is classified by what it is blocked on:
 *   software      needs no register access, simply not written yet
 *   regs-*        needs register knowledge listed in docs/radio-tables.md (T1..T9)
 * Until those exist the functions only keep software state; anything that would put a packet on the
 * air asserts through OWO_RADIO_TODO() instead of writing guessed registers.
 */
#include <stdint.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/irq.h>
#include <soc.h>

#include "hal/ccm.h"
#include "hal/radio.h"
#include "hal/debug.h"

#include "util/mem.h"

#include "lll/pdu_vendor.h"
#include "ll_sw/pdu.h"

/* Marks a function that cannot be implemented without the radio register map: logs once. */
#define OWO_RADIO_TODO(tag) \
	do { \
		static bool owo_once_; \
		if (!owo_once_) { \
			owo_once_ = true; \
			printk("owo-radio: %s needs %s (docs/radio-tables.md)\n", __func__, tag); \
		} \
	} while (0)

/* Same, for the functions that would start air activity: refuse to run on stubs. */
#define OWO_RADIO_AIR_TODO(tag) \
	do { \
		OWO_RADIO_TODO(tag); \
		LL_ASSERT_MSG(!IS_ENABLED(CONFIG_OWO_RADIO_EFR32_STUB_REGISTERS), \
			      "radio register map unknown"); \
	} while (0)

static radio_isr_cb_t isr_cb;
static void *isr_cb_param;
static uint8_t MALIGN(4) pkt_empty[PDU_EM_LL_SIZE_MAX];
/* Largest data PDU the controller can be configured to receive, plus MIC; or an advertising PDU. */
static uint8_t MALIGN(4) pkt_scratch[MAX(PDU_DC_LL_HEADER_SIZE + PDU_DC_PAYLOAD_SIZE_MAX + 4,
					 PDU_AC_LL_SIZE_MAX)];


/* [regs] IRQ routing, block enables and reset sequence: RAC/FRC/BUFC/MODEM/SYNTH/AGC EN registers and RAC reset (docs/radio-tables.md T1, T2) */
void isr_radio(void *arg)
{
	ARG_UNUSED(arg);
	if (isr_cb) {
		isr_cb(isr_cb_param);
	}
}

/* [regs] IRQ routing, block enables and reset sequence: RAC/FRC/BUFC/MODEM/SYNTH/AGC EN registers and RAC reset (docs/radio-tables.md T1, T2) */
void radio_isr_set(radio_isr_cb_t cb, void *param)
{
	isr_cb = cb;
	isr_cb_param = param;
	OWO_RADIO_TODO("IRQ routing (T1)");
}

/* [regs] IRQ routing, block enables and reset sequence: RAC/FRC/BUFC/MODEM/SYNTH/AGC EN registers and RAC reset (docs/radio-tables.md T1, T2) */
void radio_setup(void)
{
	OWO_RADIO_TODO("IRQ routing, block enables and reset sequence");
}

/* [regs] IRQ routing, block enables and reset sequence: RAC/FRC/BUFC/MODEM/SYNTH/AGC EN registers and RAC reset (docs/radio-tables.md T1, T2) */
void radio_reset(void)
{
	OWO_RADIO_TODO("IRQ routing, block enables and reset sequence");
}

/* [regs-public] PHY table: sl_rail_ble_config.c modemConfig arrays are public, but the write opcode format and block bases are inferred (docs/radio-tables.md T3) */
void radio_phy_set(uint8_t phy, uint8_t flags)
{
	ARG_UNUSED(phy);
	ARG_UNUSED(flags);
	OWO_RADIO_TODO("regs-public: PHY table");
}

/* [regs] PA registers (rfhal_pa): unknown (T7) */
void radio_tx_power_set(uint32_t power)
{
	ARG_UNUSED(power);
	OWO_RADIO_TODO("PA registers (rfhal_pa)");
}

/* [regs] PA registers (rfhal_pa): unknown (T7) */
void radio_tx_power_max_set(void)
{
	OWO_RADIO_TODO("PA registers (rfhal_pa)");
}

/* [regs] SYNTH channel programming: channel table is public, SYNTH registers are not (T4) */
void radio_freq_chan_set(uint32_t chan)
{
	ARG_UNUSED(chan);
	OWO_RADIO_TODO("SYNTH channel programming");
}

/* [regs] FRC/CRC/whitening/sync-word registers: unknown (T3, T5) */
void radio_whiten_iv_set(uint32_t iv)
{
	ARG_UNUSED(iv);
	OWO_RADIO_TODO("FRC/CRC/whitening/sync-word registers");
}

/* [regs] FRC/CRC/whitening/sync-word registers: unknown (T3, T5) */
void radio_aa_set(const uint8_t *aa)
{
	ARG_UNUSED(aa);
	OWO_RADIO_TODO("FRC/CRC/whitening/sync-word registers");
}

/* [regs] FRC/CRC/whitening/sync-word registers: unknown (T3, T5) */
void radio_pkt_configure(uint8_t bits_len, uint8_t max_len, uint8_t flags)
{
	ARG_UNUSED(bits_len);
	ARG_UNUSED(max_len);
	ARG_UNUSED(flags);
	OWO_RADIO_TODO("FRC/CRC/whitening/sync-word registers");
}

/* [regs-partial] BUFC layout is known (T6, [F]); FRC descriptors that feed it are not (T5) */
void radio_pkt_rx_set(void *rx_packet)
{
	ARG_UNUSED(rx_packet);
	OWO_RADIO_TODO("regs-partial: BUFC layout is known (T6, [F]); FRC descriptors th");
}

/* [regs-partial] BUFC layout is known (T6, [F]); FRC descriptors that feed it are not (T5) */
void radio_pkt_tx_set(void *tx_packet)
{
	ARG_UNUSED(tx_packet);
	OWO_RADIO_TODO("regs-partial: BUFC layout is known (T6, [F]); FRC descriptors th");
}

/* [software] software (no register access needed); not implemented yet */
uint32_t radio_tx_ready_delay_get(uint8_t phy, uint8_t flags)
{
	ARG_UNUSED(phy);
	ARG_UNUSED(flags);
	OWO_RADIO_TODO("software");
	return 0;
}

/* [software] software (no register access needed); not implemented yet */
uint32_t radio_tx_chain_delay_get(uint8_t phy, uint8_t flags)
{
	ARG_UNUSED(phy);
	ARG_UNUSED(flags);
	OWO_RADIO_TODO("software");
	return 0;
}

/* [software] software (no register access needed); not implemented yet */
uint32_t radio_rx_ready_delay_get(uint8_t phy, uint8_t flags)
{
	ARG_UNUSED(phy);
	ARG_UNUSED(flags);
	OWO_RADIO_TODO("software");
	return 0;
}

/* [software] software (no register access needed); not implemented yet */
uint32_t radio_rx_chain_delay_get(uint8_t phy, uint8_t flags)
{
	ARG_UNUSED(phy);
	ARG_UNUSED(flags);
	OWO_RADIO_TODO("software");
	return 0;
}

/* [regs] RAC command/state and FRC/BUFC status/IF bits: unknown (T2) */
void radio_rx_enable(void)
{
	OWO_RADIO_AIR_TODO("RAC command/state and FRC/BUFC status/IF bits");
}

/* [regs] RAC command/state and FRC/BUFC status/IF bits: unknown (T2) */
void radio_tx_enable(void)
{
	OWO_RADIO_AIR_TODO("RAC command/state and FRC/BUFC status/IF bits");
}

/* [regs] RAC command/state and FRC/BUFC status/IF bits: unknown (T2) */
void radio_disable(void)
{
	OWO_RADIO_TODO("RAC command/state and FRC/BUFC status/IF bits");
}

/* [regs] RAC command/state and FRC/BUFC status/IF bits: unknown (T2) */
void radio_status_reset(void)
{
	OWO_RADIO_TODO("RAC command/state and FRC/BUFC status/IF bits");
}

/* [regs] RAC command/state and FRC/BUFC status/IF bits: unknown (T2) */
uint32_t radio_is_ready(void)
{
	OWO_RADIO_TODO("regs");
	return 0;
}

/* [regs] RAC command/state and FRC/BUFC status/IF bits: unknown (T2) */
uint32_t radio_is_done(void)
{
	OWO_RADIO_TODO("regs");
	return 0;
}

/* [regs] RAC command/state and FRC/BUFC status/IF bits: unknown (T2) */
uint32_t radio_has_disabled(void)
{
	OWO_RADIO_TODO("regs");
	return 0;
}

/* [regs] RAC command/state and FRC/BUFC status/IF bits: unknown (T2) */
uint32_t radio_is_idle(void)
{
	OWO_RADIO_TODO("regs");
	return 0;
}

/* [regs] FRC/CRC/whitening/sync-word registers: unknown (T3, T5) */
void radio_crc_configure(uint32_t polynomial, uint32_t iv)
{
	ARG_UNUSED(polynomial);
	ARG_UNUSED(iv);
	OWO_RADIO_TODO("FRC/CRC/whitening/sync-word registers");
}

/* [regs] RAC command/state and FRC/BUFC status/IF bits: unknown (T2) */
uint32_t radio_crc_is_valid(void)
{
	OWO_RADIO_TODO("regs");
	return 0;
}

/* [software] software (no register access needed); not implemented yet */
void *radio_pkt_empty_get(void)
{
	return pkt_empty;
}

/* [software] software (no register access needed); not implemented yet */
void *radio_pkt_scratch_get(void)
{
	return pkt_scratch;
}

/* [regs] software TIFS on top of PROTIMER compare + RAC commands (T2, T8) */
void radio_switch_complete_and_rx(uint8_t phy_rx)
{
	ARG_UNUSED(phy_rx);
	OWO_RADIO_TODO("software TIFS on top of PROTIMER compare + RAC com");
}

/* [regs] software TIFS on top of PROTIMER compare + RAC commands (T2, T8) */
void radio_switch_complete_and_tx(uint8_t phy_rx, uint8_t flags_rx, uint8_t phy_tx,
				  uint8_t flags_tx)
{
	ARG_UNUSED(phy_rx);
	ARG_UNUSED(flags_rx);
	ARG_UNUSED(phy_tx);
	ARG_UNUSED(flags_tx);
	OWO_RADIO_TODO("software TIFS on top of PROTIMER compare + RAC com");
}

/* [regs] software TIFS on top of PROTIMER compare + RAC commands (T2, T8) */
void radio_switch_complete_and_disable(void)
{
	OWO_RADIO_TODO("software TIFS on top of PROTIMER compare + RAC com");
}

/* [regs] AGC RSSI registers: unknown (T9) */
void radio_rssi_measure(void)
{
	OWO_RADIO_TODO("AGC RSSI registers");
}

/* [regs] AGC RSSI registers: unknown (T9) */
uint32_t radio_rssi_get(void)
{
	OWO_RADIO_TODO("regs");
	return 0;
}

/* [regs] AGC RSSI registers: unknown (T9) */
void radio_rssi_status_reset(void)
{
	OWO_RADIO_TODO("AGC RSSI registers");
}

/* [regs] AGC RSSI registers: unknown (T9) */
uint32_t radio_rssi_is_ready(void)
{
	OWO_RADIO_TODO("regs");
	return 0;
}

/* [software] software (no register access needed); not implemented yet */
void radio_filter_configure(uint8_t bitmask_enable, uint8_t bitmask_addr_type,
			    uint8_t *bdaddr)
{
	ARG_UNUSED(bitmask_enable);
	ARG_UNUSED(bitmask_addr_type);
	ARG_UNUSED(bdaddr);
	OWO_RADIO_TODO("software filter");
}

/* [software] software (no register access needed); not implemented yet */
void radio_filter_disable(void)
{
	OWO_RADIO_TODO("software filter");
}

/* [software] software (no register access needed); not implemented yet */
void radio_filter_status_reset(void)
{
	OWO_RADIO_TODO("software filter");
}

/* [software] software (no register access needed); not implemented yet */
uint32_t radio_filter_has_match(void)
{
	OWO_RADIO_TODO("software");
	return 0;
}

/* [software] software (no register access needed); not implemented yet */
uint32_t radio_filter_match_get(void)
{
	OWO_RADIO_TODO("software");
	return 0;
}

/* [software] software (no register access needed); not implemented yet */
void radio_bc_configure(uint32_t n)
{
	ARG_UNUSED(n);
	OWO_RADIO_TODO("software filter");
}

/* [software] software (no register access needed); not implemented yet */
void radio_bc_status_reset(void)
{
	OWO_RADIO_TODO("software filter");
}

/* [software] software (no register access needed); not implemented yet */
uint32_t radio_bc_has_match(void)
{
	OWO_RADIO_TODO("software");
	return 0;
}

/* [regs] PROTIMER (timebase, compare, captures) and RTC sync: unknown (T8) */
void radio_tmr_status_reset(void)
{
	OWO_RADIO_TODO("PROTIMER (timebase, compare, captures) and RTC syn");
}

/* [regs] software TIFS on top of PROTIMER compare + RAC commands (T2, T8) */
void radio_tmr_tifs_set(uint32_t tifs)
{
	ARG_UNUSED(tifs);
	OWO_RADIO_TODO("software TIFS on top of PROTIMER compare + RAC com");
}

/* [regs] PROTIMER (timebase, compare, captures) and RTC sync: unknown (T8) */
uint32_t radio_tmr_start(uint8_t trx, uint32_t ticks_start, uint32_t remainder)
{
	ARG_UNUSED(trx);
	ARG_UNUSED(ticks_start);
	ARG_UNUSED(remainder);
	OWO_RADIO_AIR_TODO("regs");
	return 0;
}

/* [regs] PROTIMER (timebase, compare, captures) and RTC sync: unknown (T8) */
uint32_t radio_tmr_start_tick(uint8_t trx, uint32_t tick)
{
	ARG_UNUSED(trx);
	ARG_UNUSED(tick);
	OWO_RADIO_AIR_TODO("regs");
	return 0;
}

/* [regs] PROTIMER (timebase, compare, captures) and RTC sync: unknown (T8) */
void radio_tmr_start_us(uint8_t trx, uint32_t us)
{
	ARG_UNUSED(trx);
	ARG_UNUSED(us);
	OWO_RADIO_AIR_TODO("PROTIMER (timebase, compare, captures) and RTC syn");
}

/* [regs] PROTIMER (timebase, compare, captures) and RTC sync: unknown (T8) */
uint32_t radio_tmr_start_now(uint8_t trx)
{
	ARG_UNUSED(trx);
	OWO_RADIO_AIR_TODO("regs");
	return 0;
}

/* [regs] PROTIMER (timebase, compare, captures) and RTC sync: unknown (T8) */
uint32_t radio_tmr_start_get(void)
{
	OWO_RADIO_TODO("regs");
	return 0;
}

/* [regs] PROTIMER (timebase, compare, captures) and RTC sync: unknown (T8) */
void radio_tmr_stop(void)
{
	OWO_RADIO_TODO("PROTIMER (timebase, compare, captures) and RTC syn");
}

/* [regs] PROTIMER (timebase, compare, captures) and RTC sync: unknown (T8) */
void radio_tmr_hcto_configure(uint32_t hcto)
{
	ARG_UNUSED(hcto);
	OWO_RADIO_TODO("PROTIMER (timebase, compare, captures) and RTC syn");
}

/* [regs] PROTIMER (timebase, compare, captures) and RTC sync: unknown (T8) */
void radio_tmr_aa_capture(void)
{
	OWO_RADIO_TODO("PROTIMER (timebase, compare, captures) and RTC syn");
}

/* [regs] PROTIMER (timebase, compare, captures) and RTC sync: unknown (T8) */
uint32_t radio_tmr_aa_get(void)
{
	OWO_RADIO_TODO("regs");
	return 0;
}

/* [regs] PROTIMER (timebase, compare, captures) and RTC sync: unknown (T8) */
void radio_tmr_aa_save(uint32_t aa)
{
	ARG_UNUSED(aa);
	OWO_RADIO_TODO("PROTIMER (timebase, compare, captures) and RTC syn");
}

/* [regs] PROTIMER (timebase, compare, captures) and RTC sync: unknown (T8) */
uint32_t radio_tmr_aa_restore(void)
{
	OWO_RADIO_TODO("regs");
	return 0;
}

/* [regs] PROTIMER (timebase, compare, captures) and RTC sync: unknown (T8) */
uint32_t radio_tmr_ready_get(void)
{
	OWO_RADIO_TODO("regs");
	return 0;
}

/* [regs] PROTIMER (timebase, compare, captures) and RTC sync: unknown (T8) */
void radio_tmr_end_capture(void)
{
	OWO_RADIO_TODO("PROTIMER (timebase, compare, captures) and RTC syn");
}

/* [regs] PROTIMER (timebase, compare, captures) and RTC sync: unknown (T8) */
uint32_t radio_tmr_end_get(void)
{
	OWO_RADIO_TODO("regs");
	return 0;
}

/* [regs] software TIFS on top of PROTIMER compare + RAC commands (T2, T8) */
uint32_t radio_tmr_tifs_base_get(void)
{
	OWO_RADIO_TODO("regs");
	return 0;
}

/* [regs] PROTIMER (timebase, compare, captures) and RTC sync: unknown (T8) */
void radio_tmr_sample(void)
{
	OWO_RADIO_TODO("PROTIMER (timebase, compare, captures) and RTC syn");
}

/* [regs] PROTIMER (timebase, compare, captures) and RTC sync: unknown (T8) */
uint32_t radio_tmr_sample_get(void)
{
	OWO_RADIO_TODO("regs");
	return 0;
}

/* [software] software (no register access needed); not implemented yet */
void radio_gpio_pa_setup(void)
{
	/* No front-end module on the target board. */
}

/* [software] software (no register access needed); not implemented yet */
void radio_gpio_lna_setup(void)
{
	/* No front-end module on the target board. */
}

/* [software] software (no register access needed); not implemented yet */
void radio_gpio_lna_on(void)
{
	/* No front-end module on the target board. */
}

/* [software] software (no register access needed); not implemented yet */
void radio_gpio_lna_off(void)
{
	/* No front-end module on the target board. */
}

/* [software] software (no register access needed); not implemented yet */
void radio_gpio_pa_lna_enable(uint32_t trx_us)
{
	ARG_UNUSED(trx_us);
	/* No front-end module on the target board. */
}

/* [software] software (no register access needed); not implemented yet */
void radio_gpio_pa_lna_disable(void)
{
	/* No front-end module on the target board. */
}

/* [software] software (no register access needed); not implemented yet */
void *radio_ccm_rx_pkt_set(struct ccm *ccm, uint8_t phy, void *pkt)
{
	ARG_UNUSED(ccm);
	ARG_UNUSED(phy);
	ARG_UNUSED(pkt);
	OWO_RADIO_TODO("software: software (no register access needed); not implemen");
	return NULL;
}

/* [software] software (no register access needed); not implemented yet */
void *radio_ccm_tx_pkt_set(struct ccm *ccm, void *pkt)
{
	ARG_UNUSED(ccm);
	ARG_UNUSED(pkt);
	OWO_RADIO_TODO("software: software (no register access needed); not implemen");
	return NULL;
}

/* [software] software (no register access needed); not implemented yet */
uint32_t radio_ccm_is_done(void)
{
	OWO_RADIO_TODO("software");
	return 0;
}

/* [software] software (no register access needed); not implemented yet */
uint32_t radio_ccm_mic_is_valid(void)
{
	OWO_RADIO_TODO("software");
	return 0;
}

/* [software] software (no register access needed); not implemented yet */
uint32_t radio_ccm_is_available(void)
{
	OWO_RADIO_TODO("software");
	return 0;
}

/* [software] software (no register access needed); not implemented yet */
void radio_ar_configure(uint32_t nirk, void *irk)
{
	ARG_UNUSED(nirk);
	ARG_UNUSED(irk);
	OWO_RADIO_TODO("software: software (no register access needed); not implemen");
}

/* [software] software (no register access needed); not implemented yet */
uint32_t radio_ar_match_get(void)
{
	OWO_RADIO_TODO("software");
	return 0;
}

/* [software] software (no register access needed); not implemented yet */
void radio_ar_status_reset(void)
{
	OWO_RADIO_TODO("software: software (no register access needed); not implemen");
}

/* [software] software (no register access needed); not implemented yet */
uint32_t radio_ar_has_match(void)
{
	OWO_RADIO_TODO("software");
	return 0;
}

/* [regs] IRQ routing, block enables and reset sequence: RAC/FRC/BUFC/MODEM/SYNTH/AGC EN registers and RAC reset (docs/radio-tables.md T1, T2) */
uint32_t radio_sleep(void)
{
	OWO_RADIO_TODO("regs");
	return 0;
}

/* [regs] IRQ routing, block enables and reset sequence: RAC/FRC/BUFC/MODEM/SYNTH/AGC EN registers and RAC reset (docs/radio-tables.md T1, T2) */
uint32_t radio_wake(void)
{
	OWO_RADIO_TODO("regs");
	return 0;
}

/* [regs] IRQ routing, block enables and reset sequence: RAC/FRC/BUFC/MODEM/SYNTH/AGC EN registers and RAC reset (docs/radio-tables.md T1, T2) */
uint32_t radio_is_off(void)
{
	OWO_RADIO_TODO("regs");
	return 0;
}
