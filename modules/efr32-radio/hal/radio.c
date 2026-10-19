/*
 * Radio HAL of the EFR32BG22 lower link layer: the API contract of Zephyr's OpenISA LLL
 * (subsys/bluetooth/controller/ll_sw/openisa/hal/RV32M1/radio/radio.h, 78 functions).
 * SPDX-License-Identifier: Apache-2.0
 *
 * *** UNVALIDATED, UNFINISHED: public generated register layouts are now known,
 * but the required radio/sequencer behavior is not fully characterized. ***
 * Every function below is classified by what it is blocked on:
 *   software      needs no register access, simply not written yet
 *   regs-*        needs register knowledge listed in docs/radio-tables.md (T1..T9)
 * Until those exist the functions only keep software state; anything that would put a packet on the
 * air asserts through EFR32_RADIO_TODO() instead of writing guessed registers.
 */
#include <stdint.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/irq.h>
#include <soc.h>
#include <psa/crypto.h>

#include "hal/ccm.h"
#include "hal/radio.h"
#include "hal/debug.h"

#include "util/mem.h"

#include "lll/pdu_vendor.h"
#include "ll_sw/pdu.h"

/* Marks a function that cannot be implemented without the radio register map: logs once. */
#define EFR32_RADIO_TODO(tag) \
	do { \
		static bool efr32_once_; \
		if (!efr32_once_) { \
			efr32_once_ = true; \
			printk("efr32-radio: %s needs %s (docs/radio-tables.md)\n", __func__, tag); \
		} \
	} while (0)

/* Same, for the functions that would start air activity: refuse to run on stubs. */
#define EFR32_RADIO_AIR_TODO(tag) \
	do { \
		EFR32_RADIO_TODO(tag); \
		LL_ASSERT_MSG(!IS_ENABLED(CONFIG_EFR32_RADIO_STUB_REGISTERS), \
			      "radio register map unknown"); \
	} while (0)

static radio_isr_cb_t isr_cb;
static void *isr_cb_param;
static uint8_t MALIGN(4) pkt_empty[PDU_EM_LL_SIZE_MAX];
/* Largest data PDU the controller can be configured to receive, plus MIC; or an advertising PDU. */
static uint8_t MALIGN(4) pkt_scratch[MAX(PDU_DC_LL_HEADER_SIZE + PDU_DC_PAYLOAD_SIZE_MAX + 4,
					 PDU_AC_LL_SIZE_MAX)];
/*
 * RAIL encodes a 512-byte BUFC TX FIFO as control value three. Staging keeps
 * that declared allocation real even when OpenISA hands us a short PDU.
 */
static uint8_t MALIGN(4) efr32_tx_fifo[512];
/* RAIL configures the RX BUFC channel as a 512-byte power-of-two FIFO too. */
static uint8_t MALIGN(4) efr32_rx_fifo[512];

#define BLE_CCM_MIC_SIZE 4U
#define BLE_CCM_NONCE_SIZE 13U
#define BLE_CCM_AAD_MASK 0xE3U

#define EFR32_SYNTH_BASE 0xB8018000UL
#define EFR32_SYNTH_CHCTRL_OFFSET 0x040U
#define EFR32_SYNTH_CHCTRL_CHNO_MASK 0x3FU

#define EFR32_RFCRC_BASE 0xB8010000UL
#define EFR32_RFCRC_EN_OFFSET 0x004U
#define EFR32_RFCRC_CTRL_OFFSET 0x008U
#define EFR32_RFCRC_INIT_OFFSET 0x018U
#define EFR32_RFCRC_POLY_OFFSET 0x020U
#define EFR32_RFCRC_BLE_CTRL 0x00000708U

#define EFR32_FRC_BASE 0xB8004000UL
#define EFR32_AGC_BASE 0xB800C000UL
#define EFR32_FRC_WHITEPOLY_OFFSET 0x034U
#define EFR32_FRC_WHITEINIT_OFFSET 0x038U
#define EFR32_FRC_TRAILRXDATA_OFFSET 0x054U
#define EFR32_FRC_IF_OFFSET 0x068U
#define EFR32_FRC_IEN_OFFSET 0x06CU
#define EFR32_FRC_BLE_WHITEPOLY 0x0044U
#define EFR32_FRC_BLE_WHITEINIT_MSB 0x0040U
#define EFR32_FRC_IF_TXDONE (1U << 0)
#define EFR32_FRC_IF_RXDONE (1U << 4)
#define EFR32_FRC_IF_COMPLETION (EFR32_FRC_IF_TXDONE | EFR32_FRC_IF_RXDONE)
#define EFR32_FRC_TRAILRXDATA_CRCOK (1U << 1)

#define EFR32_MODEM_BASE 0xB8014000UL
#define EFR32_MODEM_SYNC0_OFFSET 0x068U

#define EFR32_AGC_FRAMERSSI_OFFSET 0x01CU
#define EFR32_AGC_RSSI_CODE_SHIFT 6U
#define EFR32_AGC_RSSI_CODE_MASK 0x3FFU
#define EFR32_AGC_RSSI_INVALID (-512)
#define EFR32_AGC_RSSI_OFFSET_QUARTER_DB 200

#define EFR32_RAC_BASE 0xB8020000UL
#define EFR32_RAC_RXENSRCEN_OFFSET 0x008U
#define EFR32_RAC_STATUS_OFFSET 0x00CU
#define EFR32_RAC_CMD_OFFSET 0x010U
#define EFR32_REGISTER_SET_OFFSET 0x1000U
#define EFR32_REGISTER_CLEAR_OFFSET 0x2000U
#define EFR32_RAC_STATUS_STATE_SHIFT 24U
#define EFR32_RAC_STATUS_STATE_MASK 0xFU
#define EFR32_RAC_STATE_OFF 0U
#define EFR32_RAC_CMD_TXEN (1U << 0)
#define EFR32_RAC_CMD_TXDIS (1U << 5)
#define EFR32_RAC_CMD_RXDIS (1U << 8)
#define EFR32_RAC_RXENSRCEN_SWRXEN (1U << 1)

#define EFR32_BUFC_BASE 0xBA000000UL
#define EFR32_BUFC_TX_BASE_OFFSET 0x00CU
#define EFR32_BUFC_RX_BASE_OFFSET 0x04CU
#define EFR32_BUFC_CFG_OFFSET 0x000U
#define EFR32_BUFC_ADDR_OFFSET 0x004U
#define EFR32_BUFC_WRITEOFFSET 0x008U
#define EFR32_BUFC_THRESHOLD_OFFSET 0x024U
#define EFR32_BUFC_CMD_OFFSET 0x028U
#define EFR32_BUFC_CMD_CLEAR 1U
#define EFR32_BUFC_CMD_PREFETCH 2U
#define EFR32_BUFC_FIFO_512_CONFIG 3U
#define EFR32_BUFC_RX_THRESHOLD 0x0FFFU

#define EFR32_RADIO_EN_OFFSET 0x004U

static inline volatile uint32_t *radio_reg32(uintptr_t address)
{
	return (volatile uint32_t *)address;
}

/*
 * The public RAIL configurator uses this compact, address-independent format
 * for radio tables. Do not put SEQ.PHYINFO records here: their data words are
 * pointers into the vendor image and are not portable to this firmware.
 */
#define EFR32_RADIO_TABLE_END 0xFFFFFFFFU
#define EFR32_RADIO_TABLE_OP_WRITE 0U
#define EFR32_RADIO_TABLE_OP_AND 1U
#define EFR32_RADIO_TABLE_OP_OR 3U

/*
 * Public xG22 BLE viterbi 1M configuration, copied as table operations rather
 * than as a vendor-addressed PHYINFO record. Source: the Zlib-licensed
 * ble_efr32xg22_configurator_out.c arrays at lines 225-319 and 474-534 in
 * the sibling Simplicity SDK checkout. radio_aa_set(), radio_crc_configure(),
 * and radio_whiten_iv_set() deliberately replace its dynamic defaults later.
 */
static const uint32_t efr32_ble_1m_base_table[] = {
	0x0001400CU, 0x00018101U, /* FRC.DFLCTRL */
	0x00024020U, 0x00000000U, 0x00000001U, /* FRC.WCNTCMP0..1 */
	0x00034030U, 0x00000000U, 0x00000000U, 0x00000000U, /* FRC.WHITE* */
	0x00024040U, 0x00000000U, 0x00004000U, /* FRC RAM addresses */
	0x00014060U, 0x00000101U, /* FRC.PUNCTCTRL */
	0x000140A8U, 0x00000007U, /* FRC.AUTOCG */
	0x000440BCU, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
	0x00034108U, 0x00004CFFU, 0x00004DFFU, 0x00004DFFU, /* FRC.FCD1..3 */
	0x1001C020U, 0x0007F800U, 0x3001C020U, 0x000802F5U, /* AGC.CTRL0 */
	0x1001C024U, 0x000000FFU, 0x3001C024U, 0x00001300U, /* AGC.CTRL1 */
	0x0007C028U, 0x83B380ECU, 0x51407543U, 0x48000FA0U, 0x00004030U,
		0x00000000U, 0x00000000U, 0x0000022EU, /* AGC.CTRL2..MANGAIN */
	0x0004C050U, 0x04301151U, 0xE6092D0EU, 0x08070654U, 0x0002B6D1U,
	0x000AC064U, 0x1C003004U, 0x09183040U, 0x2079640DU, 0x01FBFCEBU,
		0x03E8F67FU, 0x15724BBDU, 0x0518A311U, 0x76543210U,
		0x00000A98U, 0x00000000U, /* AGC.GAINRANGE..LBT */
	0x01010008U, 0x00000708U, /* RFCRC.CTRL */
	0x01010018U, 0x00AAAAAAU, /* RFCRC.INIT */
	0x01010020U, 0x00DA6000U, /* RFCRC.POLY */
	0x0101401CU, 0x00000010U, /* MODEM.MIXCTRL */
	0x0103402CU, 0x00008000U, 0x03000000U, 0x00000000U, /* MODEM.CTRL3..5 */
	0x01024068U, 0x71764129U, 0x00000000U, /* MODEM.SYNC0..1 */
	0x010240BCU, 0x00000000U, 0x00000000U, /* MODEM.SHAPING10..11 */
	0x010240E0U, 0x00000033U, 0x00000000U, /* MODEM.DCCOMP* */
	0x010240F4U, 0x07830464U, 0x3AC81388U, /* MODEM.DSATHD0..1 */
	0x01014124U, 0x00000000U, /* MODEM.AUTOCG */
	0x01024130U, 0x0C660664U, 0x0000010CU, /* MODEM.DSATHD2, DIRECTMODE */
	0x01014154U, 0x00000101U, /* MODEM.LRFRC */
	0x01024168U, 0x07830464U, 0x00821388U, /* MODEM.DSATHD3..4 */
	0x01014230U, 0x00000000U, /* MODEM.BLEIQDSA */
	0x01024238U, 0x00000000U, 0x00000000U, /* MODEM.SYNCPROPERTIES, DIGIGAINCTRL */
	0x01014244U, 0x00000000U, /* MODEM.PADEBUG */
	0x01094254U, 0x00000000U, 0x003C0000U, 0x00000000U, 0x00000000U,
		0x55555555U, 0x00000017U, 0x00000000U, 0x00000000U, 0x0006AAAAU,
	0x0102427CU, 0x00000000U, 0x00000000U, /* MODEM.ANTDIVCTRL, BLEIQDSAEXT2 */
	0x01018010U, 0x00000003U, /* SYNTH.CTRL */
	0x0101803CU, 0x00000001U, /* SYNTH.DIVCTRL */
	0x010280B0U, 0x02000300U, 0x01000037U, /* SYNTH.DSMCTRL* */
	0x02020098U, 0x04000C00U, 0x0000004CU, /* RAC.SYNTHREGCTRL, VCOCTRL */
	0x020100A4U, 0x00000400U, /* RAC.SYNTHCTRL */
	0x020200D0U, 0xAA400005U, 0x00000188U, /* RAC.CLKMULTEN* */
	0x020200F4U, 0x00000000U, 0x1108213DU, /* RAC.LNAMIX* */
	0x120100FCU, 0x0000003FU, 0x320100FCU, 0x00045400U, /* RAC.LNAMIXTRIM1 */
	0x02010130U, 0x02510060U, /* RAC.PGACTRL */
	0x02010168U, 0x00000400U, /* RAC.SYMMDCTRL */
	EFR32_RADIO_TABLE_END,
};

static const uint32_t efr32_ble_1m_delta_table[] = {
	/* The omitted first record is the non-portable SEQ.PHYINFO pointer. */
	0x00014010U, 0x00004101U, /* FRC.MAXLENGTH */
	0x0001403CU, 0x00000000U, /* FRC.FECCTRL */
	0x00014048U, 0x000407A0U, /* FRC.CTRL */
	0x00014050U, 0x00000000U, /* FRC.TRAILTXDATACTRL */
	0x0001405CU, 0x00000000U, /* FRC.CONVGENERATOR */
	0x00014104U, 0x00004CFFU, /* FRC.FCD0 */
	0x01034020U, 0x86000000U, 0x0001C01FU, 0x00006000U, /* MODEM.CTRL0..2 */
	0x01014038U, 0x40000000U, /* MODEM.CTRL6 */
	0x01044058U, 0x00FF04C8U, 0x00000840U, 0x00000001U, 0x00040092U,
	0x010F4080U, 0x020C008CU, 0x00000000U, 0x002203B6U, 0x00000000U,
		0x00000000U, 0x0E050200U, 0x53432F1CU, 0x61686962U, 0x04324355U,
		0x0E0E1016U, 0x080A0C0DU, 0x04050607U, 0x02020303U, 0x01010102U,
		0x00010101U, /* MODEM.TIMING..SHAPING9 */
	0x010140ECU, 0x8C2BE890U, /* MODEM.SRCCHF */
	0x010340FCU, 0x000A2090U, 0x80585B05U, 0x4A413747U, /* MODEM.DSA* */
	0x0103410CU, 0x005262F5U, 0x2949192AU, 0x40403B89U, /* MODEM.DIGMIX* */
	0x01014128U, 0x0000DDFFU, /* MODEM.CGCLKSTOP */
	0x01074138U, 0x00FA53E8U, 0x00000000U, 0x00000000U, 0x00000000U,
		0x00000000U, 0x00000000U, 0x00000000U, /* MODEM.LONGRANGE* */
	0x01014170U, 0x000003C1U, /* MODEM.VTBLETIMING */
	0x01014234U, 0x0E000000U, /* MODEM.BLEIQDSAEXT1 */
	0x01014248U, 0xC01EAA58U, /* MODEM.REALTIMCFE */
	0x01014278U, 0x00000000U, /* MODEM.COCURRMODE */
	0x01018038U, 0x00102555U, /* SYNTH.IFFREQ */
	0x0103809CU, 0x00000000U, 0x00037870U, 0x000000D0U, /* SYNTH.LPFCTRL1* */
	0x110180A8U, 0x000001F0U, 0x310180A8U, 0x01CB4205U, /* SYNTH.LPFCTRL2RX */
	0x110180ACU, 0x000001F0U, 0x310180ACU, 0x00FD3E05U, /* SYNTH.LPFCTRL2TX */
	0x020100E4U, 0x13512C6EU, /* RAC.IFADCTRIM0 */
	0x02010154U, 0x00023FC4U, /* RAC.SYTRIM1 */
	EFR32_RADIO_TABLE_END,
};

/* Public BLE viterbi 2M delta at configurator_out.c:537-598; PHYINFO omitted. */
static const uint32_t efr32_ble_2m_delta_table[] = {
	0x00014010U, 0x00004101U, /* FRC.MAXLENGTH */
	0x0001403CU, 0x00000000U, /* FRC.FECCTRL */
	0x00014048U, 0x000407A0U, /* FRC.CTRL */
	0x00014050U, 0x00000000U, /* FRC.TRAILTXDATACTRL */
	0x0001405CU, 0x00000000U, /* FRC.CONVGENERATOR */
	0x00014104U, 0x00004CFFU, /* FRC.FCD0 */
	0x01034020U, 0x86000000U, 0x0001C01FU, 0x00006000U, /* MODEM.CTRL0..2 */
	0x01014038U, 0x40000000U, /* MODEM.CTRL6 */
	0x01044058U, 0x00FF0264U, 0x00000840U, 0x00000001U, 0x00080092U,
	0x010F4080U, 0x020C008CU, 0x00000000U, 0x002203D7U, 0x00000000U,
		0x00000000U, 0x0D050200U, 0x503F2D1AU, 0x63696860U, 0x25354657U,
		0x08090D15U, 0x05050607U, 0x03040404U, 0x02030303U, 0x02020202U,
		0x01010101U, /* MODEM.TIMING..SHAPING9 */
	0x010140ECU, 0x8C2C3890U, /* MODEM.SRCCHF */
	0x010340FCU, 0x000A209CU, 0x80585B05U, 0x4A413747U, /* MODEM.DSA* */
	0x0103410CU, 0x005255B8U, 0x2949192AU, 0x40403B89U, /* MODEM.DIGMIX* */
	0x01014128U, 0x0000DDFFU, /* MODEM.CGCLKSTOP */
	0x01074138U, 0x00FA53E8U, 0x00000000U, 0x00000000U, 0x00000000U,
		0x00000000U, 0x00000000U, 0x00000000U, /* MODEM.LONGRANGE* */
	0x01014170U, 0x000003C1U, /* MODEM.VTBLETIMING */
	0x01014234U, 0x0E000000U, /* MODEM.BLEIQDSAEXT1 */
	0x01014248U, 0xC01EAA58U, /* MODEM.REALTIMCFE */
	0x01014278U, 0x00000000U, /* MODEM.COCURRMODE */
	0x01018038U, 0x00104911U, /* SYNTH.IFFREQ */
	0x0103809CU, 0x00000000U, 0x00037870U, 0x0002C0FFU, /* SYNTH.LPFCTRL1* */
	0x110180A8U, 0x000001F0U, 0x310180A8U, 0x01CB4205U, /* SYNTH.LPFCTRL2RX */
	0x110180ACU, 0x000001F0U, 0x310180ACU, 0x000D0A05U, /* SYNTH.LPFCTRL2TX */
	0x020100E4U, 0x11512C6CU, /* RAC.IFADCTRIM0 */
	0x02010154U, 0x00003FC4U, /* RAC.SYTRIM1 */
	EFR32_RADIO_TABLE_END,
};

/* Public BLE long-range 125 kb/s delta at configurator_out.c:600-661. */
static const uint32_t efr32_ble_coded_s8_delta_table[] = {
	/* The omitted first record is the non-portable SEQ.PHYINFO pointer. */
	0x00014010U, 0x00004100U, 0x0001403CU, 0x00000010U,
	0x00014048U, 0x00040720U, 0x00014050U, 0x00000400U,
	0x0001405CU, 0x00000F0DU, 0x00014104U, 0x0001C003U,
	0x01034020U, 0x46000030U, 0x0001C81FU, 0x10002000U,
	0x01014038U, 0x00000000U,
	0x01044058U, 0x00050018U, 0x00000840U, 0x00000009U, 0x000A0092U,
	0x010F4080U, 0x022C008CU, 0x00000000U, 0x00120012U, 0x1B8C805BU,
		0x00000000U, 0x0E050200U, 0x53432F1CU, 0x61686962U, 0x04324355U,
		0x0E0E1016U, 0x080A0C0DU, 0x04050607U, 0x02020303U, 0x01010102U,
		0x00010101U,
	0x010140ECU, 0x8CB1B080U,
	0x010340FCU, 0x000A209CU, 0x80206101U, 0x123556B7U,
	0x0103410CU, 0x001255B8U, 0x29041BA0U, 0x40408808U,
	0x01014128U, 0x00000000U,
	0x01074138U, 0x3BE8D44CU, 0x306F1904U, 0x1A140E08U, 0x322C2620U,
		0x32103E38U, 0x0A987654U, 0x00005F1DU,
	0x01014170U, 0x00000000U, 0x01014234U, 0x0E000800U,
	0x01014248U, 0x001F81F4U, 0x01014278U, 0x00000000U,
	0x01018038U, 0x00104911U,
	0x0103809CU, 0x000240EBU, 0x00033870U, 0x000000D0U,
	0x110180A8U, 0x000001F0U, 0x310180A8U, 0x01FD5A05U,
	0x110180ACU, 0x000001F0U, 0x310180ACU, 0x00FD3E05U,
	0x020100E4U, 0x11512C6CU, 0x02010154U, 0x00003FC4U,
	EFR32_RADIO_TABLE_END,
};

/* Public BLE long-range 500 kb/s delta at configurator_out.c:663-724. */
static const uint32_t efr32_ble_coded_s2_delta_table[] = {
	/* The omitted first record is the non-portable SEQ.PHYINFO pointer. */
	0x00014010U, 0x00004100U, 0x0001403CU, 0x00000010U,
	0x00014048U, 0x00041720U, 0x00014050U, 0x00000401U,
	0x0001405CU, 0x00000F0DU, 0x00014104U, 0x0001C003U,
	0x01034020U, 0x46000030U, 0x0001C81FU, 0x10002000U,
	0x01014038U, 0x00000000U,
	0x01044058U, 0x00050018U, 0x00000840U, 0x00000009U, 0x000A0092U,
	0x010F4080U, 0x022C008CU, 0x00000000U, 0x00120012U, 0x1B8C805BU,
		0x00000000U, 0x0E050200U, 0x53432F1CU, 0x61686962U, 0x04324355U,
		0x0E0E1016U, 0x080A0C0DU, 0x04050607U, 0x02020303U, 0x01010102U,
		0x00010101U,
	0x010140ECU, 0x8CB1B080U,
	0x010340FCU, 0x000A209CU, 0x80206101U, 0x123556B7U,
	0x0103410CU, 0x001255B8U, 0x29041BA0U, 0x40408808U,
	0x01014128U, 0x00000000U,
	0x01074138U, 0x3BE8D44CU, 0x306F1904U, 0x1C16100AU, 0x342E2822U,
		0x3210403AU, 0x0A987654U, 0xA4805F1DU,
	0x01014170U, 0x00000000U, 0x01014234U, 0x0E000800U,
	0x01014248U, 0x001F81F4U, 0x01014278U, 0x00000000U,
	0x01018038U, 0x00104911U,
	0x0103809CU, 0x000240EBU, 0x00033870U, 0x000000D0U,
	0x110180A8U, 0x000001F0U, 0x310180A8U, 0x01FD5A05U,
	0x110180ACU, 0x000001F0U, 0x310180ACU, 0x00FD3E05U,
	0x020100E4U, 0x11512C6CU, 0x02010154U, 0x00003FC4U,
	EFR32_RADIO_TABLE_END,
};

static void efr32_radio_table_apply(const uint32_t *table)
{
	for (size_t index = 0U; table[index] != EFR32_RADIO_TABLE_END;) {
		uint32_t header = table[index++];
		uint32_t operation = header >> 28;
		uint32_t block = (header >> 24) & 0xFU;
		uint32_t count = (header >> 16) & 0xFFU;
		/*
		 * The public configurator encodes secure 0xA8xx_xxxx block
		 * addresses. Zephyr's xG22 radio devicetree exposes radio RAM at
		 * 0xB0000000, and the generated *_NS models place these blocks in
		 * the non-secure 0xB8xx_xxxx window used by this application.
		 */
		uintptr_t address = 0xB8000000UL + ((uintptr_t)block << 16) +
			(header & 0xFFFFU);

		LL_ASSERT(count != 0U);
		for (uint32_t word = 0U; word < count; word++, address += sizeof(uint32_t)) {
			uint32_t value = table[index++];

			switch (operation) {
			case EFR32_RADIO_TABLE_OP_WRITE:
				*radio_reg32(address) = value;
				break;
			case EFR32_RADIO_TABLE_OP_AND:
				*radio_reg32(address) &= value;
				break;
			case EFR32_RADIO_TABLE_OP_OR:
				*radio_reg32(address) |= value;
				break;
			default:
				LL_ASSERT(false);
			}
		}
	}
}

static uint32_t bit_reverse24(uint32_t value)
{
	uint32_t reversed = 0U;

	for (uint32_t bit = 0U; bit < 24U; bit++) {
		reversed = (reversed << 1) | (value & 1U);
		value >>= 1;
	}

	return reversed;
}

static struct {
	struct ccm config;
	struct pdu_data *rx_in;
	struct pdu_data *rx_out;
	bool pending_rx;
	bool done;
	bool mic_valid;
	bool empty;
} ccm_ctx;

static void ccm_nonce_build(const struct ccm *ccm, uint8_t nonce[BLE_CCM_NONCE_SIZE])
{
	uint64_t counter = ccm->counter;

	memcpy(nonce, &counter, 5);
	nonce[4] |= ccm->direction << 7;
	memcpy(&nonce[5], ccm->iv, sizeof(ccm->iv));
}

static psa_key_id_t ccm_key_import(const struct ccm *ccm, psa_key_usage_t usage)
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_key_id_t id = 0;
	psa_algorithm_t alg = PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, BLE_CCM_MIC_SIZE);

	if (psa_crypto_init() != PSA_SUCCESS) {
		return 0;
	}
	psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
	psa_set_key_bits(&attr, 128);
	psa_set_key_usage_flags(&attr, usage);
	psa_set_key_algorithm(&attr, alg);
	if (psa_import_key(&attr, ccm->key, sizeof(ccm->key), &id) != PSA_SUCCESS) {
		return 0;
	}

	return id;
}


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
	EFR32_RADIO_TODO("IRQ routing (T1)");
}

/* [regs-public] Enable the six blocks whose public xG22 models expose EN.EN. */
void radio_setup(void)
{
	/*
	 * Every address below is a rev-B2 non-secure EN register at +0x004, with
	 * the sole writable field EN.EN at bit zero. This deliberately does not
	 * claim an ordering, reset protocol, BUFC enable, or IRQ event policy.
	 */
	*radio_reg32(EFR32_FRC_BASE + EFR32_RADIO_EN_OFFSET) = 1U;
	*radio_reg32(EFR32_AGC_BASE + EFR32_RADIO_EN_OFFSET) = 1U;
	*radio_reg32(EFR32_RFCRC_BASE + EFR32_RADIO_EN_OFFSET) = 1U;
	*radio_reg32(EFR32_MODEM_BASE + EFR32_RADIO_EN_OFFSET) = 1U;
	*radio_reg32(EFR32_SYNTH_BASE + EFR32_RADIO_EN_OFFSET) = 1U;
	*radio_reg32(EFR32_RAC_BASE + EFR32_RADIO_EN_OFFSET) = 1U;

	/*
	 * The LLL connects its radio ISR to FRC_IRQn.  Enable only the two
	 * completion latches it observes, rather than treating a RAC state change
	 * or an unrelated FRC condition as an OpenISA radio completion.
	 */
	*radio_reg32(EFR32_FRC_BASE + EFR32_REGISTER_SET_OFFSET +
		     EFR32_FRC_IEN_OFFSET) = EFR32_FRC_IF_COMPLETION;
}

/* [regs] IRQ routing, block enables and reset sequence: RAC/FRC/BUFC/MODEM/SYNTH/AGC EN registers and RAC reset (docs/radio-tables.md T1, T2) */
void radio_reset(void)
{
	EFR32_RADIO_TODO("IRQ routing, block enables and reset sequence");
}

/* [regs-public] Address-independent BLE 1M table; PHYINFO remains intentionally excluded (T3). */
void radio_phy_set(uint8_t phy, uint8_t flags)
{
	ARG_UNUSED(flags);

	switch (phy) {
	case PHY_LEGACY:
	case PHY_1M:
		efr32_radio_table_apply(efr32_ble_1m_base_table);
		efr32_radio_table_apply(efr32_ble_1m_delta_table);
		break;
	case PHY_2M:
		efr32_radio_table_apply(efr32_ble_1m_base_table);
		efr32_radio_table_apply(efr32_ble_2m_delta_table);
		break;
	case PHY_CODED:
		efr32_radio_table_apply(efr32_ble_1m_base_table);
		if (flags == PHY_FLAGS_S8) {
			efr32_radio_table_apply(efr32_ble_coded_s8_delta_table);
		} else {
			LL_ASSERT(flags == PHY_FLAGS_S2);
			efr32_radio_table_apply(efr32_ble_coded_s2_delta_table);
		}
		break;
	default:
		EFR32_RADIO_TODO("regs-public: unknown PHY");
		break;
	}
}

/* [regs] PA registers (rfhal_pa): unknown (T7) */
void radio_tx_power_set(uint32_t power)
{
	ARG_UNUSED(power);
	EFR32_RADIO_TODO("PA registers (rfhal_pa)");
}

/* [regs] PA registers (rfhal_pa): unknown (T7) */
void radio_tx_power_max_set(void)
{
	EFR32_RADIO_TODO("PA registers (rfhal_pa)");
}

/* [regs-public] BLE base=2402 MHz, spacing=2 MHz; CHCTRL.CHNO is bits 0..5 (T4). */
void radio_freq_chan_set(uint32_t chan)
{
	uint32_t channel_number;

	/* Zephyr passes MHz relative to 2400: 2..80, always even. */
	LL_ASSERT((chan >= 2U) && (chan <= 80U) && ((chan & 1U) == 0U));
	channel_number = (chan - 2U) / 2U;
	*radio_reg32(EFR32_SYNTH_BASE + EFR32_SYNTH_CHCTRL_OFFSET) =
		channel_number & EFR32_SYNTH_CHCTRL_CHNO_MASK;
}

/* [regs-public] RAIL's BLE helper writes FRC.WHITEPOLY=0x44 and WHITEINIT=iv|0x40 (T5). */
void radio_whiten_iv_set(uint32_t iv)
{
	/* LLL supplies a Bluetooth physical channel number, 0 through 39. */
	LL_ASSERT(iv <= 39U);
	*radio_reg32(EFR32_FRC_BASE + EFR32_FRC_WHITEPOLY_OFFSET) = EFR32_FRC_BLE_WHITEPOLY;
	*radio_reg32(EFR32_FRC_BASE + EFR32_FRC_WHITEINIT_OFFSET) =
		EFR32_FRC_BLE_WHITEINIT_MSB | iv;
}

/* [regs-public] MODEM.SYNC0 is the 32-bit BLE synchronization word register (T3). */
void radio_aa_set(const uint8_t *aa)
{
	/*
	 * OpenISA callers pass the Bluetooth access address in little-endian byte
	 * order.  Assemble it explicitly rather than relying on aa being aligned
	 * for a uint32_t load (the API only promises a byte pointer).
	 */
	uint32_t sync0 = (uint32_t)aa[0] |
			 ((uint32_t)aa[1] << 8) |
			 ((uint32_t)aa[2] << 16) |
			 ((uint32_t)aa[3] << 24);

	*radio_reg32(EFR32_MODEM_BASE + EFR32_MODEM_SYNC0_OFFSET) = sync0;
}

/* [regs-partial] FRC length/header configuration remains unproven (T5;
 * docs/radio-tables.md §4.2). CRC, whitening, and access-address
 * configuration are implemented above. */
void radio_pkt_configure(uint8_t bits_len, uint8_t max_len, uint8_t flags)
{
	ARG_UNUSED(bits_len);
	ARG_UNUSED(max_len);
	ARG_UNUSED(flags);
	EFR32_RADIO_TODO("FRC/CRC/whitening/sync-word registers");
}

/*
 * [regs-partial] Configure only the source-backed BUFC RX staging FIFO.
 * FRC's delivery of a received packet to this FIFO remains unproven (T5).
 */
void radio_pkt_rx_set(void *rx_packet)
{
	uintptr_t base = EFR32_BUFC_BASE + EFR32_BUFC_RX_BASE_OFFSET;

	ARG_UNUSED(rx_packet);
	LL_ASSERT(((uintptr_t)efr32_rx_fifo & 3U) == 0U);

	/*
	 * RAIL's RX FIFO helper configures channel 1 in this exact order: CLEAR,
	 * capacity encoding, backing address, and threshold control. Unlike TX,
	 * its helper does not issue PREFETCH. Keep the caller buffer untouched
	 * until the unproven FRC packet handoff can be implemented.
	 */
	*radio_reg32(base + EFR32_BUFC_CMD_OFFSET) = EFR32_BUFC_CMD_CLEAR;
	*radio_reg32(base + EFR32_BUFC_CFG_OFFSET) = EFR32_BUFC_FIFO_512_CONFIG;
	*radio_reg32(base + EFR32_BUFC_ADDR_OFFSET) = (uint32_t)(uintptr_t)efr32_rx_fifo;
	*radio_reg32(base + EFR32_BUFC_THRESHOLD_OFFSET) = EFR32_BUFC_RX_THRESHOLD;
}

/*
 * [regs-partial] Configure only the source-backed BUFC TX staging FIFO.
 * FRC's consumption of that FIFO and sequencer handoff remain unproven (T5).
 */
void radio_pkt_tx_set(void *tx_packet)
{
	const uint8_t *pdu = tx_packet;
	uint32_t bytes = (uint32_t)pdu[1] + 2U;
	uintptr_t base = EFR32_BUFC_BASE + EFR32_BUFC_TX_BASE_OFFSET;

	/* OpenISA PDUs are header byte, payload-length byte, then payload. */
	LL_ASSERT(bytes <= sizeof(efr32_tx_fifo));
	LL_ASSERT(((uintptr_t)efr32_tx_fifo & 3U) == 0U);
	memcpy(efr32_tx_fifo, pdu, bytes);

	/*
	 * Exact RAIL SetTxFifo order for a 512-byte FIFO: CLEAR, configuration
	 * value log2(512)-6, address, initial byte count, then PREFETCH.
	 */
	*radio_reg32(base + EFR32_BUFC_CMD_OFFSET) = EFR32_BUFC_CMD_CLEAR;
	*radio_reg32(base + EFR32_BUFC_CFG_OFFSET) = EFR32_BUFC_FIFO_512_CONFIG;
	*radio_reg32(base + EFR32_BUFC_ADDR_OFFSET) = (uint32_t)(uintptr_t)efr32_tx_fifo;
	*radio_reg32(base + EFR32_BUFC_WRITEOFFSET) = bytes;
	*radio_reg32(base + EFR32_BUFC_CMD_OFFSET) = EFR32_BUFC_CMD_PREFETCH;
}

/* [software] software (no register access needed); not implemented yet */
uint32_t radio_tx_ready_delay_get(uint8_t phy, uint8_t flags)
{
	ARG_UNUSED(phy);
	ARG_UNUSED(flags);
	EFR32_RADIO_TODO("software");
	return 0;
}

/* [software] software (no register access needed); not implemented yet */
uint32_t radio_tx_chain_delay_get(uint8_t phy, uint8_t flags)
{
	ARG_UNUSED(phy);
	ARG_UNUSED(flags);
	EFR32_RADIO_TODO("software");
	return 0;
}

/* [software] software (no register access needed); not implemented yet */
uint32_t radio_rx_ready_delay_get(uint8_t phy, uint8_t flags)
{
	ARG_UNUSED(phy);
	ARG_UNUSED(flags);
	EFR32_RADIO_TODO("software");
	return 0;
}

/* [software] software (no register access needed); not implemented yet */
uint32_t radio_rx_chain_delay_get(uint8_t phy, uint8_t flags)
{
	ARG_UNUSED(phy);
	ARG_UNUSED(flags);
	EFR32_RADIO_TODO("software");
	return 0;
}

/*
 * [regs-vendor] RAIL StartRx writes RXENSRCEN.SET.SWRXEN (bit 1). The
 * archived primitive is at librail_efr32xg22_gcc_release.a(rail_rf_hal.o),
 * sli_rail_dadf1c52f26190429dcacf7151424c68; see radio-tables.md §4.1.
 */
void radio_rx_enable(void)
{
	while (!radio_is_idle()) {
	}

	*radio_reg32(EFR32_RAC_BASE + EFR32_REGISTER_SET_OFFSET +
		     EFR32_RAC_RXENSRCEN_OFFSET) = EFR32_RAC_RXENSRCEN_SWRXEN;
}

/* [regs-public] RAC.CMD.TXEN is a write-only command bit (T2). */
void radio_tx_enable(void)
{
	/* Match the OpenISA radio contract: do not start a new TX while active. */
	while (!radio_is_idle()) {
	}

	*radio_reg32(EFR32_RAC_BASE + EFR32_RAC_CMD_OFFSET) = EFR32_RAC_CMD_TXEN;
}

/* [regs-public] RAC.CMD TXDIS/RXDIS are write-only command bits (T2). */
void radio_disable(void)
{
	/* Keep independent command writes: their combined hardware semantics are unverified. */
	*radio_reg32(EFR32_RAC_BASE + EFR32_RAC_CMD_OFFSET) = EFR32_RAC_CMD_TXDIS;
	*radio_reg32(EFR32_RAC_BASE + EFR32_RAC_CMD_OFFSET) = EFR32_RAC_CMD_RXDIS;
}

/* [regs-public] FRC.IF has named TXDONE and RXDONE completion latches. */
void radio_status_reset(void)
{
	*radio_reg32(EFR32_FRC_BASE + EFR32_REGISTER_CLEAR_OFFSET +
		     EFR32_FRC_IF_OFFSET) = EFR32_FRC_IF_COMPLETION;
}

/* [regs] RAC command/state and FRC/BUFC status/IF bits: unknown (T2) */
uint32_t radio_is_ready(void)
{
	EFR32_RADIO_TODO("regs");
	return 0;
}

/* [regs-public] FRC.IF.TXDONE/RXDONE are the completion latches (T2). */
uint32_t radio_is_done(void)
{
	return !!(*radio_reg32(EFR32_FRC_BASE + EFR32_FRC_IF_OFFSET) &
		 EFR32_FRC_IF_COMPLETION);
}

/* [regs] RAC command/state and FRC/BUFC status/IF bits: unknown (T2) */
uint32_t radio_has_disabled(void)
{
	EFR32_RADIO_TODO("regs");
	return 0;
}

/* [regs-public] RAC.STATUS.STATE is read-only and OFF is state 0 (T2). */
uint32_t radio_is_idle(void)
{
	return ((*radio_reg32(EFR32_RAC_BASE + EFR32_RAC_STATUS_OFFSET) >>
		 EFR32_RAC_STATUS_STATE_SHIFT) & EFR32_RAC_STATUS_STATE_MASK) ==
		EFR32_RAC_STATE_OFF;
}

/* [regs-public] RFCRC values match BLE's 24-bit CRC after bit reversal (T5). */
void radio_crc_configure(uint32_t polynomial, uint32_t iv)
{
	LL_ASSERT((polynomial & ~0x00FFFFFFU) == 0U);
	LL_ASSERT((iv & ~0x00FFFFFFU) == 0U);
	*radio_reg32(EFR32_RFCRC_BASE + EFR32_RFCRC_EN_OFFSET) = 1U;
	*radio_reg32(EFR32_RFCRC_BASE + EFR32_RFCRC_CTRL_OFFSET) = EFR32_RFCRC_BLE_CTRL;
	*radio_reg32(EFR32_RFCRC_BASE + EFR32_RFCRC_INIT_OFFSET) = bit_reverse24(iv);
	*radio_reg32(EFR32_RFCRC_BASE + EFR32_RFCRC_POLY_OFFSET) = bit_reverse24(polynomial);
}

/* [regs-public] FRC.TRAILRXDATA.CRCOK records the completed RX CRC result. */
uint32_t radio_crc_is_valid(void)
{
	return !!(*radio_reg32(EFR32_FRC_BASE + EFR32_FRC_TRAILRXDATA_OFFSET) &
		 EFR32_FRC_TRAILRXDATA_CRCOK);
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
	EFR32_RADIO_TODO("software TIFS on top of PROTIMER compare + RAC com");
}

/* [regs] software TIFS on top of PROTIMER compare + RAC commands (T2, T8) */
void radio_switch_complete_and_tx(uint8_t phy_rx, uint8_t flags_rx, uint8_t phy_tx,
				  uint8_t flags_tx)
{
	ARG_UNUSED(phy_rx);
	ARG_UNUSED(flags_rx);
	ARG_UNUSED(phy_tx);
	ARG_UNUSED(flags_tx);
	EFR32_RADIO_TODO("software TIFS on top of PROTIMER compare + RAC com");
}

/* [regs] software TIFS on top of PROTIMER compare + RAC commands (T2, T8) */
void radio_switch_complete_and_disable(void)
{
	EFR32_RADIO_TODO("software TIFS on top of PROTIMER compare + RAC com");
}

/* [regs-partial] AGC RSSI/FRAMERSSI fields are public; measurement policy is unknown (T9) */
void radio_rssi_measure(void)
{
	EFR32_RADIO_TODO("AGC RSSI registers");
}

/*
 * RAIL reads FRAMERSSI rather than the live RSSI register for a received
 * packet. The encoded 10-bit value is signed quarter-dB with RAIL's
 * source-backed -200 quarter-dB offset. OpenISA stores RSSI as a positive
 * magnitude, so negate the resulting signed dB value at the API boundary.
 */
uint32_t radio_rssi_get(void)
{
	int32_t code = (*radio_reg32(EFR32_AGC_BASE + EFR32_AGC_FRAMERSSI_OFFSET) >>
			EFR32_AGC_RSSI_CODE_SHIFT) & EFR32_AGC_RSSI_CODE_MASK;
	int32_t rssi_quarter_db;
	int32_t rssi_db;

	if (code & 0x200) {
		code -= 0x400;
	}
	if (code == EFR32_AGC_RSSI_INVALID) {
		return 127U;
	}

	rssi_quarter_db = code - EFR32_AGC_RSSI_OFFSET_QUARTER_DB;
	if (rssi_quarter_db < -511) {
		rssi_quarter_db = -511;
	}
	rssi_db = rssi_quarter_db / 4;

	return rssi_db < 0 ? (uint32_t)-rssi_db : 0U;
}

/* [regs-partial] AGC RSSI/FRAMERSSI fields are public; latch/reset semantics are unknown (T9) */
void radio_rssi_status_reset(void)
{
	EFR32_RADIO_TODO("AGC RSSI registers");
}

/* [regs-partial] AGC RSSI/FRAMERSSI fields are public; readiness semantics are unknown (T9) */
uint32_t radio_rssi_is_ready(void)
{
	EFR32_RADIO_TODO("regs");
	return 0;
}

/* [software] software (no register access needed); not implemented yet */
void radio_filter_configure(uint8_t bitmask_enable, uint8_t bitmask_addr_type,
			    uint8_t *bdaddr)
{
	ARG_UNUSED(bitmask_enable);
	ARG_UNUSED(bitmask_addr_type);
	ARG_UNUSED(bdaddr);
	EFR32_RADIO_TODO("software filter");
}

/* [software] software (no register access needed); not implemented yet */
void radio_filter_disable(void)
{
	EFR32_RADIO_TODO("software filter");
}

/* [software] software (no register access needed); not implemented yet */
void radio_filter_status_reset(void)
{
	EFR32_RADIO_TODO("software filter");
}

/* [software] software (no register access needed); not implemented yet */
uint32_t radio_filter_has_match(void)
{
	EFR32_RADIO_TODO("software");
	return 0;
}

/* [software] software (no register access needed); not implemented yet */
uint32_t radio_filter_match_get(void)
{
	EFR32_RADIO_TODO("software");
	return 0;
}

/* [software] software (no register access needed); not implemented yet */
void radio_bc_configure(uint32_t n)
{
	ARG_UNUSED(n);
	EFR32_RADIO_TODO("software filter");
}

/* [software] software (no register access needed); not implemented yet */
void radio_bc_status_reset(void)
{
	EFR32_RADIO_TODO("software filter");
}

/* [software] software (no register access needed); not implemented yet */
uint32_t radio_bc_has_match(void)
{
	EFR32_RADIO_TODO("software");
	return 0;
}

/* [regs] PROTIMER (timebase, compare, captures) and RTC sync: unknown (T8) */
void radio_tmr_status_reset(void)
{
	EFR32_RADIO_TODO("PROTIMER (timebase, compare, captures) and RTC syn");
}

/* [regs] software TIFS on top of PROTIMER compare + RAC commands (T2, T8) */
void radio_tmr_tifs_set(uint32_t tifs)
{
	ARG_UNUSED(tifs);
	EFR32_RADIO_TODO("software TIFS on top of PROTIMER compare + RAC com");
}

/* [regs] PROTIMER (timebase, compare, captures) and RTC sync: unknown (T8) */
uint32_t radio_tmr_start(uint8_t trx, uint32_t ticks_start, uint32_t remainder)
{
	ARG_UNUSED(trx);
	ARG_UNUSED(ticks_start);
	ARG_UNUSED(remainder);
	EFR32_RADIO_AIR_TODO("regs");
	return 0;
}

/* [regs] PROTIMER (timebase, compare, captures) and RTC sync: unknown (T8) */
uint32_t radio_tmr_start_tick(uint8_t trx, uint32_t tick)
{
	ARG_UNUSED(trx);
	ARG_UNUSED(tick);
	EFR32_RADIO_AIR_TODO("regs");
	return 0;
}

/* [regs] PROTIMER (timebase, compare, captures) and RTC sync: unknown (T8) */
void radio_tmr_start_us(uint8_t trx, uint32_t us)
{
	ARG_UNUSED(trx);
	ARG_UNUSED(us);
	EFR32_RADIO_AIR_TODO("PROTIMER (timebase, compare, captures) and RTC syn");
}

/* [regs] PROTIMER (timebase, compare, captures) and RTC sync: unknown (T8) */
uint32_t radio_tmr_start_now(uint8_t trx)
{
	ARG_UNUSED(trx);
	EFR32_RADIO_AIR_TODO("regs");
	return 0;
}

/* [regs] PROTIMER (timebase, compare, captures) and RTC sync: unknown (T8) */
uint32_t radio_tmr_start_get(void)
{
	EFR32_RADIO_TODO("regs");
	return 0;
}

/* [regs] PROTIMER (timebase, compare, captures) and RTC sync: unknown (T8) */
void radio_tmr_stop(void)
{
	EFR32_RADIO_TODO("PROTIMER (timebase, compare, captures) and RTC syn");
}

/* [regs] PROTIMER (timebase, compare, captures) and RTC sync: unknown (T8) */
void radio_tmr_hcto_configure(uint32_t hcto)
{
	ARG_UNUSED(hcto);
	EFR32_RADIO_TODO("PROTIMER (timebase, compare, captures) and RTC syn");
}

/* [regs] PROTIMER (timebase, compare, captures) and RTC sync: unknown (T8) */
void radio_tmr_aa_capture(void)
{
	EFR32_RADIO_TODO("PROTIMER (timebase, compare, captures) and RTC syn");
}

/* [regs] PROTIMER (timebase, compare, captures) and RTC sync: unknown (T8) */
uint32_t radio_tmr_aa_get(void)
{
	EFR32_RADIO_TODO("regs");
	return 0;
}

/* [regs] PROTIMER (timebase, compare, captures) and RTC sync: unknown (T8) */
void radio_tmr_aa_save(uint32_t aa)
{
	ARG_UNUSED(aa);
	EFR32_RADIO_TODO("PROTIMER (timebase, compare, captures) and RTC syn");
}

/* [regs] PROTIMER (timebase, compare, captures) and RTC sync: unknown (T8) */
uint32_t radio_tmr_aa_restore(void)
{
	EFR32_RADIO_TODO("regs");
	return 0;
}

/* [regs] PROTIMER (timebase, compare, captures) and RTC sync: unknown (T8) */
uint32_t radio_tmr_ready_get(void)
{
	EFR32_RADIO_TODO("regs");
	return 0;
}

/* [regs] PROTIMER (timebase, compare, captures) and RTC sync: unknown (T8) */
void radio_tmr_end_capture(void)
{
	EFR32_RADIO_TODO("PROTIMER (timebase, compare, captures) and RTC syn");
}

/* [regs] PROTIMER (timebase, compare, captures) and RTC sync: unknown (T8) */
uint32_t radio_tmr_end_get(void)
{
	EFR32_RADIO_TODO("regs");
	return 0;
}

/* [regs] software TIFS on top of PROTIMER compare + RAC commands (T2, T8) */
uint32_t radio_tmr_tifs_base_get(void)
{
	EFR32_RADIO_TODO("regs");
	return 0;
}

/* [regs] PROTIMER (timebase, compare, captures) and RTC sync: unknown (T8) */
void radio_tmr_sample(void)
{
	EFR32_RADIO_TODO("PROTIMER (timebase, compare, captures) and RTC syn");
}

/* [regs] PROTIMER (timebase, compare, captures) and RTC sync: unknown (T8) */
uint32_t radio_tmr_sample_get(void)
{
	EFR32_RADIO_TODO("regs");
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

/* [software] Synchronous BLE AES-CCM through PSA. */
void *radio_ccm_rx_pkt_set(struct ccm *ccm, uint8_t phy, void *pkt)
{
	ARG_UNUSED(phy);
	memcpy(&ccm_ctx.config, ccm, sizeof(*ccm));
	ccm_ctx.rx_in = (struct pdu_data *)pkt_scratch;
	ccm_ctx.rx_out = pkt;
	ccm_ctx.pending_rx = true;
	ccm_ctx.done = false;
	ccm_ctx.mic_valid = false;
	ccm_ctx.empty = false;

	return pkt_scratch;
}

/* [software] Encrypts synchronously; returned storage remains valid until the next radio packet. */
void *radio_ccm_tx_pkt_set(struct ccm *ccm, void *pkt)
{
	struct pdu_data *in = pkt;
	uint8_t nonce[BLE_CCM_NONCE_SIZE];
	uint8_t aad = *(uint8_t *)pkt & BLE_CCM_AAD_MASK;
	psa_algorithm_t alg = PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, BLE_CCM_MIC_SIZE);
	psa_key_id_t id;
	size_t out_len;
	psa_status_t status;

	ccm_ctx.pending_rx = false;
	ccm_ctx.done = false;
	ccm_ctx.mic_valid = false;
	ccm_ctx.empty = false;
	if (in->len == 0U) {
		ccm_ctx.done = true;
		return pkt;
	}

	id = ccm_key_import(ccm, PSA_KEY_USAGE_ENCRYPT);
	if (id == 0) {
		return NULL;
	}
	ccm_nonce_build(ccm, nonce);
	status = psa_aead_encrypt(id, alg, nonce, sizeof(nonce), &aad, sizeof(aad),
				  (uint8_t *)pkt + 2, in->len, pkt_scratch + 2,
				  sizeof(pkt_scratch) - 2, &out_len);
	(void)psa_destroy_key(id);
	if (status != PSA_SUCCESS || out_len != in->len + BLE_CCM_MIC_SIZE) {
		return NULL;
	}

	pkt_scratch[0] = *(uint8_t *)pkt;
	pkt_scratch[1] = in->len + BLE_CCM_MIC_SIZE;
	ccm_ctx.done = true;
	return pkt_scratch;
}

/* [software] Completes a pending receive decrypt synchronously. */
uint32_t radio_ccm_is_done(void)
{
	uint8_t nonce[BLE_CCM_NONCE_SIZE];
	uint8_t aad;
	psa_algorithm_t alg = PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, BLE_CCM_MIC_SIZE);
	psa_key_id_t id;
	size_t out_len;
	psa_status_t status;

	if (!ccm_ctx.pending_rx) {
		return ccm_ctx.done;
	}
	ccm_ctx.pending_rx = false;
	if (ccm_ctx.rx_in->len == 0U) {
		memcpy(ccm_ctx.rx_out, ccm_ctx.rx_in, 2);
		ccm_ctx.empty = true;
		ccm_ctx.mic_valid = true;
		ccm_ctx.done = true;
		return 1;
	}
	if (ccm_ctx.rx_in->len <= BLE_CCM_MIC_SIZE) {
		return 0;
	}

	id = ccm_key_import(&ccm_ctx.config, PSA_KEY_USAGE_DECRYPT);
	if (id == 0) {
		return 0;
	}
	ccm_nonce_build(&ccm_ctx.config, nonce);
	aad = *(uint8_t *)ccm_ctx.rx_in & BLE_CCM_AAD_MASK;
	status = psa_aead_decrypt(id, alg, nonce, sizeof(nonce), &aad, sizeof(aad),
				  (uint8_t *)ccm_ctx.rx_in + 2, ccm_ctx.rx_in->len,
				  (uint8_t *)ccm_ctx.rx_out + 2,
				  ccm_ctx.rx_in->len - BLE_CCM_MIC_SIZE, &out_len);
	(void)psa_destroy_key(id);
	if (status != PSA_SUCCESS || out_len != ccm_ctx.rx_in->len - BLE_CCM_MIC_SIZE) {
		return 0;
	}

	*(uint8_t *)ccm_ctx.rx_out = *(uint8_t *)ccm_ctx.rx_in;
	ccm_ctx.rx_out->len = out_len;
	ccm_ctx.mic_valid = true;
	ccm_ctx.done = true;
	return 1;
}

/* [software] */
uint32_t radio_ccm_mic_is_valid(void)
{
	return ccm_ctx.mic_valid;
}

/* [software] OpenISA uses this to report an empty received PDU. */
uint32_t radio_ccm_is_available(void)
{
	return ccm_ctx.empty;
}

/* [software] software (no register access needed); not implemented yet */
void radio_ar_configure(uint32_t nirk, void *irk)
{
	ARG_UNUSED(nirk);
	ARG_UNUSED(irk);
	EFR32_RADIO_TODO("software: software (no register access needed); not implemen");
}

/* [software] software (no register access needed); not implemented yet */
uint32_t radio_ar_match_get(void)
{
	EFR32_RADIO_TODO("software");
	return 0;
}

/* [software] software (no register access needed); not implemented yet */
void radio_ar_status_reset(void)
{
	EFR32_RADIO_TODO("software: software (no register access needed); not implemen");
}

/* [software] software (no register access needed); not implemented yet */
uint32_t radio_ar_has_match(void)
{
	EFR32_RADIO_TODO("software");
	return 0;
}

/* [regs] IRQ routing, block enables and reset sequence: RAC/FRC/BUFC/MODEM/SYNTH/AGC EN registers and RAC reset (docs/radio-tables.md T1, T2) */
uint32_t radio_sleep(void)
{
	EFR32_RADIO_TODO("regs");
	return 0;
}

/* [regs] IRQ routing, block enables and reset sequence: RAC/FRC/BUFC/MODEM/SYNTH/AGC EN registers and RAC reset (docs/radio-tables.md T1, T2) */
uint32_t radio_wake(void)
{
	EFR32_RADIO_TODO("regs");
	return 0;
}

/* [regs] IRQ routing, block enables and reset sequence: RAC/FRC/BUFC/MODEM/SYNTH/AGC EN registers and RAC reset (docs/radio-tables.md T1, T2) */
uint32_t radio_is_off(void)
{
	EFR32_RADIO_TODO("regs");
	return 0;
}
