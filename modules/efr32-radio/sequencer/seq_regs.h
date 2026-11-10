/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Register addresses, fields and SEQRAM words of the open radio sequencer, shared by its C
 * (radio_seq.c) and its timed assembly (radio_seq_core.S): macros only, so both can include it.
 */
#ifndef EFR32_RADIO_SEQ_REGS_H_
#define EFR32_RADIO_SEQ_REGS_H_

#include "hal/efr32/seq_abi.h"
#define EFR32_SEQ_STACK_TOP                   0x00003e00
#define EFR32_SEQ_IDENTITY_WORD               0x00004fe0
#define EFR32_SEQ_IDENTITY_OPEN_V1            0x4f535131 /* "OSQ1" */
#define EFR32_SEQ_TRACE_TXWARM                0x00004fc0
#define EFR32_SEQ_TRACE_TX                    0x00004fd0
/* Diagnostic only: the remaining loop-counter value when each of the
 * TXWARM handler's two bounded wait loops exits (docs/radio-tables.md
 * SS7.15) -- 0 means the loop was still waiting when it hit its bound
 * (never acked/cleared in time), nonzero means it exited early with that
 * much headroom left. Lets a post-hoc debug-probe read distinguish "timed
 * out" from "succeeded quickly" without needing to catch the M0+ core
 * itself mid-loop. */
#define EFR32_SEQ_TRACE_IRQ_COUNTS            0x00004f80 /* 24 bytes, IRQ0..23 */
#define EFR32_SEQ_TRACE_RXFRAME_TXEN          0x00004f98 /* diagnostic only */
#define EFR32_SEQ_TRACE_RXFRAME_STATUS        0x00004f9c /* diagnostic only */
#define EFR32_RAC_STATUS                      0xa802000c
#define EFR32_RAC_RXENSRCEN_SWRXEN_BIT        (1 << 1)
#define EFR32_SEQ_SYNTH_LOCK_CAL              0 /* calibrate (TX->RX retune) */
#define EFR32_SEQ_SYNTH_LOCK_CAL_AND_CACHE    1 /* calibrate, keep as TX tuning */
#define EFR32_SEQ_SYNTH_LOCK_CACHED           2 /* reuse this event's TX tuning */
#define EFR32_SEQ_TX_VCOTUNING_CACHE          0x00004fa0
#define EFR32_SEQ_TRACE_WAIT_BIT_REMAINING    0x00004fa8
#define EFR32_SEQ_TRACE_SEQIF_ACK_REMAINING   0x00004fac
/* Diagnostic only (not part of the bounded production trace words above):
 * FRC.STATUS before this handler acts, and again just before it returns, so
 * a later debug-probe attach can see whether this image's own mechanics
 * changed it (docs/radio-tables.md SS7.4). */
#define EFR32_SEQ_TRACE_STATUS_BEFORE         0x00004fb0
#define EFR32_SEQ_TRACE_STATUS_AFTER          0x00004fb4
/* Diagnostic only: the exact live inputs fn_0x894's own `flags`
 * accumulator reads (docs/radio-tables.md SS7.3.1), captured at the same
 * point its real reads happen rather than reconstructed from a
 * post-hoc, decayed read -- the whole reason a 100ms-later debug-halt
 * read could not answer this. Not part of the bounded production trace. */
#define EFR32_SEQ_TRACE_LOCAL_3F64            0x00004fe4 /* clear of 0x4fb0..0x4fbf
                                                   * (status/distance) and
                                                   * 0x4fc0..0x4fdf (the
                                                   * production trace
                                                   * words); just past the
                                                   * single-word identity
                                                   * at 0x4fe0. */
#define EFR32_SEQ_TRACE_LOCAL_3F64_P8         0x00004fe8
#define EFR32_SEQ_TRACE_AA00011C              0x00004fec
#define EFR32_SEQ_TRACE_GATE_VAL              0x00004ff0
#define EFR32_SEQ_LOCAL_3F64                  0x00003f64
#define EFR32_OBS_BLOCK_AA00011C              0xaa00011c
#define EFR32_FRC_STATUS                      0xa8004008
#define EFR32_RAC_SEQIF                       0xa8020028
#define EFR32_RAC_SEQIF_CLEAR                 0xa8022028
#define EFR32_RAC_SEQIEN_SET                  0xa802102c
#define EFR32_RAC_SEQIF_STATE_EVENTS          0x3fff0000
#define EFR32_FRC_IF                          0xa8004068
#define EFR32_FRC_SEQIF                       0xa80040b4
#define EFR32_BUFC_TX_CHANNEL_BASE            0xaa00000c
#define EFR32_BUFC_CHANNEL_WRITE_OFFSET       0x00000008
#define EFR32_BUFC_CHANNEL_READ_OFFSET        0x0000000c
#define EFR32_BUFC_TX_WRITE_OFFSET            (EFR32_BUFC_TX_CHANNEL_BASE + EFR32_BUFC_CHANNEL_WRITE_OFFSET)
#define EFR32_BUFC_TX_READ_OFFSET             (EFR32_BUFC_TX_CHANNEL_BASE + EFR32_BUFC_CHANNEL_READ_OFFSET)
#define EFR32_NVIC_ISER0                      0xe000e100
/* State event n (SEQIF bit n+16) arrives on M0+ IRQn (SS7.62/SS7.63): RXWARM 1,
 * RXSEARCH 2, RXFRAME 3, RXPD 4, RX2RX 5, RX2TX 7, TXWARM 8, TX 9, TXPD 10,
 * TX2RX 11, SHUTDOWN 13.  RAC holds a transition until every state event
 * enabled in SEQIEN is acknowledged (measured, SS7.69): with SEQIEN.SHUTDOWN
 * and SEQIEN.RX2RX latched but their IRQs disabled, RAC parked in SHUTDOWN and
 * RX2RX.  So every enabled event gets its IRQ, as stock enables IRQ0-15
 * (SS7.63); those without a handler are acknowledged by the common tail. */
#define EFR32_RAC_SEQIF_STATE_SHIFT           16
#define EFR32_RAC_IRQ_STATE_EVENTS            (EFR32_RAC_SEQIF_STATE_EVENTS >> EFR32_RAC_SEQIF_STATE_SHIFT)
#define EFR32_EXCEPTION_RXWARM                (16 + 1)
#define EFR32_EXCEPTION_RXPD                  (16 + 4)
#define EFR32_EXCEPTION_RX2TX                 (16 + 7)
#define EFR32_EXCEPTION_RXFRAME               (16 + 3)
#define EFR32_EXCEPTION_TX2RX                 (16 + 11)
#define EFR32_EXCEPTION_TXWARM                (16 + 8)
#define EFR32_EXCEPTION_TX                    (16 + 9)
#define EFR32_EXCEPTION_TXPD                  (16 + 10)
#define EFR32_EXCEPTION_TIMED                 (16 + EFR32_SEQ_RDMAILBOX_IRQ)
/*
 * The following are the stock IRQ8/TXWARM handler's bounded, mechanical
 * prologue: a RAC gate pulse, one observed-but-unexplained register write,
 * and a confirmed clear-and-wait on one state-event bit. Resolved by static
 * decomp of generic_efr32xg22_release_seq.o against a live AP3 capture of
 * the running SEQRAM (docs/radio-tables.md SS7.2). The stock handler's
 * intervening job-dispatch logic (SS7.3) is deliberately not reproduced here;
 * see this file's header comment.
 *
 * SR0/SR1 below are RAC+0x40/+0x44's *official* Silicon Labs names
 * (build/reference-models/RAC_S_register.py, fetched directly from
 * github.com/SiliconLabs/simplicity_sdk) -- and that model documents both
 * as generic 32-bit read-write scratch words, reset value 0, with no
 * hardware meaning assigned at all. So the "gate"/"wait" bits named below
 * are not a documented hardware feature; they are stock's *own firmware
 * convention*, using otherwise-blank scratch space as a software
 * semaphore (this project replicates the exact bit pattern stock uses,
 * confirmed byte-for-byte against the original, without claiming any
 * hardware authority for what the bits mean).
 */
#define EFR32_RAC_SR0_BASE                    0xa8020040
#define EFR32_RAC_SR0_SET                     0xa8021040
#define EFR32_RAC_SR0_CLR                     0xa8022040
#define EFR32_RAC_SR0_GATE_BIT                (1 << 18)
#define EFR32_RAC_SR0_WAIT_BIT                (1 << 2)
/* RAC.PRESC's SET alias (official name; RAC+0x3C, SET alias at +0x1000).
 * Semantics of writing value 5 to it here are unestablished -- PRESC is a
 * documented prescaler-control register, but this project has not traced
 * which of its fields bits 0/2 (value 5) actually correspond to. */
#define EFR32_RAC_PRESC_SET                   0xa802113c
#define EFR32_RAC_PRESC_SET_VALUE             5
/* fn_0x894's own unconditional entry/exit bracket (docs/radio-tables.md
 * SS7.3/SS7.6): set on entry, cleared at exit, around the whole call --
 * not part of the job-dispatch logic this project deliberately does not
 * port, just the one bounded hardware gate fn_0x894 itself brackets its
 * entire body with, regardless of IRQ number or RAC state. SR1, like SR0
 * above, is officially blank scratch space, not a documented gate. */
#define EFR32_RAC_SR1_BASE                    0xa8020044
#define EFR32_RAC_SR1_SET                     0xa8021044
#define EFR32_RAC_SR1_BIT_FUNCTION_GATE       (1 << 16)
/*
 * fn_0xd38's own second, previously-undocumented pulse of the TXWARM
 * gate's SET alias (docs/radio-tables.md SS7.3.1), reached from
 * fn_0x894's unconditional tail call for ordinary TXWARM/TX servicing.
 * Only the branch stock *always* takes this pulse on is reproduced: when
 * `(1 << (((PROTIMER+0x38 bits[31:29]) + 6))) - (FRC+0x1C & 0x1fff)`,
 * truncated to 16 bits, is below 8, fn_0xd38 unconditionally clobbers its
 * own gating flags word to a value that always passes the mask test
 * guarding this pulse -- so this specific condition's outcome does not
 * depend on the broader job-dispatch state this project does not
 * reproduce. The other branch (distance >= 8) depends on that state and
 * is NOT reproduced; this is a deliberately conservative subset, not the
 * full original condition.
 */
#define EFR32_SEQ_TRACE_DISTANCE              0x00004fb8 /* diagnostic only */
/* CORRECTION: a full re-verification of fn_0xd38.c's second half (see
 * sequencer-decomp/src/fn_0xd38.c's header comment and docs/radio-tables.md
 * SS7.13) found both operands below were wrong. Re-deriving the literal
 * pool directly: both reads are off the *same* OBS_BLOCK_AA000054 base
 * (BUFC.BUF[1].XWRITE, SS7.3) the surrounding function keeps in a register
 * for its whole second half -- `ldr r2,[r6,#56]`/`ldr r0,[r6,#88]`, not
 * pc-relative loads of PROTIMER_0X38/FRC_0X1C at all. The bit-extraction
 * for the first term was also wrong: the real instructions (`lsls r2,r2,#29;
 * lsrs r2,r2,#29`) isolate bits [2:0], not bits [31:29] (a left-shift that
 * drops everything above bit 2 before the right-shift ever runs). */
#define EFR32_OBS_BLOCK_AA000054_0X38         0xaa00008c
#define EFR32_OBS_BLOCK_AA000054_0X58         0xaa0000ac
/* Bit-width masks for the distance computation below: a 3-bit field
 * (bits[2:0] of EFR32_OBS_BLOCK_AA000054_0X38), a 13-bit field (bits
 * [12:0] of EFR32_OBS_BLOCK_AA000054_0X58), and the final 16-bit
 * truncation -- generic masks, but named rather than left as bare
 * literals in the arithmetic itself. */
#define EFR32_SEQ_DISTANCE_TERM_A_MASK        0x7
#define EFR32_SEQ_DISTANCE_TERM_B_MASK        0x1fff
#define EFR32_SEQ_DISTANCE_RESULT_MASK        0xffff
/* Pulses SR0's SET alias again (not SR1, despite this constant's name
 * before this file's official-naming pass -- the operand at this call
 * site is EFR32_RAC_SR0_SET, confirmed by re-reading the code below, not
 * just renaming blindly). */
#define EFR32_RAC_SR0_SECOND_PULSE_BIT        (1 << 22)
#define EFR32_OBS_BLOCK_A8015020              0xa8015020
#define EFR32_OBS_BLOCK_A8015020_BIT          (1 << 21)
#define EFR32_OBS_BLOCK_A8016100              0xa8016100
/* Three observed CLR-alias writes; semantics unestablished beyond
 * RXENSRCEN's own official name (RAC+0x08, already used elsewhere in
 * this project, e.g. docs/radio-tables.md SS1) -- which bit(s) of it
 * value 0x10 actually clears is not traced. */
#define EFR32_RAC_SR1_CLR                     0xa8022044
#define EFR32_RAC_SR1_CLR_MASK                0x03
#define EFR32_RAC_SR0_CLR_MASK2               0xa8
#define EFR32_RAC_RXENSRCEN_CLR               0xa8022008
#define EFR32_RAC_RXENSRCEN_CLR_MASK          0x10
/* The specific state-event bit the stock handler clears and confirms
 * cleared (by re-reading RAC.SEQIF) before returning. Within the already
 * broader EFR32_RAC_SEQIF_STATE_EVENTS mask cleared unconditionally below. */
#define EFR32_RAC_SEQIF_TXWARM_ACK_MASK       (1 << 24)
/* The stock handler's own wait is unbounded; this project has not verified
 * the wait condition holds in the open-image context, so both waits below
 * are bounded instead of risking a permanent M0+ lockup on real silicon. */
#define EFR32_RAC_TXWARM_WAIT_BOUND           0x00010000
/*
 * Stock's own reset handler (docs/radio-tables.md SS7.9) sets NVIC
 * priority 1 for a block of IRQs including 8 and 9 (byte-addressable
 * NVIC.IPR, architected Armv6-M, not a vendor register -- IPR_BYTE[n] is
 * IRQn's own priority, at NVIC.IPR_BASE+n) before enabling them. Ported
 * only for the two IRQs this core actually enables below; stock's other
 * ~17 entries configure IRQs/exceptions this core never unmasks via
 * NVIC.ISER0, so their priority is moot for this bounded implementation
 * and is not reproduced.
 */
#define EFR32_NVIC_IPR_BASE                   0xe000e400
#define EFR32_NVIC_IPR_IRQ8                   (EFR32_NVIC_IPR_BASE + 8)
#define EFR32_NVIC_IPR_IRQ9                   (EFR32_NVIC_IPR_BASE + 9)
#define EFR32_NVIC_IPR_IRQ10                  (EFR32_NVIC_IPR_BASE + 10)
#define EFR32_NVIC_PRIORITY_TXWARM_TX         1
/*
 * Stock's own reset handler also writes RAC.STCTRL and sets up this
 * core's private SysTick immediately after the priority setup above,
 * gated behind a busy-wait on RAC.SR3 bit 17 (docs/radio-tables.md
 * SS7.9). That exact gate is deliberately NOT reproduced: RAC.SR3 reads
 * 0 throughout this core's own boot on the open stack (SS7.5.2), so a
 * faithful copy of stock's wait would hang this core forever, the same
 * bounded-instead-of-unbounded precedent as EFR32_RAC_TXWARM_WAIT_BOUND
 * above. The two writes themselves are unconditional otherwise.
 */
#define EFR32_RAC_STCTRL                      0xa8020050
/* RAC.STCTRL's own two documented fields (RAC_S_field.py: STCAL
 * bits[23:0], STSKEW bit 24); STCAL's numeric content is an opaque
 * empirical value this project has no theory for (same status as
 * PATRIM/IFADCTRIM trim words elsewhere in this block) -- but it is the
 * exact value independently confirmed by a live hardware read of this
 * register on stock (docs/radio-tables.md SS7.8), decomposed by field
 * rather than left as one opaque hex blob. */
#define EFR32_RAC_STCTRL_STCAL                0x00061a7f
#define EFR32_RAC_STCTRL_STSKEW               (1 << 24)
#define EFR32_RAC_STCTRL_VALUE                (EFR32_RAC_STCTRL_STCAL | EFR32_RAC_STCTRL_STSKEW)
/* Standard Armv6-M/Armv8-M SysTick (architected, not vendor-specific):
 * CSR at the base, RELOAD at +4, CALIB at +0xC. This core's own private
 * instance, not the M33's. */
#define EFR32_SYSTICK_CSR                     0xe000e010
#define EFR32_SYSTICK_RELOAD                  0xe000e014
#define EFR32_SYSTICK_CALIB                   0xe000e01c
#define EFR32_SYSTICK_CALIB_TENMS_MASK        0x00ffffff
/* CALIB.TENMS's reload count for a 10ms period, scaled to stock's own
 * deliberate choice of a 250ms SysTick period -- not an architected
 * constant; see docs/radio-tables.md SS7.9. */
#define EFR32_SYSTICK_PERIOD_MULTIPLIER       25
#define EFR32_SYSTICK_CSR_ENABLE_AND_CLKSOURCE 5 /* ENABLE(0)|CLKSOURCE(2):
                                                 * processor clock, no
                                                 * interrupt (TICKINT
                                                 * clear). */
/*
 * TX synthesizer power-up and VCO calibration (docs/radio-tables.md SS7.60,
 * SS7.61).  Without this the RAC reaches TX with every synthesizer analog
 * block off and the VCO never started, and FRC stalls after parsing the
 * header (ACTIVETXFCD never sets).  The order, values and waits below are
 * the stock TXWARM handler's own, recorded store-by-store on hardware with
 * tests/hardware-lockstep/trace_stores.py and decoded against the public
 * RAC_S/SYNTH_S field models; every value is expressed in those fields.
 */
#define EFR32_RAC_BASE                        0xa8020000
#define EFR32_SYNTH_BASE                      0xa8018000
#define EFR32_ALIAS_SET                       0x1000
#define EFR32_ALIAS_CLR                       0x2000
#define EFR32_RAC_CTRL                        (EFR32_RAC_BASE + 0x014)
#define EFR32_RAC_CTRL_FORCEDISABLE           (1 << 0)
#define EFR32_RAC_SYNTHENCTRL                 (EFR32_RAC_BASE + 0x094)
#define EFR32_RAC_SYNTHENCTRL_VCOSTARTUP      (1 << 1)
#define EFR32_RAC_SYNTHENCTRL_SYNTHSTARTREQ   (1 << 9)
#define EFR32_RAC_SYNTHENCTRL_LPFBWSEL        (1 << 20)
#define EFR32_RAC_SYNTHREGCTRL                (EFR32_RAC_BASE + 0x098)
#define EFR32_RAC_SYNTHREGCTRL_CHPLDOVREFTRIM_SHIFT 24
#define EFR32_RAC_SYNTHREGCTRL_CHPLDOVREFTRIM_MASK (7 << EFR32_RAC_SYNTHREGCTRL_CHPLDOVREFTRIM_SHIFT)
#define EFR32_RAC_SYNTHREGCTRL_CHPLDOVREFTRIM_TX (2 << EFR32_RAC_SYNTHREGCTRL_CHPLDOVREFTRIM_SHIFT)
#define EFR32_RAC_VCOCTRL                     (EFR32_RAC_BASE + 0x09c)
#define EFR32_RAC_VCOCTRL_TX                  ((0xc << 0) | (4 << 4)) /* VCOAMPLITUDE=12, VCODETAMPLITUDE=4 */
#define EFR32_RAC_CHPCTRL                     (EFR32_RAC_BASE + 0x0a0)
#define EFR32_RAC_CHPCTRL_CHPOUTPUTTRISTATEMODE (1 << 14)
#define EFR32_RAC_PATRIM1                     (EFR32_RAC_BASE + 0x114)
#define EFR32_RAC_PATRIM1_TX0DBMTRIMTAPCAP_MASK (7 << 16)
#define EFR32_RAC_RADIOEN                     (EFR32_RAC_BASE + 0x13c)
#define EFR32_RAC_RADIOEN_PREEN               (1 << 0)
#define EFR32_RAC_RADIOEN_PRESTB100UDIS       (1 << 1)
#define EFR32_RAC_RADIOEN_RFBIASEN            (1 << 2)
#define EFR32_RAC_RX                          (EFR32_RAC_BASE + 0x144)
#define EFR32_RAC_RX_SYCHPBIASTRIMBUF         (1 << 7)
#define EFR32_RAC_SYTRIM0                     (EFR32_RAC_BASE + 0x150)
/* SYCHPCURR=5, SYCHPLEVPSRC=7, SYCHPSRCEN=1, SYCHPREPLICACURRADJ=1,
 * SYTRIMCHPREGAMPBIAS=3. */
#define EFR32_RAC_SYTRIM0_TX                  ((5 << 3) | (7 << 9) | (1 << 13) | (1 << 14) | (3 << 17))
#define EFR32_RAC_SYTRIM1                     (EFR32_RAC_BASE + 0x154)
/* SYMMDREPLICA1CURRADJ=7, SYMMDREPLICA2CURRADJ=7, SYTRIMMMDREGAMPBIAS=3;
 * SYLODIVLDOTRIMNDIO is 3 while starting and 1 once the synthesizer runs. */
#define EFR32_RAC_SYTRIM1_COMMON              ((7 << 6) | (7 << 9) | (3 << 12))
#define EFR32_RAC_SYTRIM1_STARTUP             (EFR32_RAC_SYTRIM1_COMMON | (3 << 2))
#define EFR32_RAC_SYTRIM1_RUNNING             (EFR32_RAC_SYTRIM1_COMMON | (1 << 2))
#define EFR32_RAC_SYCAL                       (EFR32_RAC_BASE + 0x158)
#define EFR32_RAC_SYCAL_SYVCOMORECURRENT      (1 << 9)
#define EFR32_RAC_SYCAL_SYVCOSLOWNOISEFILTER  (1 << 10)
#define EFR32_RAC_SYEN                        (EFR32_RAC_BASE + 0x15c)
#define EFR32_RAC_SYEN_SYCHPEN                (1 << 0)
#define EFR32_RAC_SYEN_SYCHPLPEN              (1 << 1)
#define EFR32_RAC_SYEN_SYENCHPREG             (1 << 2)
#define EFR32_RAC_SYEN_SYENCHPREPLICA         (1 << 3)
#define EFR32_RAC_SYEN_SYENMMDREG             (1 << 4)
#define EFR32_RAC_SYEN_SYENMMDREPLICA1        (1 << 5)
#define EFR32_RAC_SYEN_SYENVCOBIAS            (1 << 7)
#define EFR32_RAC_SYEN_SYENVCOPFET            (1 << 8)
#define EFR32_RAC_SYEN_SYENVCOREG             (1 << 9)
#define EFR32_RAC_SYEN_SYLODIVEN              (1 << 10)
#define EFR32_RAC_SYEN_SYLODIVLDOBIASEN       (1 << 11)
#define EFR32_RAC_SYEN_SYLODIVLDOEN           (1 << 12)
#define EFR32_RAC_SYEN_STAGE1                 (EFR32_RAC_SYEN_SYCHPEN | EFR32_RAC_SYEN_SYENCHPREG | \
    EFR32_RAC_SYEN_SYENCHPREPLICA | EFR32_RAC_SYEN_SYENMMDREG | \
    EFR32_RAC_SYEN_SYENMMDREPLICA1 | EFR32_RAC_SYEN_SYENVCOBIAS | \
    EFR32_RAC_SYEN_SYLODIVLDOBIASEN)
#define EFR32_RAC_SYEN_STAGE3                 (EFR32_RAC_SYEN_SYENVCOREG | EFR32_RAC_SYEN_SYLODIVLDOEN)
#define EFR32_RAC_SYMMDCTRL                   (EFR32_RAC_BASE + 0x168)
#define EFR32_RAC_SYMMDCTRL_SYMMDENRSDIG      (1 << 0)
#define EFR32_RAC_SYMMDCTRL_TX                (2 << 9) /* SYMMDMODE=2 */
#define EFR32_RAC_DIGCLKRETIMECTRL            (EFR32_RAC_BASE + 0x16c)
#define EFR32_RAC_XORETIMECTRL                (EFR32_RAC_BASE + 0x174)
#define EFR32_RAC_RETIME_ENRETIME             (1 << 0) /* same position in both RETIMECTRLs */
#define EFR32_RAC_RETIME_RESETN               (1 << 2)
#define EFR32_SYNTH_VCDACCTRL                 (EFR32_SYNTH_BASE + 0x02c)
#define EFR32_SYNTH_VCDACCTRL_VCDACEN         (1 << 6)
#define EFR32_SYNTH_VCDACCTRL_LPFEN           (1 << 7)
#define EFR32_SYNTH_VCDACCTRL_LPFQSEN         (1 << 8)
#define EFR32_SYNTH_CMD                       (EFR32_SYNTH_BASE + 0x00c)
#define EFR32_SYNTH_CMD_SYNTHSTART            (1 << 0)
#define EFR32_SYNTH_CMD_SYNTHSTOP             (1 << 1)
#define EFR32_SYNTH_CMD_DISABLEIF             (1 << 3)
#define EFR32_SYNTH_CMD_CAPCALSTART           (1 << 4)
#define EFR32_SYNTH_VCOTUNING                 (EFR32_SYNTH_BASE + 0x04c)
#define EFR32_SYNTH_VCOTUNING_VCAPSEL_TX      (0x10 << 11)
#define EFR32_SYNTH_VCOTUNING_TOP_BIT         10 /* VCOTUNING is 11 bits wide */
#define EFR32_SYNTH_VCORANGE                  (EFR32_SYNTH_BASE + 0x054)
#define EFR32_SYNTH_VCORANGE_TX               ((3 << 3) | (5 << 6)) /* HIGHTHRESH=3, LOWTHRESH=5 */
#define EFR32_SYNTH_VCORANGE_LOAD             (1 << 11)
#define EFR32_SYNTH_VCORANGE_THROUGH          (1 << 18)
#define EFR32_SYNTH_LOCNTCTRL                 (EFR32_SYNTH_BASE + 0x088)
#define EFR32_SYNTH_LOCNTCTRL_ENABLE          (1 << 0)
#define EFR32_SYNTH_LOCNTCTRL_CLEAR           (1 << 1)
#define EFR32_SYNTH_LOCNTCTRL_RUN             (1 << 2)
#define EFR32_SYNTH_LOCNTCTRL_READ            (1 << 3)
#define EFR32_SYNTH_LOCNTCTRL_NUMCYCLE_SHIFT  4
#define EFR32_SYNTH_LOCNTCTRL_NUMCYCLE_MASK   (0xf << EFR32_SYNTH_LOCNTCTRL_NUMCYCLE_SHIFT)
#define EFR32_SYNTH_LOCNTSTATUS               (EFR32_SYNTH_BASE + 0x08c)
#define EFR32_SYNTH_LOCNTSTATUS_LOCOUNT_MASK  0x0007ffff
#define EFR32_SYNTH_LOCNTSTATUS_BUSY          (1 << 19)
#define EFR32_SYNTH_LOCNTTARGET               (EFR32_SYNTH_BASE + 0x090)
/* Stock bounds each LO-count measurement to this many BUSY polls before
 * abandoning the calibration (and then does not start the synthesizer). */
#define EFR32_SYNTH_LOCNT_POLL_BOUND          2047
#define EFR32_SEQ_TRACE_SYNTH_TUNING          0x00004fbc /* diagnostic only */
#define EFR32_SEQ_SYNTH_CAL_ABORTED           0xffffffff
/*
 * Stock's microsecond-scale busy wait: count = n * 195 - 111, then subtract
 * 30 per pass while count >= 0, with the count held in memory each pass.
 * Reproduced with the same memory round trip so the per-pass cost, and so
 * the delay, stays comparable rather than becoming several times shorter.
 */
#define EFR32_SEQ_DELAY_PER_UNIT              195
#define EFR32_SEQ_DELAY_BIAS                  111
#define EFR32_SEQ_DELAY_STEP                  30
/* Powers the synthesizer down in stock's TXPD order (SS7.62).  Clobbers
 * r0-r3. */
#define EFR32_RAC_SYEN_REMAINING              (EFR32_RAC_SYEN_SYCHPEN | EFR32_RAC_SYEN_SYENCHPREG | \
    EFR32_RAC_SYEN_SYENCHPREPLICA | EFR32_RAC_SYEN_SYENMMDREG | \
    EFR32_RAC_SYEN_SYENMMDREPLICA1 | (1 << 6) /* SYENMMDREPLICA2 */ | \
    EFR32_RAC_SYEN_SYENVCOBIAS | EFR32_RAC_SYEN_SYLODIVLDOBIASEN | \
    (1 << 13) /* SYSTARTCHPREG */ | (1 << 14) /* SYSTARTMMDREG */)
/*
 * 6 dBm PA bring-up and ramp-down (SS7.62), in the stock TXWARM/TXPD
 * handlers' recorded order and waits.  Stock's ramp-down waits on
 * MODEM.SEQIF.TXRAMPDONE without a bound; bounded here like the other
 * waits in this file.
 */
#define EFR32_MODEM_BASE                      0xa8014000
#define EFR32_MODEM_RAMPCTRL                  (EFR32_MODEM_BASE + 0x0c4)
#define EFR32_MODEM_RAMPCTRL_RAMPDIS          (1 << 23)
#define EFR32_MODEM_SEQIF                     (EFR32_MODEM_BASE + 0x24c)
#define EFR32_MODEM_SEQIF_TXRAMPDONE          (1 << 3)
#define EFR32_RAC_CMD                         (EFR32_RAC_BASE + 0x010)
#define EFR32_RAC_CMD_PAENSET                 (1 << 12)
#define EFR32_RAC_CMD_PAENCLEAR               (1 << 13)
#define EFR32_RAC_PAENCTRL                    (EFR32_RAC_BASE + 0x0b4)
#define EFR32_RAC_PAENCTRL_PARAMP             (1 << 8)
#define EFR32_RAC_PAENCTRL_INVRAMPCLK         (1 << 16)
#define EFR32_RAC_APC                         (EFR32_RAC_BASE + 0x0b8)
#define EFR32_RAC_APC_ENAPCSW                 (1 << 2)
#define EFR32_RAC_APC_AMPCONTROLLIMITSW_MAX   (0xff << 24)
#define EFR32_RAC_LNAMIXEN                    (EFR32_RAC_BASE + 0x108)
#define EFR32_RAC_LNAMIXEN_LNAMIXENLDO        (1 << 0)
#define EFR32_RAC_PATRIM3                     (EFR32_RAC_BASE + 0x11c)
#define EFR32_RAC_PATRIM3_TX6DBMTRIMREGBLEEDAUTO (1 << 11)
#define EFR32_RAC_PATRIM3_TX6DBMTRIMREGPSR    (1 << 14)
#define EFR32_RAC_PACTRL                      (EFR32_RAC_BASE + 0x120)
#define EFR32_RAC_PACTRL_TX6DBMSLICERESET     (1 << 24)
#define EFR32_RAC_RFPATHEN                    (EFR32_RAC_BASE + 0x140)
#define EFR32_RAC_RFPATHEN_LNAMIXRFATTDCEN    (1 << 2)
#define EFR32_RAC_RFPATHEN_LNAMIXTRSW         (1 << 4)
#define EFR32_RAC_RX_LNAMIXLDOLOWCUR          (1 << 4)
#define EFR32_RAC_RX_LNAMIXREGLOADEN          (1 << 5)
#define EFR32_RAC_TX                          (EFR32_RAC_BASE + 0x148)
#define EFR32_RAC_TX_TX0DBMENBLEEDPREDRVREG   (1 << 0)
#define EFR32_RAC_TX_TX0DBMENBLEEDREG         (1 << 1)
#define EFR32_RAC_TX_TX0DBMENPREDRV           (1 << 2)
#define EFR32_RAC_TX_TX0DBMENPREDRVREG        (1 << 3)
#define EFR32_RAC_TX_TX0DBMENPREDRVREGBIAS    (1 << 4)
#define EFR32_RAC_TX_TX0DBMENBIAS             (1 << 5)
#define EFR32_RAC_TX_TX0DBMENREG              (1 << 7)
#define EFR32_RAC_TX_TX6DBMENBLEEDPREDRVREG   (1 << 16)
#define EFR32_RAC_TX_TX6DBMENBLEEDREG         (1 << 17)
#define EFR32_RAC_TX_TX6DBMENPREDRVREG        (1 << 19)
#define EFR32_RAC_TX_TX6DBMENRAMPCLK          (1 << 22)
#define EFR32_RAC_TX_TX6DBMENREG              (1 << 23)
#define EFR32_RAC_TX_TX6DBMENPACORE           (1 << 24)
#define EFR32_RAC_TX_TX6DBMENPAOUT            (1 << 25)
#define EFR32_RAC_TX_ENPAPOWER                (1 << 30)
#define EFR32_RAC_TX_ENPASELSLICE             (1 << 31)
#define EFR32_RAC_TX_BLEED                    (EFR32_RAC_TX_TX0DBMENBLEEDPREDRVREG | EFR32_RAC_TX_TX0DBMENBLEEDREG | \
    EFR32_RAC_TX_TX6DBMENBLEEDPREDRVREG | EFR32_RAC_TX_TX6DBMENBLEEDREG)
#define EFR32_RAC_SYLOEN                      (EFR32_RAC_BASE + 0x160)
#define EFR32_RAC_SYLOEN_SYLODIVTLO6DBM2G4EN  (1 << 10)
#define EFR32_RAC_AGCOVERWRITE                (EFR32_RAC_BASE + 0x188)
/* LNA/mixer held at manual maximum attenuation, one slice, while the PA is
 * on: ENMANLNAMIXRFATT, ENMANLNAMIXSLICE, MANLNAMIXRFATT=63,
 * MANLNAMIXSLICE=1. */
#define EFR32_RAC_AGCOVERWRITE_TX             ((1 << 0) | (1 << 1) | (0x3f << 4) | (1 << 10))
#define EFR32_SEQ_RAMP_WAIT_BOUND             0x00010000
/*
 * TX->RX turnaround, in stock IRQ11's recorded order (SS7.63): PA down, keep
 * the RX request asserted, retune the running synthesizer for RX, power the
 * receive chain, then calibrate the IF PGA's DC offset.
 */
#define EFR32_RAC_RXENSRCEN                   (EFR32_RAC_BASE + 0x008)
#define EFR32_RAC_RXENSRCEN_SWRXEN            (1 << 1)
#define EFR32_SYNTH_CMD_ENABLEIF              (1 << 2)
#define EFR32_SYNTH_VCORANGE_RX               ((1 << 3) | (3 << 6)) /* HIGHTHRESH=1, LOWTHRESH=3 */
#define EFR32_RAC_SYMMDCTRL_RX                (4 << 9) /* SYMMDMODE=4 */
#define EFR32_RAC_RX_IFADCCAPRESET            (1 << 0)
#define EFR32_RAC_RX_IFADCENLDOSERIES         (1 << 1)
#define EFR32_RAC_RX_IFADCENLDOSHUNT          (1 << 2)
#define EFR32_RAC_RX_PGAENLDO                 (1 << 6)
#define EFR32_RAC_RX_SYPFDCHPLPEN             (1 << 9)
#define EFR32_RAC_RX_SYPFDFPWEN               (1 << 10)
/* Stock's RX value 0x64E29: the TX trims with SYCHPBIAS=1 and without
 * SYCHPSRCEN (SS7.64 corrects SS7.63's port, which kept SYCHPSRCEN). */
#define EFR32_RAC_SYTRIM0_SYCHPSRCEN          (1 << 13)
#define EFR32_RAC_SYTRIM0_RX                  ((EFR32_RAC_SYTRIM0_TX & ~EFR32_RAC_SYTRIM0_SYCHPSRCEN) | (1 << 0))
#define EFR32_RAC_SYLOEN_RX                   ((1 << 0) | (1 << 1)) /* SYLODIVRLOADCCLK2G4EN, SYLODIVRLO2G4EN */
#define EFR32_RAC_SYTRIM1_SYLODIVRLOADCCLKSEL (1 << 17)
#define EFR32_RAC_CMD_LNAENSET                (1 << 14)
#define EFR32_RAC_IFADCDBG                    (EFR32_RAC_BASE + 0x0e0)
/* IFADCENADC, IFADCENREFBUF, IFADCENSIDETONE, IFADCINVERTCLK; then IFADCENCLK. */
#define EFR32_RAC_IFADCDBG_RX_POWER           ((1 << 0) | (1 << 2) | (1 << 3) | (1 << 4))
#define EFR32_RAC_IFADCDBG_IFADCENCLK         (1 << 1)
#define EFR32_RAC_RFPATHEN_LNAMIXEN           (1 << 1)
#define EFR32_RAC_PGACTRL                     (EFR32_RAC_BASE + 0x130)
/* PGAENBIAS, PGAENGHZ, PGAENPGAI, PGAENPGAQ, PGAENPKD, PGAENRCMOUT, PGAENSUBG. */
#define EFR32_RAC_PGACTRL_RX_ENABLES          ((1 << 2) | (1 << 3) | (1 << 9) | (1 << 10) | (1 << 11) | (1 << 12) | (1 << 13))
#define EFR32_SEQ_RX_LDO_SETTLE_US            7
/*
 * IF PGA DC-offset calibration (stock fn_0x21e8, SS7.63): with the LNA
 * isolated, measure MODEM.DCESTI at two opposite PGAOFF settings, interpolate
 * each of I and Q to the zero-offset P code, and set each N code to its
 * 6-bit two's complement.
 */
#define EFR32_AGC_CTRL0                       0xa800c020
#define EFR32_AGC_CTRL0_ADCATTENCODE_MASK     (3 << 25)
/* ENMANLNAMIXRFATT, ENMANLNAMIXSLICE, ENMANPGAGAIN, MANLNAMIXRFATT=63,
 * MANLNAMIXSLICE=5 -- and the mask stock clears afterwards (all manual
 * enables and the three manual value fields). */
#define EFR32_RAC_AGCOVERWRITE_DCCAL          ((1 << 0) | (1 << 1) | (1 << 2) | (0x3f << 4) | (5 << 10))
#define EFR32_RAC_AGCOVERWRITE_DCCAL_RELEASE  ((1 << 0) | (1 << 1) | (1 << 2) | (0x3f << 4) | (0x3f << 10) | (0xf << 20))
#define EFR32_RAC_IFPGACTRL                   (EFR32_RAC_BASE + 0x0b0)
#define EFR32_RAC_IFPGACTRL_DCCALON           (1 << 19)
#define EFR32_RAC_IFPGACTRL_DCRSTEN           (1 << 20)
#define EFR32_RAC_IFPGACTRL_DCESTIEN          (1 << 21)
#define EFR32_RAC_IFPGACTRL_DCCALDEC0_MASK    (7 << 22)
#define EFR32_RAC_IFPGACTRL_DCCALDCGEAR_MASK  (7 << 25)
#define EFR32_RAC_PGACAL                      (EFR32_RAC_BASE + 0x12c)
/* Fields PGAOFFNCALI[5:0], NCALQ[13:8], PCALI[21:16], PCALQ[29:24]. */
#define EFR32_RAC_PGACAL_POINT_A              0x30301010 /* P=48, N=16 */
#define EFR32_RAC_PGACAL_POINT_B              0x10103030 /* P=16, N=48 */
#define EFR32_RAC_PGACAL_MIDSCALE             0x20202020
#define EFR32_RAC_PGACAL_CODE_MASK            0x3f
#define EFR32_RAC_PGACAL_N_FIELDS             0x3f3f
#define EFR32_RAC_PGACAL_N_NEGATE_ONE         0x0101 /* +1 in each N field */
#define EFR32_MODEM_DCCOMP                    (EFR32_MODEM_BASE + 0x0e0)
#define EFR32_MODEM_DCCOMP_DCCOMPFREEZE       (1 << 3)
#define EFR32_MODEM_DCESTI                    (EFR32_MODEM_BASE + 0x0e8)
#define EFR32_MODEM_DCESTI_Q_SHIFT            15
#define EFR32_SEQ_DCCAL_ISOLATE_US            16
#define EFR32_SEQ_DCCAL_SETTLE_US             8
#define EFR32_SEQ_DCCAL_FREEZE_US             1
/*
 * Stock fn_0x2308, lifted: r0 = DC estimate at point B, r1 = at point A, each
 * a signed 15-bit value in bits [14:0].  Returns the 6-bit P code at which
 * the linear interpolation between the two crosses zero.
 */
#define EFR32_SEQ_DCEST_SEXT_UP               17 /* bit 14 to bit 31 */
#define EFR32_SEQ_DCEST_SEXT_DOWN             3 /* keep 14 fractional bits */
#define EFR32_SEQ_DCCAL_TOP_CODE_BIT          32
/*
 * Stock's RF front-end power-down (fn_0xc02, SS7.65), offsets now named from
 * the public RAC_S model.  Stock first clears the retime enables when SR0
 * bit 0 is clear; this image's other paths already clear them.
 */
#define EFR32_RAC_RX_FRONTEND_OFF             (EFR32_RAC_RX_IFADCENLDOSERIES | EFR32_RAC_RX_IFADCENLDOSHUNT | \
    (1 << 3) /* LNAMIXENRFPKD */ | EFR32_RAC_RX_LNAMIXLDOLOWCUR | EFR32_RAC_RX_PGAENLDO | \
    EFR32_RAC_RX_SYPFDCHPLPEN | EFR32_RAC_RX_SYPFDFPWEN)
/* PGAENBIAS, PGAENGHZ, PGAENPGAI, PGAENPGAQ, PGAENPKD, PGAENRCMOUT. */
#define EFR32_RAC_PGACTRL_FRONTEND_OFF        ((1 << 2) | (1 << 3) | (1 << 9) | (1 << 10) | (1 << 11) | (1 << 12))
#define EFR32_RAC_IFADCDBG_FRONTEND_OFF       (EFR32_RAC_IFADCDBG_IFADCENCLK | (1 << 0)) /* + IFADCENADC */
#define EFR32_RAC_CMD_LNAENCLEAR              (1 << 15)
/* Open ABI word: the M33 writes PENDING when it arms RAC.CMD.TXAFTERFRAME,
 * then RESPOND after publishing the response to BUFC or CANCEL when the LLL
 * declines (SS7.65). */
#define EFR32_SEQ_RX2TX_DECISION              0x00004fa4
#define EFR32_SEQ_RX2TX_PENDING               3 /* 0 = idle: SEQRAM reset value */
#define EFR32_SEQ_RX2TX_RESPOND               1
#define EFR32_SEQ_RX2TX_CANCEL                2
/* Bound in ~1 us steps; the response must start within the 150 us tIFS. */
#define EFR32_SEQ_RX2TX_DECISION_WAIT_US      120
#define EFR32_RAC_CMD_TXDIS                   (1 << 5)
#define EFR32_RAC_CMD_TXEN_BIT                (1 << 0)
/* Diagnostic only: bump byte r0 of the per-IRQ count area (slots 24-26 are
 * the RX2TX decision outcomes).  Clobbers r0-r1. */
#define EFR32_SEQ_TRACE_RX2TX_RESPOND_SLOT    25
#define EFR32_SEQ_TRACE_RX2TX_CANCEL_SLOT     26
#define EFR32_SEQ_TRACE_RX2TX_TIMEOUT_SLOT    27
/* Diagnostic only: RAC.STIMER into word r0 of the RX2TX timing record
 * (entry, decision, locked, PA up).  Clobbers r0-r1. */
#define EFR32_SEQ_TRACE_RX2TX_TIMES           0x00004f40
/* The RX2TX hold (EFR32_SEQ_RX2TX_TX_AT_STIMER) and the RAC.STIMER rate are in
 * include/hal/efr32/seq_abi.h, shared with the M33 HAL, which derives its
 * RESPOND deadline from them.  The hold is matched to the stock stack on
 * silicon (SS7.71); the chain delays from MODEM EOF and SYNCSENT to the
 * antenna are the same silicon in both, so matching stock's interval
 * matches its on-air T_IFS. */
#define EFR32_SEQ_TRACE_RESPONSE_PENDING      0x00004f34 /* diagnostic only */
#define EFR32_SEQ_TRACE_RESPONSE_TX_AT        0x00004f38 /* diagnostic only */
#define EFR32_RAC_STIMER                      0xa8020030
#define EFR32_SYSTICK_CVR                     0xe000e018
/*
 * Timed command (seq_abi.h, docs/radio-tables.md SS7.82).  The M33 has done
 * every preparation (FIFO, packet stage, SR0 handshake) and posted one command
 * with its instant as a TIMER0 count; this core waits for the instant and
 * issues it, so no M33 interrupt can delay it.  The wait polls TIMER0.CNT
 * (signed difference, wrap-safe) and the OP word, which the M33 clears to
 * cancel; past LIMIT nothing is issued.  Called from the IRQ 23 entry.
 */
#define EFR32_SEQ_RAC_RXENSRCEN_SET           (EFR32_RAC_RXENSRCEN + EFR32_ALIAS_SET)
#define EFR32_SEQ_CONN_PDU_SEQ_BITS           ((1 << EFR32_SEQ_PDU_SN_BIT) | (1 << EFR32_SEQ_PDU_NESN_BIT))
#define EFR32_SEQ_CONN_PDU_MD                 (1 << EFR32_SEQ_PDU_MD_BIT)

#endif /* EFR32_RADIO_SEQ_REGS_H_ */
