/*
 * Timing constants and shared words of the open radio sequencer (sequencer/radio_seq.S, M0+) and
 * the M33 HAL. Preprocessor definitions only: the sequencer source includes this file too.
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

/*
 * Timed commands (docs/radio-tables.md SS7.82). The M33 posts a radio command (TXEN, SWRXEN)
 * with the instant it is due, as a count of TIMER0, the scheduler clock's active provider, and
 * rings RDMAILBOX1; the sequencer's mailbox handler polls TIMER0.CNT through the non-secure
 * peripheral window (SS7.74: the M0+ reaches TIMER1 at 0x5004C024 there; TIMER0 is 0x50048000,
 * efr32bg22 device header TIMER0_NS_BASE) and issues the command at the instant. No M33
 * interrupt can delay it. The block lives in SEQRAM below the other shared words.
 *
 * OP: bits 7:0 the command, bit 8 SWRXEN right after TXEN (a TX that turns to RX), bits 31:16 a
 * generation; written last, after TARGET and LIMIT. The M33 cancels by writing OP_NONE.
 * TARGET: TIMER0 count of the instant. LIMIT: the latest count at which the command may still
 * be issued; past it the sequencer issues nothing and reports EFR32_SEQ_TIMED_LATE.
 * DONE: the sequencer echoes OP when it has acted on it, with EFR32_SEQ_TIMED_LATE or
 * EFR32_SEQ_TIMED_CANCELLED set when it did not issue the command. AT: TIMER0 read right after
 * the command store. ENTRY: TIMER0 at the handler's first poll (wake latency, diagnostic).
 */
#define EFR32_SEQ_TIMED_BLOCK 0x00004e00
#define EFR32_SEQ_TIMED_OP 0x00
#define EFR32_SEQ_TIMED_TARGET 0x04
#define EFR32_SEQ_TIMED_LIMIT 0x08
#define EFR32_SEQ_TIMED_DONE 0x0c
#define EFR32_SEQ_TIMED_AT 0x10
#define EFR32_SEQ_TIMED_ENTRY 0x14
/*
 * Diagnostic: TIMER1.CNT right after ENTRY and right after AT, when TIMER1's bus clock is on
 * (a debugger's PRS capture setup; TIMER1 is otherwise unused). They relate TIMER0's instant to
 * TIMER1 input captures of RAC's activity signal (docs/radio-tables.md 7.82).
 */
#define EFR32_SEQ_TIMED_ENTRY_T1 0x18
#define EFR32_SEQ_TIMED_AT_T1 0x1c
/*
 * Diagnostic, sequencer built with EFR32_SEQ_WAKE_PROBE (Kconfig EFR32_SEQ_WAKE_PROBE): the
 * M0+'s SysTick (counting down at its core clock) at every exception entry (EXC_ST, scratch),
 * and, copied at the timed command's first poll, at its own entry (ENTRY_ST) and at that poll
 * (POLL_ST) (docs/radio-tables.md 7.94).
 */
#define EFR32_SEQ_TIMED_EXC_ST 0x20
#define EFR32_SEQ_TIMED_ENTRY_ST 0x24
#define EFR32_SEQ_TIMED_POLL_ST 0x28
#define EFR32_SEQ_CMU_CLKEN0_NS 0x50008064
#define EFR32_SEQ_CMU_CLKEN0_TIMER1 (1 << 5)
#define EFR32_SEQ_TIMER1_CNT_NS 0x5004c024
#define EFR32_SEQ_TIMED_OP_NONE 0
#define EFR32_SEQ_TIMED_OP_TXEN 1
#define EFR32_SEQ_TIMED_OP_SWRXEN 2
#define EFR32_SEQ_TIMED_OP_MASK 0xff
#define EFR32_SEQ_TIMED_RX_AFTER_TX 0x100
#define EFR32_SEQ_TIMED_GENERATION_SHIFT 16
#define EFR32_SEQ_TIMED_LATE 0x200
#define EFR32_SEQ_TIMED_CANCELLED 0x400

/*
 * RDMAILBOX1: the M33 -> M0+ mailbox. Register layout from the stock radio stack's use of it
 * (docs/radio-tables.md SS7.78 item 7): FLAGS at +0x40 (one flag per message pointer) with the
 * usual SET/CLR aliases, IEN at +0x44; the M0+ takes IRQ 23 for FLAGS & IEN. Bus clock
 * CMU.CLKEN1.RDMAILBOX1 (bit 9, efr32bg22_cmu.h).
 */
#define EFR32_SEQ_RDMAILBOX1_BASE 0xa802c000
#define EFR32_SEQ_RDMAILBOX_FLAGS 0x040
#define EFR32_SEQ_RDMAILBOX_IEN 0x044
#define EFR32_SEQ_RDMAILBOX_TIMED_FLAG 0x1
#define EFR32_SEQ_RDMAILBOX_IRQ 23
/* TIMER0.CNT as the M0+ reads it (non-secure window; TIMER CNT is at +0x24). */
#define EFR32_SEQ_TIMER0_CNT_NS 0x50048024

/*
 * Connection reply staged before the frame ends (docs/radio-tables.md SS7.82). Before a receive
 * in a connection event the M33 stages both replies the link layer can give to it: CUR, the PDU
 * it sends when the received frame does not acknowledge its last one (or fails its CRC), and
 * NEXT, the one it sends when it does, each a complete image (header, length, payload, MIC when
 * encrypted) in main SRAM. In RX2TX the sequencer reads the received header and the CRC result
 * from the RX FIFO and decides as lll_conn.c's isr_rx() does (Core v6.1 Vol 6 Part B 4.5.9):
 *   ack    = CRC ok and received NESN != SN       -> NEXT, else CUR
 *   accept = CRC ok and received SN == NESN and an RX buffer was free (RX_FREE)
 *   done   = (CRC failed and CRC_CLOSES) or (CRC ok, received MD clear and the reply empty)
 *   the reply's SN is SN ^ ack, its NESN NESN ^ accept (both outside the CCM AAD, which masks
 *   NESN, SN and MD: Vol 6 Part E 2.3.3);
 * a Central sends nothing when done, a Peripheral sends its reply and then nothing more. It
 * copies the reply into the BUFC TX FIFO and turns around at T_IFS without the M33; when the
 * event continues it arms the next receive (SWRXEN, decision word PENDING). The M33 commits the
 * same outcome to the link layer afterwards (OUTCOME).
 *
 * STATE: generation << 16 | EFR32_SEQ_CONN_STAGED, written last; the sequencer clears it when it
 * takes the stage. FLAGS: the bits below. RX_BASE/RX_OFFSET/RX_MASK: the RX FIFO, the offset the
 * frame will start at and the FIFO size less one. RX_MAX: the largest payload length the receive
 * is configured for (a longer one is a delivery failure, as a CRC error). CUR/NEXT: address and
 * octet count of each image. OUTCOME: written by the sequencer, the generation in the upper half.
 */
/*
 * RX->TX decision word value (SEQRAM 0x4FA4, next to IDLE 0, RESPOND 1, CANCEL 2, PENDING 3):
 * the window is armed for a response, which is staged here.
 */
#define EFR32_SEQ_RX2TX_STAGED 4
#define EFR32_SEQ_CONN_BLOCK 0x00004e40
#define EFR32_SEQ_CONN_STATE 0x00
#define EFR32_SEQ_CONN_FLAGS 0x04
#define EFR32_SEQ_CONN_RX_BASE 0x08
#define EFR32_SEQ_CONN_RX_OFFSET 0x0c
#define EFR32_SEQ_CONN_RX_MASK 0x10
#define EFR32_SEQ_CONN_RX_MAX 0x14
#define EFR32_SEQ_CONN_CUR 0x18
#define EFR32_SEQ_CONN_CUR_OCTETS 0x1c
#define EFR32_SEQ_CONN_NEXT 0x20
#define EFR32_SEQ_CONN_NEXT_OCTETS 0x24
#define EFR32_SEQ_CONN_OUTCOME 0x28
/* RAC.STIMER (counting since RX2TX began) when the sequencer took the stage (diagnostic). */
#define EFR32_SEQ_CONN_TAKEN_AT 0x2c
#define EFR32_SEQ_CONN_STAGED 0x1
#define EFR32_SEQ_CONN_GENERATION_SHIFT 16
/* FLAGS: SN and NESN are the link layer's current ones (bit 0 of lll->sn, lll->nesn). */
#define EFR32_SEQ_CONN_CENTRAL 0x01
#define EFR32_SEQ_CONN_RX_FREE 0x02
#define EFR32_SEQ_CONN_CRC_CLOSES 0x04
#define EFR32_SEQ_CONN_SN_SHIFT 3
#define EFR32_SEQ_CONN_SN (1 << EFR32_SEQ_CONN_SN_SHIFT)
#define EFR32_SEQ_CONN_NESN_SHIFT 4
#define EFR32_SEQ_CONN_NESN (1 << EFR32_SEQ_CONN_NESN_SHIFT)
/*
 * ADV: an advertiser's receive after its ADV_IND/ADV_SCAN_IND. The sequencer answers, as
 * lll_adv.c's isr_rx_pdu() does without a scan-request filter or privacy, a SCAN_REQ (type 3,
 * 12 octets, CRC ok) whose RxAdd equals ADV_TXADD and whose AdvA equals ADVA (6 octets) with
 * CUR (the SCAN_RSP, sent unchanged), and nothing else; the event then ends (DONE).
 */
#define EFR32_SEQ_CONN_ADV 0x20
#define EFR32_SEQ_CONN_ADV_TXADD 0x40
#define EFR32_SEQ_CONN_ADVA 0x30
#define EFR32_SEQ_ADV_SCAN_REQ_TYPE 0x3
#define EFR32_SEQ_ADV_TYPE_MASK 0xf
#define EFR32_SEQ_ADV_RXADD_BIT 7
#define EFR32_SEQ_ADV_SCAN_REQ_LEN 12
#define EFR32_SEQ_ADV_SCAN_REQ_ADVA 6
#define EFR32_SEQ_ADV_ADDR_LEN 6
/*
 * SCAN: a scanner's or initiator's receive (docs/radio-tables.md 7.87). Answered, as
 * lll_scan.c's isr_rx_pdu() does without privacy, only with SCAN_REQ or INIT set, and only when
 * the frame's CRC is ok, it carries an AdvA (Core v6.1 Vol 6 Part B 2.3.1) and an RX buffer was
 * free (RX_FREE):
 * - SCAN_REQ (active scanner): an ADV_IND or ADV_SCAN_IND (AdvA and 0-31 octets of AdvData) gets
 *   CUR, the SCAN_REQ, with RxAdd set to the frame's TxAdd and AdvA copied from the frame; the
 *   scanner then listens for the SCAN_RSP (the next receive is armed, STAGED).
 * - INIT (initiator): an ADV_IND from AdvA ADVA (TxAdd ADV_TXADD), or an ADV_DIRECT_IND from it
 *   whose TargetA is INITA with RxAdd INIT_RXADD (or, with INIT_TGTA_RPA, any resolvable private
 *   TargetA with RxAdd 1), gets CUR, the CONNECT_IND, unchanged; the event then ends (DONE).
 *   With INIT_TIMED it is answered only when the frame ends, counted on TIMER0 from START and
 *   modulo PERIOD when PERIOD is not 0, less than LATEST counts after START: lll_scan.c's
 *   CONNECT_IND-fits-the-scan-slot check. The frame's end is TIMER0 at the decision less
 *   RAC.STIMER (since RX2TX began) times T0_PER_STIMER_Q8 / 256, less EOF_TO_EXIT counts.
 */
#define EFR32_SEQ_CONN_SCAN 0x80
#define EFR32_SEQ_CONN_SCAN_REQ 0x100
#define EFR32_SEQ_CONN_INIT 0x200
#define EFR32_SEQ_CONN_INIT_RXADD 0x400
#define EFR32_SEQ_CONN_INIT_TGTA_RPA 0x800
#define EFR32_SEQ_CONN_INIT_TIMED 0x1000
#define EFR32_SEQ_CONN_INITA 0x38
#define EFR32_SEQ_SCAN_START 0x40
#define EFR32_SEQ_SCAN_LATEST 0x44
#define EFR32_SEQ_SCAN_PERIOD 0x48
#define EFR32_SEQ_SCAN_T0_PER_STIMER_Q8 0x4c
#define EFR32_SEQ_SCAN_EOF_TO_EXIT 0x50
#define EFR32_SEQ_SCAN_Q8_SHIFT 8
/* Advertising channel PDU header (Vol 6 Part B 2.3): PDU type 3:0, TxAdd 6, RxAdd 7. */
#define EFR32_SEQ_ADV_IND_TYPE 0x0
#define EFR32_SEQ_ADV_DIRECT_IND_TYPE 0x1
#define EFR32_SEQ_ADV_SCAN_IND_TYPE 0x6
#define EFR32_SEQ_ADV_TXADD_BIT 6
/* ADV_IND, ADV_SCAN_IND: AdvA and 0-31 octets of AdvData (2.3.1.1, 2.3.1.4). */
#define EFR32_SEQ_ADV_IND_LEN_MAX 37
/* ADV_DIRECT_IND: AdvA, TargetA (2.3.1.2). */
#define EFR32_SEQ_ADV_DIRECT_IND_LEN 12
#define EFR32_SEQ_ADV_DIRECT_TARGETA 6
/* A resolvable private address: its two most significant bits are 0b01 (Vol 6 Part B 1.3.2.2). */
#define EFR32_SEQ_ADDR_TYPE_OCTET 5
#define EFR32_SEQ_ADDR_TYPE_MASK 0xc0
#define EFR32_SEQ_ADDR_TYPE_RPA 0x40
/* SCAN_REQ: header, length, ScanA, AdvA (2.3.2.1): AdvA at PDU octet 8. */
#define EFR32_SEQ_SCAN_REQ_ADVA_OCTET 8
/* OUTCOME */
#define EFR32_SEQ_CONN_OUT_VALID 0x01
#define EFR32_SEQ_CONN_OUT_CRC_OK 0x02
#define EFR32_SEQ_CONN_OUT_ACK_SHIFT 2
#define EFR32_SEQ_CONN_OUT_ACK (1 << EFR32_SEQ_CONN_OUT_ACK_SHIFT)
#define EFR32_SEQ_CONN_OUT_ACCEPT_SHIFT 3
#define EFR32_SEQ_CONN_OUT_ACCEPT (1 << EFR32_SEQ_CONN_OUT_ACCEPT_SHIFT)
#define EFR32_SEQ_CONN_OUT_DONE 0x10
#define EFR32_SEQ_CONN_OUT_REPLY 0x20
/* Data channel PDU header, octet 0 (Vol 6 Part B 2.4): LLID 1:0, NESN 2, SN 3, MD 4. */
#define EFR32_SEQ_PDU_NESN_BIT 2
#define EFR32_SEQ_PDU_SN_BIT 3
#define EFR32_SEQ_PDU_MD_BIT 4
/* The appended FRC trailer octet; bit 7 is the CRC result (SS7.66). */
#define EFR32_SEQ_FRC_TRAILER_CRCOK 0x80
/*
 * FRC.WCNTCMP0 (radio window 0xA8004000 + 0x20) and the length-pending word count the M33's
 * radio_pkt_configure() sets before every frame (EFR32_FRC_DYNAMIC_LENGTH_PENDING_WORD_COUNT).
 */
#define EFR32_SEQ_FRC_WCNTCMP0 0xa8004020
#define EFR32_SEQ_FRC_WCNTCMP0_LENGTH_PENDING 0xff
/* BUFC TX channel as the M0+ reads it (BUFC 0xAA000000; channel 0 at +0x0C). */
#define EFR32_SEQ_BUFC_TX_CMD 0xaa000034
#define EFR32_SEQ_BUFC_TX_WRITEDATA 0xaa000024
#define EFR32_SEQ_BUFC_CMD_CLEAR 0x1

#endif /* EFR32_RADIO_SEQ_ABI_H_ */
