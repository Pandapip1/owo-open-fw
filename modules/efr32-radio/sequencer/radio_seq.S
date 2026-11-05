/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Open firmware for the EFR32BG22 private radio Cortex-M0+.
 *
 * This core implements only bounded peripheral mechanics.  It does not choose
 * jobs, priorities, channels, deadlines, or follow-up operations; those remain
 * owned by Zephyr's M33 scheduler.  The current first milestone establishes a
 * source-built, identifiable image and a conservative interrupt boundary.
 * Additional handlers are added only after their register-level contracts are
 * explained in docs/radio-tables.md and covered by simulator tests.
 */

#include "hal/efr32/seq_abi.h"

.syntax unified
.cpu cortex-m0plus
.thumb

.equ EFR32_SEQ_STACK_TOP,             0x00003e00
.equ EFR32_SEQ_IDENTITY_WORD,         0x00004fe0
.equ EFR32_SEQ_IDENTITY_OPEN_V1,      0x4f535131 /* "OSQ1" */
.equ EFR32_SEQ_TRACE_TXWARM,          0x00004fc0
.equ EFR32_SEQ_TRACE_TX,              0x00004fd0
/* Diagnostic only: the remaining loop-counter value when each of the
 * TXWARM handler's two bounded wait loops exits (docs/radio-tables.md
 * SS7.15) -- 0 means the loop was still waiting when it hit its bound
 * (never acked/cleared in time), nonzero means it exited early with that
 * much headroom left. Lets a post-hoc debug-probe read distinguish "timed
 * out" from "succeeded quickly" without needing to catch the M0+ core
 * itself mid-loop. */
.equ EFR32_SEQ_TRACE_IRQ_COUNTS,     0x00004f80 /* 24 bytes, IRQ0..23 */
.equ EFR32_SEQ_TRACE_RXFRAME_TXEN,    0x00004f98 /* diagnostic only */
.equ EFR32_SEQ_TRACE_RXFRAME_STATUS,  0x00004f9c /* diagnostic only */
.equ EFR32_RAC_STATUS,                0xa802000c
.equ EFR32_RAC_RXENSRCEN_SWRXEN_BIT, (1 << 1)
.equ EFR32_SEQ_SYNTH_LOCK_CAL,          0 /* calibrate (TX->RX retune) */
.equ EFR32_SEQ_SYNTH_LOCK_CAL_AND_CACHE, 1 /* calibrate, keep as TX tuning */
.equ EFR32_SEQ_SYNTH_LOCK_CACHED,       2 /* reuse this event's TX tuning */
.equ EFR32_SEQ_TX_VCOTUNING_CACHE,      0x00004fa0
.equ EFR32_SEQ_TRACE_WAIT_BIT_REMAINING,  0x00004fa8
.equ EFR32_SEQ_TRACE_SEQIF_ACK_REMAINING, 0x00004fac
/* Diagnostic only (not part of the bounded production trace words above):
 * FRC.STATUS before this handler acts, and again just before it returns, so
 * a later debug-probe attach can see whether this image's own mechanics
 * changed it (docs/radio-tables.md SS7.4). */
.equ EFR32_SEQ_TRACE_STATUS_BEFORE,   0x00004fb0
.equ EFR32_SEQ_TRACE_STATUS_AFTER,    0x00004fb4
/* Diagnostic only: the exact live inputs fn_0x894's own `flags`
 * accumulator reads (docs/radio-tables.md SS7.3.1), captured at the same
 * point its real reads happen rather than reconstructed from a
 * post-hoc, decayed read -- the whole reason a 100ms-later debug-halt
 * read could not answer this. Not part of the bounded production trace. */
.equ EFR32_SEQ_TRACE_LOCAL_3F64,      0x00004fe4 /* clear of 0x4fb0..0x4fbf
                                                   * (status/distance) and
                                                   * 0x4fc0..0x4fdf (the
                                                   * production trace
                                                   * words); just past the
                                                   * single-word identity
                                                   * at 0x4fe0. */
.equ EFR32_SEQ_TRACE_LOCAL_3F64_P8,   0x00004fe8
.equ EFR32_SEQ_TRACE_AA00011C,        0x00004fec
.equ EFR32_SEQ_TRACE_GATE_VAL,        0x00004ff0
.equ EFR32_SEQ_LOCAL_3F64,            0x00003f64
.equ EFR32_OBS_BLOCK_AA00011C,        0xaa00011c
.equ EFR32_FRC_STATUS,                0xa8004008
.equ EFR32_RAC_SEQIF,                 0xa8020028
.equ EFR32_RAC_SEQIF_CLEAR,           0xa8022028
.equ EFR32_RAC_SEQIEN_SET,            0xa802102c
.equ EFR32_RAC_SEQIF_STATE_EVENTS,    0x3fff0000
.equ EFR32_FRC_IF,                    0xa8004068
.equ EFR32_FRC_SEQIF,                 0xa80040b4
.equ EFR32_BUFC_TX_CHANNEL_BASE,      0xaa00000c
.equ EFR32_BUFC_CHANNEL_WRITE_OFFSET, 0x00000008
.equ EFR32_BUFC_CHANNEL_READ_OFFSET,  0x0000000c
.equ EFR32_BUFC_TX_WRITE_OFFSET,      (EFR32_BUFC_TX_CHANNEL_BASE + EFR32_BUFC_CHANNEL_WRITE_OFFSET)
.equ EFR32_BUFC_TX_READ_OFFSET,       (EFR32_BUFC_TX_CHANNEL_BASE + EFR32_BUFC_CHANNEL_READ_OFFSET)
.equ EFR32_NVIC_ISER0,                0xe000e100
/* State event n (SEQIF bit n+16) arrives on M0+ IRQn (SS7.62/SS7.63): RXWARM 1,
 * RXSEARCH 2, RXFRAME 3, RXPD 4, RX2RX 5, RX2TX 7, TXWARM 8, TX 9, TXPD 10,
 * TX2RX 11, SHUTDOWN 13.  RAC holds a transition until every state event
 * enabled in SEQIEN is acknowledged (measured, SS7.69): with SEQIEN.SHUTDOWN
 * and SEQIEN.RX2RX latched but their IRQs disabled, RAC parked in SHUTDOWN and
 * RX2RX.  So every enabled event gets its IRQ, as stock enables IRQ0-15
 * (SS7.63); those without a handler are acknowledged by the common tail. */
.equ EFR32_RAC_SEQIF_STATE_SHIFT,     16
.equ EFR32_RAC_IRQ_STATE_EVENTS,      (EFR32_RAC_SEQIF_STATE_EVENTS >> EFR32_RAC_SEQIF_STATE_SHIFT)
.equ EFR32_EXCEPTION_RXWARM,          (16 + 1)
.equ EFR32_EXCEPTION_RXPD,            (16 + 4)
.equ EFR32_EXCEPTION_RX2TX,           (16 + 7)
.equ EFR32_EXCEPTION_RXFRAME,         (16 + 3)
.equ EFR32_EXCEPTION_TX2RX,           (16 + 11)
.equ EFR32_EXCEPTION_TXWARM,          (16 + 8)
.equ EFR32_EXCEPTION_TX,              (16 + 9)
.equ EFR32_EXCEPTION_TXPD,            (16 + 10)
.equ EFR32_EXCEPTION_TIMED,           (16 + EFR32_SEQ_RDMAILBOX_IRQ)

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
.equ EFR32_RAC_SR0_BASE,            0xa8020040
.equ EFR32_RAC_SR0_SET,             0xa8021040
.equ EFR32_RAC_SR0_CLR,             0xa8022040
.equ EFR32_RAC_SR0_GATE_BIT,        (1 << 18)
.equ EFR32_RAC_SR0_WAIT_BIT,        (1 << 2)
/* RAC.PRESC's SET alias (official name; RAC+0x3C, SET alias at +0x1000).
 * Semantics of writing value 5 to it here are unestablished -- PRESC is a
 * documented prescaler-control register, but this project has not traced
 * which of its fields bits 0/2 (value 5) actually correspond to. */
.equ EFR32_RAC_PRESC_SET,              0xa802113c
.equ EFR32_RAC_PRESC_SET_VALUE,        5
/* fn_0x894's own unconditional entry/exit bracket (docs/radio-tables.md
 * SS7.3/SS7.6): set on entry, cleared at exit, around the whole call --
 * not part of the job-dispatch logic this project deliberately does not
 * port, just the one bounded hardware gate fn_0x894 itself brackets its
 * entire body with, regardless of IRQ number or RAC state. SR1, like SR0
 * above, is officially blank scratch space, not a documented gate. */
.equ EFR32_RAC_SR1_BASE,              0xa8020044
.equ EFR32_RAC_SR1_SET,               0xa8021044
.equ EFR32_RAC_SR1_BIT_FUNCTION_GATE, (1 << 16)
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
.equ EFR32_SEQ_TRACE_DISTANCE,         0x00004fb8 /* diagnostic only */
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
.equ EFR32_OBS_BLOCK_AA000054_0X38,   0xaa00008c
.equ EFR32_OBS_BLOCK_AA000054_0X58,   0xaa0000ac
/* Bit-width masks for the distance computation below: a 3-bit field
 * (bits[2:0] of EFR32_OBS_BLOCK_AA000054_0X38), a 13-bit field (bits
 * [12:0] of EFR32_OBS_BLOCK_AA000054_0X58), and the final 16-bit
 * truncation -- generic masks, but named rather than left as bare
 * literals in the arithmetic itself. */
.equ EFR32_SEQ_DISTANCE_TERM_A_MASK,  0x7
.equ EFR32_SEQ_DISTANCE_TERM_B_MASK,  0x1fff
.equ EFR32_SEQ_DISTANCE_RESULT_MASK,  0xffff
/* Pulses SR0's SET alias again (not SR1, despite this constant's name
 * before this file's official-naming pass -- the operand at this call
 * site is EFR32_RAC_SR0_SET, confirmed by re-reading the code below, not
 * just renaming blindly). */
.equ EFR32_RAC_SR0_SECOND_PULSE_BIT,   (1 << 22)
.equ EFR32_OBS_BLOCK_A8015020,         0xa8015020
.equ EFR32_OBS_BLOCK_A8015020_BIT,     (1 << 21)
.equ EFR32_OBS_BLOCK_A8016100,         0xa8016100
/* Three observed CLR-alias writes; semantics unestablished beyond
 * RXENSRCEN's own official name (RAC+0x08, already used elsewhere in
 * this project, e.g. docs/radio-tables.md SS1) -- which bit(s) of it
 * value 0x10 actually clears is not traced. */
.equ EFR32_RAC_SR1_CLR,                0xa8022044
.equ EFR32_RAC_SR1_CLR_MASK,           0x03
.equ EFR32_RAC_SR0_CLR_MASK2,          0xa8
.equ EFR32_RAC_RXENSRCEN_CLR,          0xa8022008
.equ EFR32_RAC_RXENSRCEN_CLR_MASK,     0x10
/* The specific state-event bit the stock handler clears and confirms
 * cleared (by re-reading RAC.SEQIF) before returning. Within the already
 * broader EFR32_RAC_SEQIF_STATE_EVENTS mask cleared unconditionally below. */
.equ EFR32_RAC_SEQIF_TXWARM_ACK_MASK, (1 << 24)
/* The stock handler's own wait is unbounded; this project has not verified
 * the wait condition holds in the open-image context, so both waits below
 * are bounded instead of risking a permanent M0+ lockup on real silicon. */
.equ EFR32_RAC_TXWARM_WAIT_BOUND,     0x00010000

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
.equ EFR32_NVIC_IPR_BASE,             0xe000e400
.equ EFR32_NVIC_IPR_IRQ8,             (EFR32_NVIC_IPR_BASE + 8)
.equ EFR32_NVIC_IPR_IRQ9,             (EFR32_NVIC_IPR_BASE + 9)
.equ EFR32_NVIC_IPR_IRQ10,            (EFR32_NVIC_IPR_BASE + 10)
.equ EFR32_NVIC_PRIORITY_TXWARM_TX,   1

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
.equ EFR32_RAC_STCTRL,                0xa8020050
/* RAC.STCTRL's own two documented fields (RAC_S_field.py: STCAL
 * bits[23:0], STSKEW bit 24); STCAL's numeric content is an opaque
 * empirical value this project has no theory for (same status as
 * PATRIM/IFADCTRIM trim words elsewhere in this block) -- but it is the
 * exact value independently confirmed by a live hardware read of this
 * register on stock (docs/radio-tables.md SS7.8), decomposed by field
 * rather than left as one opaque hex blob. */
.equ EFR32_RAC_STCTRL_STCAL,          0x00061a7f
.equ EFR32_RAC_STCTRL_STSKEW,         (1 << 24)
.equ EFR32_RAC_STCTRL_VALUE,          (EFR32_RAC_STCTRL_STCAL | EFR32_RAC_STCTRL_STSKEW)

/* Standard Armv6-M/Armv8-M SysTick (architected, not vendor-specific):
 * CSR at the base, RELOAD at +4, CALIB at +0xC. This core's own private
 * instance, not the M33's. */
.equ EFR32_SYSTICK_CSR,               0xe000e010
.equ EFR32_SYSTICK_RELOAD,            0xe000e014
.equ EFR32_SYSTICK_CALIB,             0xe000e01c
.equ EFR32_SYSTICK_CALIB_TENMS_MASK,  0x00ffffff
/* CALIB.TENMS's reload count for a 10ms period, scaled to stock's own
 * deliberate choice of a 250ms SysTick period -- not an architected
 * constant; see docs/radio-tables.md SS7.9. */
.equ EFR32_SYSTICK_PERIOD_MULTIPLIER, 25
.equ EFR32_SYSTICK_CSR_ENABLE_AND_CLKSOURCE, 5 /* ENABLE(0)|CLKSOURCE(2):
                                                 * processor clock, no
                                                 * interrupt (TICKINT
                                                 * clear). */

.section .vectors, "a", %progbits
.word EFR32_SEQ_STACK_TOP
.word efr32_radio_seq_reset + 1
.rept 14
.word efr32_radio_seq_exception + 1
.endr
.rept 48
.word efr32_radio_seq_exception + 1
.endr

.section .text, "ax", %progbits
.global efr32_radio_seq_reset
.thumb_func
efr32_radio_seq_reset:
    cpsid i

    /* Publish image identity before accepting any peripheral interrupt. */
    ldr r0, =EFR32_SEQ_IDENTITY_WORD
    ldr r1, =EFR32_SEQ_IDENTITY_OPEN_V1
    str r1, [r0]

    /* Discard retained state-transition indications, then expose only that
     * bounded class to this core.  No operation is scheduled here. */
    ldr r0, =EFR32_RAC_SEQIF_CLEAR
    ldr r1, =EFR32_RAC_SEQIF_STATE_EVENTS
    str r1, [r0]
    ldr r0, =EFR32_RAC_SEQIEN_SET
    str r1, [r0]

    /* Stock sets NVIC priority 1 for IRQ8/IRQ9 before enabling them
     * (docs/radio-tables.md SS7.9); reproduced here, for only these two
     * IRQs, via single-byte stores so neighboring IRQs' priority bytes
     * in the same 32-bit NVIC.IPR word are left untouched. */
    ldr r0, =EFR32_NVIC_IPR_IRQ8
    movs r1, #EFR32_NVIC_PRIORITY_TXWARM_TX
    strb r1, [r0]
    ldr r0, =EFR32_NVIC_IPR_IRQ9
    strb r1, [r0]
    ldr r0, =EFR32_NVIC_IPR_IRQ10
    strb r1, [r0]

    /* Stock's own RAC.STCTRL write and private-SysTick setup
     * (docs/radio-tables.md SS7.9); the SR3-bit-17 busy-wait stock gates
     * this behind is deliberately not reproduced -- see this file's own
     * comment above EFR32_RAC_STCTRL. */
    ldr r0, =EFR32_RAC_STCTRL
    ldr r1, =EFR32_RAC_STCTRL_VALUE
    str r1, [r0]

    ldr r0, =EFR32_SYSTICK_CALIB
    ldr r1, [r0]
    ldr r2, =EFR32_SYSTICK_CALIB_TENMS_MASK
    ands r1, r2
    ldr r2, =EFR32_SYSTICK_PERIOD_MULTIPLIER
    muls r1, r2
    ldr r0, =EFR32_SYSTICK_RELOAD
    str r1, [r0]
    ldr r0, =EFR32_SYSTICK_CSR
    movs r1, #EFR32_SYSTICK_CSR_ENABLE_AND_CLKSOURCE
    str r1, [r0]

    /* Timed commands arrive on RDMAILBOX1 (seq_abi.h, SS7.82): no message is
     * pending at reset, then its one flag raises IRQ 23. */
    ldr r0, =EFR32_SEQ_TIMED_BLOCK
    movs r1, #EFR32_SEQ_TIMED_OP_NONE
    str r1, [r0, #EFR32_SEQ_TIMED_OP]
    ldr r0, =(EFR32_SEQ_RDMAILBOX1_BASE + EFR32_SEQ_RDMAILBOX_FLAGS + EFR32_ALIAS_CLR)
    movs r1, #EFR32_SEQ_RDMAILBOX_TIMED_FLAG
    str r1, [r0]
    ldr r0, =(EFR32_SEQ_RDMAILBOX1_BASE + EFR32_SEQ_RDMAILBOX_IEN)
    str r1, [r0]

    /* Enable the IRQ of every state event enabled in SEQIEN above, so each is
     * acknowledged, and the timed-command mailbox.  The M33 scheduler remains
     * the sole source of radio-job policy: it chooses each command and its
     * instant; this core only issues it then. */
    ldr r0, =EFR32_NVIC_ISER0
    ldr r1, =(EFR32_RAC_IRQ_STATE_EVENTS | (1 << EFR32_SEQ_RDMAILBOX_IRQ))
    str r1, [r0]

    cpsie i
1:  wfi
    b 1b

.global efr32_radio_seq_exception
.ltorg

.thumb_func
efr32_radio_seq_exception:
    /* Acknowledge only enabled state-transition events.  The M0+ does not
     * turn them into a new radio command; policy remains on the M33. */
    /* Diagnostic only: a wrapping per-IRQ entry count, one byte per external
     * IRQ, readable by a debugger from the M33 without halting this core. */
    mrs r3, ipsr
    subs r3, #16
    ldr r0, =EFR32_SEQ_TRACE_IRQ_COUNTS
    ldrb r1, [r0, r3]
    adds r1, #1
    strb r1, [r0, r3]
    mrs r3, ipsr
    cmp r3, #EFR32_EXCEPTION_TIMED
    bne 24f
    /* A timed command from the M33 (SS7.82); no state event to acknowledge. */
    mov r0, lr
    push {r0}
    bl efr32_seq_timed_command
    pop {r0}
    bx r0
24: cmp r3, #EFR32_EXCEPTION_TXWARM
    beq 1f
    cmp r3, #EFR32_EXCEPTION_TX2RX
    bne 16f
    /* TX->RX turnaround (SS7.63): retune and bring the receiver up, then
     * let the common tail acknowledge STATETX2RX. */
    mov r0, lr
    push {r0}
    bl efr32_seq_rx_turnaround
    pop {r0}
    mov lr, r0
    b 2f
16: cmp r3, #EFR32_EXCEPTION_RXWARM
    bne 17f
    /* RX warm-up from OFF (SS7.80): synthesizer and receiver up, then the
     * common tail acknowledges STATERXWARM. */
    mov r0, lr
    push {r0}
    bl efr32_seq_rx_warm
    pop {r0}
    mov lr, r0
    b 2f
17: cmp r3, #EFR32_EXCEPTION_RXPD
    bne 18f
    /* RX power-down (SS7.65): receive chain, then synthesizer. */
    mov r0, lr
    push {r0}
    bl efr32_seq_rx_frontend_stop
    bl efr32_seq_synth_power_down
    pop {r0}
    mov lr, r0
    b 2f
18: cmp r3, #EFR32_EXCEPTION_RXFRAME
    bne 20f
    /* A frame is arriving.  If the M33 armed a possible response for this
     * window, request TX now so the RAC turns around at frame end (SS7.65);
     * otherwise leave the window to end normally. */
    ldr r0, =EFR32_SEQ_RX2TX_DECISION
    ldr r0, [r0]
    cmp r0, #EFR32_SEQ_RX2TX_PENDING
    beq 21f
    cmp r0, #EFR32_SEQ_RX2TX_STAGED
    beq 21f
    b 2f
21: /* Stock's RXFRAME status carries TXENS without RXENS: with SWRXEN
     * still asserted the RAC returned to RX at frame end instead of RX2TX
     * (measured, SS7.65), so withdraw the RX request first. */
    ldr r0, =EFR32_RAC_RXENSRCEN_CLR
    movs r1, #EFR32_RAC_RXENSRCEN_SWRXEN_BIT
    str r1, [r0]
    ldr r0, =EFR32_RAC_CMD
    movs r1, #EFR32_RAC_CMD_TXEN_BIT
    str r1, [r0]
    /* Diagnostic only: count TXENs issued here, then sample RAC.STATUS
     * straight after the command. */
    ldr r0, =EFR32_SEQ_TRACE_RXFRAME_TXEN
    ldrb r1, [r0]
    adds r1, #1
    strb r1, [r0]
    ldr r1, =EFR32_RAC_STATUS
    ldr r1, [r1]
    ldr r0, =EFR32_SEQ_TRACE_RXFRAME_STATUS
    str r1, [r0]
    b 2f
20: cmp r3, #EFR32_EXCEPTION_RX2TX
    bne 19f
    /* RX->TX turnaround (SS7.65): receiver off, synthesizer back to TX,
     * PA up; the common tail acknowledges STATERX2TX. */
    mov r0, lr
    push {r0}
    bl efr32_seq_tx_turnaround
    pop {r0}
    mov lr, r0
    b 2f
19: cmp r3, #EFR32_EXCEPTION_TXPD
    bne 15f

    /* TX power-down: stop the synthesizer as stock's IRQ10 does, then let
     * the common tail acknowledge STATETXPD so the RAC can reach OFF. */
    mov r0, lr
    push {r0}
    bl efr32_seq_synth_stop
    pop {r0}
    mov lr, r0
    b 2f
15: cmp r3, #EFR32_EXCEPTION_TX
    beq 23f
    b 2f
23: /* Diagnostic only: STIMER at TX entry right after a responding RX2TX. */
    ldr r0, =EFR32_SEQ_TRACE_RESPONSE_PENDING
    ldr r1, [r0]
    cmp r1, #0
    beq 22f
    movs r1, #0
    str r1, [r0]
    ldr r1, =EFR32_SYSTICK_CVR
    ldr r1, [r1]
    ldr r0, =EFR32_SEQ_TRACE_RESPONSE_TX_AT
    str r1, [r0]
22:
    ldr r3, =EFR32_SEQ_TRACE_TX
    b 3f
1:  ldr r3, =EFR32_SEQ_TRACE_TXWARM
3:  /* Four source-named diagnostic words, captured before acknowledgement:
     * FRC.IF, FRC.SEQIF, BUFC.TX.WRITE, BUFC.TX.READ.  These observations do
     * not issue a command or choose subsequent radio work. */
    ldr r0, =EFR32_FRC_IF
    ldr r1, [r0]
    str r1, [r3, #0]
    ldr r0, =EFR32_FRC_SEQIF
    ldr r1, [r0]
    str r1, [r3, #4]
    ldr r0, =EFR32_BUFC_TX_WRITE_OFFSET
    ldr r1, [r0]
    str r1, [r3, #8]
    ldr r0, =EFR32_BUFC_TX_READ_OFFSET
    ldr r1, [r0]
    str r1, [r3, #12]

    ldr r0, =EFR32_FRC_STATUS
    ldr r1, [r0]
    ldr r0, =EFR32_SEQ_TRACE_STATUS_BEFORE
    str r1, [r0]

    /* Diagnostic only: fn_0x894's own flags-accumulator inputs, captured
     * live instead of reconstructed after the fact; see each constant's
     * own comment above. */
    ldr r0, =EFR32_SEQ_LOCAL_3F64
    ldr r1, [r0]
    ldr r0, =EFR32_SEQ_TRACE_LOCAL_3F64
    str r1, [r0]
    ldr r0, =EFR32_SEQ_LOCAL_3F64
    ldr r1, [r0, #8]
    ldr r0, =EFR32_SEQ_TRACE_LOCAL_3F64_P8
    str r1, [r0]
    ldr r0, =EFR32_OBS_BLOCK_AA00011C
    ldr r1, [r0]
    ldr r0, =EFR32_SEQ_TRACE_AA00011C
    str r1, [r0]
    ldr r0, =EFR32_RAC_SR0_BASE
    ldr r1, [r0]
    ldr r0, =EFR32_SEQ_TRACE_GATE_VAL
    str r1, [r0]

    mrs r3, ipsr
    cmp r3, #EFR32_EXCEPTION_TXWARM
    bne 2f
    /* Bounded TXWARM gate pulse (docs/radio-tables.md SS7.2). */
    ldr r0, =EFR32_RAC_SR0_SET
    ldr r1, =EFR32_RAC_SR0_GATE_BIT
    str r1, [r0]
    ldr r3, =EFR32_RAC_TXWARM_WAIT_BOUND
10: ldr r0, =EFR32_RAC_SR0_BASE
    ldr r1, [r0]
    ldr r2, =EFR32_RAC_SR0_WAIT_BIT
    ands r1, r2
    beq 12f
    subs r3, #1
    bne 10b
12: ldr r0, =EFR32_SEQ_TRACE_WAIT_BIT_REMAINING
    str r3, [r0]
    ldr r0, =EFR32_RAC_PRESC_SET
    movs r1, #EFR32_RAC_PRESC_SET_VALUE
    str r1, [r0]
    ldr r0, =EFR32_RAC_SR0_CLR
    ldr r1, =EFR32_RAC_SR0_GATE_BIT
    str r1, [r0]
    /* fn_0x894's entry/exit gate bracket; see its own comment above. The
     * dispatch logic that would run between these two writes in stock is
     * deliberately not reproduced -- only the gate itself. */
    ldr r0, =EFR32_RAC_SR1_SET
    ldr r1, =EFR32_RAC_SR1_BIT_FUNCTION_GATE
    str r1, [r0]
    /* fn_0xd38's conservative-subset second pulse; see its own comment
     * above. r3 = (1 << ((OBS_BLOCK_AA000054+0x38 bits[2:0]) + 6)) -
     * (OBS_BLOCK_AA000054+0x58 & 0x1fff), truncated to 16 bits. */
    ldr r0, =EFR32_OBS_BLOCK_AA000054_0X38
    ldr r1, [r0]
    ldr r2, =EFR32_SEQ_DISTANCE_TERM_A_MASK
    ands r1, r2
    adds r1, r1, #6
    movs r2, #1
    lsls r2, r2, r1
    ldr r0, =EFR32_OBS_BLOCK_AA000054_0X58
    ldr r1, [r0]
    ldr r3, =EFR32_SEQ_DISTANCE_TERM_B_MASK
    ands r1, r3
    subs r3, r2, r1
    ldr r2, =EFR32_SEQ_DISTANCE_RESULT_MASK
    ands r3, r2
    /* Diagnostic only: record the computed distance regardless of branch
     * taken, for a later debug-probe read. Not part of the production
     * trace words. */
    ldr r0, =EFR32_SEQ_TRACE_DISTANCE
    str r3, [r0]
    cmp r3, #8
    bcs 13f
    ldr r0, =EFR32_RAC_SR0_SET
    ldr r1, =EFR32_RAC_SR0_SECOND_PULSE_BIT
    str r1, [r0]
    ldr r0, =EFR32_OBS_BLOCK_A8015020
    ldr r1, =EFR32_OBS_BLOCK_A8015020_BIT
    str r1, [r0]
    ldr r0, =EFR32_OBS_BLOCK_A8016100
    movs r1, #1
    str r1, [r0]
13:
    ldr r0, =EFR32_RAC_SR1_CLR
    ldr r1, =EFR32_RAC_SR1_BIT_FUNCTION_GATE
    str r1, [r0]
    /* fn_0x894's own "done:" also does an unconditional *direct* (not
     * aliased) zero of the whole register, after the bit-16 clear above
     * and before returning -- not the conditional SEQIEN-clear next to it
     * in the original, which is RAC-state-dependent dispatch logic this
     * project does not reproduce, just this one unconditional write. */
    ldr r0, =EFR32_RAC_SR1_BASE
    movs r1, #0
    str r1, [r0]
    ldr r0, =EFR32_RAC_SR1_CLR
    movs r1, #EFR32_RAC_SR1_CLR_MASK
    str r1, [r0]
    ldr r0, =EFR32_RAC_SR0_CLR
    movs r1, #EFR32_RAC_SR0_CLR_MASK2
    str r1, [r0]
    ldr r0, =EFR32_RAC_RXENSRCEN_CLR
    movs r1, #EFR32_RAC_RXENSRCEN_CLR_MASK
    str r1, [r0]
    /* Matches stock's fn_0x7c8(mask) exactly (docs/radio-tables.md SS7.2):
     * the CLR write is re-issued on *every* iteration, not just once
     * before the loop -- confirmed from stock's disassembly at 0x7c8,
     * which re-executes `str r0, [CLR]` each time around before
     * re-reading SEQIF. A single write that doesn't fully stick (a
     * plausible race on real silicon) would otherwise leave this bounded
     * loop spinning uselessly to its timeout instead of self-correcting.
     *
     * Before acknowledging, power the TX synthesizer up and start it, as
     * stock's TXWARM handler does at this same point (SS7.60/SS7.61).  lr
     * holds EXC_RETURN, so keep it across the call. */
    mov r0, lr
    push {r0}
    bl efr32_seq_synth_start
    pop {r0}
    mov lr, r0
    ldr r3, =EFR32_RAC_TXWARM_WAIT_BOUND
11: ldr r0, =EFR32_RAC_SEQIF_CLEAR
    ldr r1, =EFR32_RAC_SEQIF_TXWARM_ACK_MASK
    str r1, [r0]
    ldr r0, =EFR32_RAC_SEQIF
    ldr r1, [r0]
    ldr r2, =EFR32_RAC_SEQIF_TXWARM_ACK_MASK
    ands r1, r2
    beq 14f
    subs r3, #1
    bne 11b
14: ldr r0, =EFR32_SEQ_TRACE_SEQIF_ACK_REMAINING
    str r3, [r0]
2:
    ldr r0, =EFR32_FRC_STATUS
    ldr r1, [r0]
    ldr r0, =EFR32_SEQ_TRACE_STATUS_AFTER
    str r1, [r0]

    ldr r0, =EFR32_RAC_SEQIF
    ldr r1, [r0]
    ldr r2, =EFR32_RAC_SEQIF_STATE_EVENTS
    ands r1, r2
    ldr r0, =EFR32_RAC_SEQIF_CLEAR
    str r1, [r0]
    bx lr

/*
 * TX synthesizer power-up and VCO calibration (docs/radio-tables.md SS7.60,
 * SS7.61).  Without this the RAC reaches TX with every synthesizer analog
 * block off and the VCO never started, and FRC stalls after parsing the
 * header (ACTIVETXFCD never sets).  The order, values and waits below are
 * the stock TXWARM handler's own, recorded store-by-store on hardware with
 * tests/hardware-lockstep/trace_stores.py and decoded against the public
 * RAC_S/SYNTH_S field models; every value is expressed in those fields.
 */
.equ EFR32_RAC_BASE,                  0xa8020000
.equ EFR32_SYNTH_BASE,                0xa8018000
.equ EFR32_ALIAS_SET,                 0x1000
.equ EFR32_ALIAS_CLR,                 0x2000

.equ EFR32_RAC_CTRL,                  (EFR32_RAC_BASE + 0x014)
.equ EFR32_RAC_CTRL_FORCEDISABLE,     (1 << 0)
.equ EFR32_RAC_SYNTHENCTRL,           (EFR32_RAC_BASE + 0x094)
.equ EFR32_RAC_SYNTHENCTRL_VCOSTARTUP,    (1 << 1)
.equ EFR32_RAC_SYNTHENCTRL_SYNTHSTARTREQ, (1 << 9)
.equ EFR32_RAC_SYNTHENCTRL_LPFBWSEL,      (1 << 20)
.equ EFR32_RAC_SYNTHREGCTRL,          (EFR32_RAC_BASE + 0x098)
.equ EFR32_RAC_SYNTHREGCTRL_CHPLDOVREFTRIM_SHIFT, 24
.equ EFR32_RAC_SYNTHREGCTRL_CHPLDOVREFTRIM_MASK,  (7 << EFR32_RAC_SYNTHREGCTRL_CHPLDOVREFTRIM_SHIFT)
.equ EFR32_RAC_SYNTHREGCTRL_CHPLDOVREFTRIM_TX,    (2 << EFR32_RAC_SYNTHREGCTRL_CHPLDOVREFTRIM_SHIFT)
.equ EFR32_RAC_VCOCTRL,               (EFR32_RAC_BASE + 0x09c)
.equ EFR32_RAC_VCOCTRL_TX,            ((0xc << 0) | (4 << 4)) /* VCOAMPLITUDE=12, VCODETAMPLITUDE=4 */
.equ EFR32_RAC_CHPCTRL,               (EFR32_RAC_BASE + 0x0a0)
.equ EFR32_RAC_CHPCTRL_CHPOUTPUTTRISTATEMODE, (1 << 14)
.equ EFR32_RAC_PATRIM1,               (EFR32_RAC_BASE + 0x114)
.equ EFR32_RAC_PATRIM1_TX0DBMTRIMTAPCAP_MASK, (7 << 16)
.equ EFR32_RAC_RADIOEN,               (EFR32_RAC_BASE + 0x13c)
.equ EFR32_RAC_RADIOEN_PREEN,         (1 << 0)
.equ EFR32_RAC_RADIOEN_PRESTB100UDIS, (1 << 1)
.equ EFR32_RAC_RADIOEN_RFBIASEN,      (1 << 2)
.equ EFR32_RAC_RX,                    (EFR32_RAC_BASE + 0x144)
.equ EFR32_RAC_RX_SYCHPBIASTRIMBUF,   (1 << 7)
.equ EFR32_RAC_SYTRIM0,               (EFR32_RAC_BASE + 0x150)
/* SYCHPCURR=5, SYCHPLEVPSRC=7, SYCHPSRCEN=1, SYCHPREPLICACURRADJ=1,
 * SYTRIMCHPREGAMPBIAS=3. */
.equ EFR32_RAC_SYTRIM0_TX,            ((5 << 3) | (7 << 9) | (1 << 13) | (1 << 14) | (3 << 17))
.equ EFR32_RAC_SYTRIM1,               (EFR32_RAC_BASE + 0x154)
/* SYMMDREPLICA1CURRADJ=7, SYMMDREPLICA2CURRADJ=7, SYTRIMMMDREGAMPBIAS=3;
 * SYLODIVLDOTRIMNDIO is 3 while starting and 1 once the synthesizer runs. */
.equ EFR32_RAC_SYTRIM1_COMMON,        ((7 << 6) | (7 << 9) | (3 << 12))
.equ EFR32_RAC_SYTRIM1_STARTUP,       (EFR32_RAC_SYTRIM1_COMMON | (3 << 2))
.equ EFR32_RAC_SYTRIM1_RUNNING,       (EFR32_RAC_SYTRIM1_COMMON | (1 << 2))
.equ EFR32_RAC_SYCAL,                 (EFR32_RAC_BASE + 0x158)
.equ EFR32_RAC_SYCAL_SYVCOMORECURRENT,     (1 << 9)
.equ EFR32_RAC_SYCAL_SYVCOSLOWNOISEFILTER, (1 << 10)
.equ EFR32_RAC_SYEN,                  (EFR32_RAC_BASE + 0x15c)
.equ EFR32_RAC_SYEN_SYCHPEN,          (1 << 0)
.equ EFR32_RAC_SYEN_SYCHPLPEN,        (1 << 1)
.equ EFR32_RAC_SYEN_SYENCHPREG,       (1 << 2)
.equ EFR32_RAC_SYEN_SYENCHPREPLICA,   (1 << 3)
.equ EFR32_RAC_SYEN_SYENMMDREG,       (1 << 4)
.equ EFR32_RAC_SYEN_SYENMMDREPLICA1,  (1 << 5)
.equ EFR32_RAC_SYEN_SYENVCOBIAS,      (1 << 7)
.equ EFR32_RAC_SYEN_SYENVCOPFET,      (1 << 8)
.equ EFR32_RAC_SYEN_SYENVCOREG,       (1 << 9)
.equ EFR32_RAC_SYEN_SYLODIVEN,        (1 << 10)
.equ EFR32_RAC_SYEN_SYLODIVLDOBIASEN, (1 << 11)
.equ EFR32_RAC_SYEN_SYLODIVLDOEN,     (1 << 12)
.equ EFR32_RAC_SYEN_STAGE1, (EFR32_RAC_SYEN_SYCHPEN | EFR32_RAC_SYEN_SYENCHPREG | \
    EFR32_RAC_SYEN_SYENCHPREPLICA | EFR32_RAC_SYEN_SYENMMDREG | \
    EFR32_RAC_SYEN_SYENMMDREPLICA1 | EFR32_RAC_SYEN_SYENVCOBIAS | \
    EFR32_RAC_SYEN_SYLODIVLDOBIASEN)
.equ EFR32_RAC_SYEN_STAGE3, (EFR32_RAC_SYEN_SYENVCOREG | EFR32_RAC_SYEN_SYLODIVLDOEN)
.equ EFR32_RAC_SYMMDCTRL,             (EFR32_RAC_BASE + 0x168)
.equ EFR32_RAC_SYMMDCTRL_SYMMDENRSDIG, (1 << 0)
.equ EFR32_RAC_SYMMDCTRL_TX,          (2 << 9) /* SYMMDMODE=2 */
.equ EFR32_RAC_DIGCLKRETIMECTRL,      (EFR32_RAC_BASE + 0x16c)
.equ EFR32_RAC_XORETIMECTRL,          (EFR32_RAC_BASE + 0x174)
.equ EFR32_RAC_RETIME_ENRETIME,       (1 << 0) /* same position in both RETIMECTRLs */
.equ EFR32_RAC_RETIME_RESETN,         (1 << 2)

.equ EFR32_SYNTH_VCDACCTRL,           (EFR32_SYNTH_BASE + 0x02c)
.equ EFR32_SYNTH_VCDACCTRL_VCDACEN,   (1 << 6)
.equ EFR32_SYNTH_VCDACCTRL_LPFEN,     (1 << 7)
.equ EFR32_SYNTH_VCDACCTRL_LPFQSEN,   (1 << 8)
.equ EFR32_SYNTH_CMD,                 (EFR32_SYNTH_BASE + 0x00c)
.equ EFR32_SYNTH_CMD_SYNTHSTART,      (1 << 0)
.equ EFR32_SYNTH_CMD_SYNTHSTOP,       (1 << 1)
.equ EFR32_SYNTH_CMD_DISABLEIF,       (1 << 3)
.equ EFR32_SYNTH_CMD_CAPCALSTART,     (1 << 4)
.equ EFR32_SYNTH_VCOTUNING,           (EFR32_SYNTH_BASE + 0x04c)
.equ EFR32_SYNTH_VCOTUNING_VCAPSEL_TX, (0x10 << 11)
.equ EFR32_SYNTH_VCOTUNING_TOP_BIT,   10 /* VCOTUNING is 11 bits wide */
.equ EFR32_SYNTH_VCORANGE,            (EFR32_SYNTH_BASE + 0x054)
.equ EFR32_SYNTH_VCORANGE_TX,         ((3 << 3) | (5 << 6)) /* HIGHTHRESH=3, LOWTHRESH=5 */
.equ EFR32_SYNTH_VCORANGE_LOAD,       (1 << 11)
.equ EFR32_SYNTH_VCORANGE_THROUGH,    (1 << 18)
.equ EFR32_SYNTH_LOCNTCTRL,           (EFR32_SYNTH_BASE + 0x088)
.equ EFR32_SYNTH_LOCNTCTRL_ENABLE,    (1 << 0)
.equ EFR32_SYNTH_LOCNTCTRL_CLEAR,     (1 << 1)
.equ EFR32_SYNTH_LOCNTCTRL_RUN,       (1 << 2)
.equ EFR32_SYNTH_LOCNTCTRL_READ,      (1 << 3)
.equ EFR32_SYNTH_LOCNTCTRL_NUMCYCLE_SHIFT, 4
.equ EFR32_SYNTH_LOCNTCTRL_NUMCYCLE_MASK,  (0xf << EFR32_SYNTH_LOCNTCTRL_NUMCYCLE_SHIFT)
.equ EFR32_SYNTH_LOCNTSTATUS,         (EFR32_SYNTH_BASE + 0x08c)
.equ EFR32_SYNTH_LOCNTSTATUS_LOCOUNT_MASK, 0x0007ffff
.equ EFR32_SYNTH_LOCNTSTATUS_BUSY,    (1 << 19)
.equ EFR32_SYNTH_LOCNTTARGET,         (EFR32_SYNTH_BASE + 0x090)
/* Stock bounds each LO-count measurement to this many BUSY polls before
 * abandoning the calibration (and then does not start the synthesizer). */
.equ EFR32_SYNTH_LOCNT_POLL_BOUND,    2047
.equ EFR32_SEQ_TRACE_SYNTH_TUNING,    0x00004fbc /* diagnostic only */
.equ EFR32_SEQ_SYNTH_CAL_ABORTED,     0xffffffff

/*
 * Stock's microsecond-scale busy wait: count = n * 195 - 111, then subtract
 * 30 per pass while count >= 0, with the count held in memory each pass.
 * Reproduced with the same memory round trip so the per-pass cost, and so
 * the delay, stays comparable rather than becoming several times shorter.
 */
.equ EFR32_SEQ_DELAY_PER_UNIT,        195
.equ EFR32_SEQ_DELAY_BIAS,            111
.equ EFR32_SEQ_DELAY_STEP,            30

.ltorg

.thumb_func
efr32_seq_delay:
    ldr r1, =EFR32_SEQ_DELAY_PER_UNIT
    muls r0, r1
    subs r0, #EFR32_SEQ_DELAY_BIAS
    sub sp, #4
    str r0, [sp]
    bmi 2f
1:  ldr r0, [sp]
    subs r0, #EFR32_SEQ_DELAY_STEP
    str r0, [sp]
    bpl 1b
2:  add sp, #4
    bx lr

/* Per-bit successive-approximation schedule, highest VCOTUNING bit last in
 * memory (index = bit number).  Both columns are read directly off the
 * stock trace: the low bits written alongside each trial bit, and the
 * LOCNTCTRL.NUMCYCLE programmed for that trial. */
.section .rodata, "a", %progbits
efr32_seq_vcotuning_trial_low_bits:
    .byte 0, 0, 0, 1, 1, 2, 4, 8, 16, 32, 32
efr32_seq_locnt_trial_numcycle:
    .byte 5, 5, 5, 4, 4, 4, 4, 3, 2, 2, 2
.section .text, "ax", %progbits

/* Returns the calibrated VCOTUNING register value in r0, or
 * EFR32_SEQ_SYNTH_CAL_ABORTED.  Clobbers r0-r3; preserves r4-r7. */
.ltorg

.thumb_func
efr32_seq_vco_calibrate:
    push {r4-r7, lr}
    ldr r2, =(EFR32_SYNTH_LOCNTCTRL + EFR32_ALIAS_SET)
    movs r1, #EFR32_SYNTH_LOCNTCTRL_READ
    str r1, [r2]
    ldr r4, =EFR32_SYNTH_VCOTUNING_VCAPSEL_TX   /* accepted bits so far */
    movs r5, #EFR32_SYNTH_VCOTUNING_TOP_BIT     /* trial bit index */
1:  movs r6, #1
    lsls r6, r5
    orrs r6, r4                                 /* trial = accepted | bit */
    ldr r0, =efr32_seq_vcotuning_trial_low_bits
    ldrb r0, [r0, r5]
    orrs r0, r6
    ldr r1, =EFR32_SYNTH_VCOTUNING
    str r0, [r1]
    ldr r1, =(EFR32_SYNTH_LOCNTCTRL + EFR32_ALIAS_CLR)
    movs r0, #EFR32_SYNTH_LOCNTCTRL_NUMCYCLE_MASK
    str r0, [r1]
    ldr r0, =efr32_seq_locnt_trial_numcycle
    ldrb r0, [r0, r5]
    lsls r0, #EFR32_SYNTH_LOCNTCTRL_NUMCYCLE_SHIFT
    movs r1, #EFR32_SYNTH_LOCNTCTRL_CLEAR
    orrs r0, r1
    ldr r1, =(EFR32_SYNTH_LOCNTCTRL + EFR32_ALIAS_SET)
    str r0, [r1]
    movs r0, #EFR32_SYNTH_LOCNTCTRL_RUN
    str r0, [r1]
    ldr r0, =EFR32_RAC_CTRL
    ldr r0, [r0]
    movs r1, #EFR32_RAC_CTRL_FORCEDISABLE
    tst r0, r1
    bne 8f
    ldr r7, =EFR32_SYNTH_LOCNT_POLL_BOUND
    ldr r2, =EFR32_SYNTH_LOCNTSTATUS
    ldr r3, =EFR32_SYNTH_LOCNTSTATUS_BUSY
2:  ldr r0, [r2]
    tst r0, r3
    beq 3f
    subs r7, #1
    bne 2b
    b 8f
3:  ldr r1, =EFR32_SYNTH_LOCNTSTATUS_LOCOUNT_MASK
    ands r0, r1
    ldr r1, =EFR32_SYNTH_LOCNTTARGET
    ldr r1, [r1]
    cmp r1, r0
    bhs 4f                 /* count <= target: VCO not too fast, drop bit */
    mov r4, r6             /* count > target: keep the trial capacitance */
4:  subs r5, #1
    bpl 1b
    ldr r1, =(EFR32_SYNTH_LOCNTCTRL + EFR32_ALIAS_CLR)
    movs r0, #EFR32_SYNTH_LOCNTCTRL_READ
    str r0, [r1]
    ldr r1, =EFR32_SYNTH_VCOTUNING
    str r4, [r1]
    mov r0, r4
    pop {r4-r7, pc}
8:  ldr r0, =EFR32_SEQ_SYNTH_CAL_ABORTED
    pop {r4-r7, pc}

/* Brings the TX synthesizer up in stock's order, then the PA.  Clobbers r0-r3. */
.ltorg

.thumb_func
efr32_seq_synth_start:
    push {r4, lr}
    bl efr32_seq_synth_power_up
    cmp r0, #0
    bne 9f
    bl efr32_seq_pa_start
9:  pop {r4, pc}

/* The synthesizer part of efr32_seq_synth_start: supplies up, configured for TX, calibrated
 * (and the calibration cached) and started; the PA stays off.  Returns r0 as
 * efr32_seq_synth_lock.  Clobbers r0-r3. */
.ltorg

.thumb_func
efr32_seq_synth_power_up:
    push {r4, lr}
    ldr r0, =(EFR32_RAC_RADIOEN + EFR32_ALIAS_SET)
    movs r1, #(EFR32_RAC_RADIOEN_PREEN | EFR32_RAC_RADIOEN_RFBIASEN)
    str r1, [r0]
    ldr r0, =(EFR32_RAC_XORETIMECTRL + EFR32_ALIAS_CLR)
    movs r1, #EFR32_RAC_RETIME_RESETN
    str r1, [r0]
    ldr r0, =(EFR32_RAC_DIGCLKRETIMECTRL + EFR32_ALIAS_CLR)
    str r1, [r0]
    ldr r0, =(EFR32_RAC_SYMMDCTRL + EFR32_ALIAS_CLR)
    movs r1, #EFR32_RAC_SYMMDCTRL_SYMMDENRSDIG
    str r1, [r0]

    ldr r0, =(EFR32_RAC_SYCAL + EFR32_ALIAS_CLR)
    ldr r1, =(EFR32_RAC_SYCAL_SYVCOMORECURRENT | EFR32_RAC_SYCAL_SYVCOSLOWNOISEFILTER)
    str r1, [r0]
    bl efr32_seq_synth_config_tx
    ldr r0, =(EFR32_RAC_SYEN + EFR32_ALIAS_SET)
    ldr r1, =EFR32_RAC_SYEN_STAGE1
    str r1, [r0]
    ldr r0, =(EFR32_RAC_XORETIMECTRL + EFR32_ALIAS_CLR)
    movs r1, #EFR32_RAC_RETIME_RESETN
    str r1, [r0]
    ldr r0, =(EFR32_RAC_DIGCLKRETIMECTRL + EFR32_ALIAS_CLR)
    str r1, [r0]
    ldr r0, =(EFR32_RAC_SYMMDCTRL + EFR32_ALIAS_CLR)
    movs r1, #EFR32_RAC_SYMMDCTRL_SYMMDENRSDIG
    str r1, [r0]
    ldr r0, =(EFR32_RAC_RADIOEN + EFR32_ALIAS_SET)
    movs r1, #EFR32_RAC_RADIOEN_PRESTB100UDIS
    str r1, [r0]
    ldr r0, =(EFR32_RAC_PATRIM1 + EFR32_ALIAS_CLR)
    ldr r1, =EFR32_RAC_PATRIM1_TX0DBMTRIMTAPCAP_MASK
    str r1, [r0]
    ldr r0, =(EFR32_RAC_SYNTHREGCTRL + EFR32_ALIAS_CLR)
    ldr r1, =EFR32_RAC_SYNTHREGCTRL_CHPLDOVREFTRIM_MASK
    str r1, [r0]
    ldr r0, =(EFR32_RAC_SYNTHREGCTRL + EFR32_ALIAS_SET)
    ldr r1, =EFR32_RAC_SYNTHREGCTRL_CHPLDOVREFTRIM_TX
    str r1, [r0]
    ldr r0, =(EFR32_RAC_SYNTHENCTRL + EFR32_ALIAS_SET)
    movs r1, #EFR32_RAC_SYNTHENCTRL_VCOSTARTUP
    str r1, [r0]
    movs r0, #1
    bl efr32_seq_delay
    ldr r0, =(EFR32_RAC_SYEN + EFR32_ALIAS_SET)
    ldr r1, =EFR32_RAC_SYEN_SYENVCOPFET
    str r1, [r0]
    movs r0, #2
    bl efr32_seq_delay
    ldr r0, =(EFR32_RAC_SYEN + EFR32_ALIAS_SET)
    ldr r1, =EFR32_RAC_SYEN_STAGE3
    str r1, [r0]
    movs r0, #2
    bl efr32_seq_delay
    ldr r0, =(EFR32_RAC_SYEN + EFR32_ALIAS_SET)
    ldr r1, =EFR32_RAC_SYEN_SYLODIVEN
    str r1, [r0]
    movs r0, #EFR32_SEQ_SYNTH_LOCK_CAL_AND_CACHE
    bl efr32_seq_synth_lock
    pop {r4, pc}

.ltorg

/* Second half of stock's synthesizer start, shared by the TX warm-up and
 * the TX->RX retune: VCDAC/loop filter on, VCO calibration, SYNTHSTART.
 * Returns r0 = 0 when started, nonzero when the calibration aborted.
 * Clobbers r0-r3. */
.thumb_func
efr32_seq_synth_lock:
    push {r4, lr}
    mov r4, r0             /* EFR32_SEQ_SYNTH_LOCK_* mode */
    ldr r0, =(EFR32_SYNTH_VCDACCTRL + EFR32_ALIAS_SET)
    ldr r1, =(EFR32_SYNTH_VCDACCTRL_VCDACEN | EFR32_SYNTH_VCDACCTRL_LPFEN | EFR32_SYNTH_VCDACCTRL_LPFQSEN)
    str r1, [r0]
    movs r0, #4
    bl efr32_seq_delay
    ldr r0, =(EFR32_RAC_SYNTHENCTRL + EFR32_ALIAS_CLR)
    movs r1, #EFR32_RAC_SYNTHENCTRL_VCOSTARTUP
    str r1, [r0]
    ldr r0, =(EFR32_SYNTH_VCORANGE + EFR32_ALIAS_SET)
    ldr r1, =EFR32_SYNTH_VCORANGE_LOAD
    str r1, [r0]
    ldr r1, =EFR32_SYNTH_VCORANGE_THROUGH
    str r1, [r0]
    ldr r0, =EFR32_SYNTH_CMD
    movs r1, #EFR32_SYNTH_CMD_CAPCALSTART
    str r1, [r0]
    ldr r0, =(EFR32_SYNTH_LOCNTCTRL + EFR32_ALIAS_SET)
    movs r1, #EFR32_SYNTH_LOCNTCTRL_ENABLE
    str r1, [r0]

    cmp r4, #EFR32_SEQ_SYNTH_LOCK_CACHED
    bne 6f
    /* Same channel as this event's TXWARM: reuse its calibration, as
     * stock's per-channel cache does (SS7.61, SS7.67). */
    ldr r0, =EFR32_SEQ_TX_VCOTUNING_CACHE
    ldr r0, [r0]
    ldr r1, =EFR32_SYNTH_VCOTUNING
    str r0, [r1]
    b 7f
6:  bl efr32_seq_vco_calibrate
    ldr r1, =EFR32_SEQ_TRACE_SYNTH_TUNING
    str r0, [r1]
    adds r0, #1            /* EFR32_SEQ_SYNTH_CAL_ABORTED + 1 == 0 */
    beq 8f
    subs r0, #1
    cmp r4, #EFR32_SEQ_SYNTH_LOCK_CAL_AND_CACHE
    bne 7f
    ldr r1, =EFR32_SEQ_TX_VCOTUNING_CACHE
    str r0, [r1]
7:  ldr r0, =EFR32_SYNTH_CMD
    movs r1, #EFR32_SYNTH_CMD_SYNTHSTART
    str r1, [r0]
    ldr r0, =EFR32_SYNTH_LOCNTCTRL
    movs r1, #0
    str r1, [r0]
    movs r0, #2
    bl efr32_seq_delay
    ldr r0, =(EFR32_SYNTH_VCDACCTRL + EFR32_ALIAS_CLR)
    movs r1, #EFR32_SYNTH_VCDACCTRL_VCDACEN
    str r1, [r0]
    ldr r1, =EFR32_SYNTH_VCDACCTRL_LPFQSEN
    str r1, [r0]
    movs r0, #2
    bl efr32_seq_delay
    ldr r0, =(EFR32_SYNTH_VCORANGE + EFR32_ALIAS_CLR)
    ldr r1, =(EFR32_SYNTH_VCORANGE_LOAD | EFR32_SYNTH_VCORANGE_THROUGH)
    str r1, [r0]
    ldr r0, =EFR32_RAC_SYTRIM1
    ldr r1, =EFR32_RAC_SYTRIM1_RUNNING
    str r1, [r0]
    movs r0, #0
    pop {r4, pc}
8:  movs r0, #1
    pop {r4, pc}

/* Powers the synthesizer down in stock's TXPD order (SS7.62).  Clobbers
 * r0-r3. */
.equ EFR32_RAC_SYEN_REMAINING, (EFR32_RAC_SYEN_SYCHPEN | EFR32_RAC_SYEN_SYENCHPREG | \
    EFR32_RAC_SYEN_SYENCHPREPLICA | EFR32_RAC_SYEN_SYENMMDREG | \
    EFR32_RAC_SYEN_SYENMMDREPLICA1 | (1 << 6) /* SYENMMDREPLICA2 */ | \
    EFR32_RAC_SYEN_SYENVCOBIAS | EFR32_RAC_SYEN_SYLODIVLDOBIASEN | \
    (1 << 13) /* SYSTARTCHPREG */ | (1 << 14) /* SYSTARTMMDREG */)

.ltorg

.thumb_func
efr32_seq_synth_stop:
    push {r4, lr}
    bl efr32_seq_pa_stop
    bl efr32_seq_synth_power_down
    pop {r4, pc}

.ltorg

/* Stock's synthesizer power-down (TXPD and RXPD share it, SS7.62/SS7.65).
 * Clobbers r0-r3. */
.thumb_func
efr32_seq_synth_power_down:
    push {r4, lr}
    ldr r0, =(EFR32_RAC_XORETIMECTRL + EFR32_ALIAS_CLR)
    movs r1, #EFR32_RAC_RETIME_ENRETIME
    str r1, [r0]
    ldr r0, =(EFR32_RAC_DIGCLKRETIMECTRL + EFR32_ALIAS_CLR)
    str r1, [r0]
    ldr r0, =(EFR32_RAC_XORETIMECTRL + EFR32_ALIAS_CLR)
    movs r1, #EFR32_RAC_RETIME_RESETN
    str r1, [r0]
    ldr r0, =(EFR32_RAC_DIGCLKRETIMECTRL + EFR32_ALIAS_CLR)
    str r1, [r0]
    ldr r0, =(EFR32_RAC_SYMMDCTRL + EFR32_ALIAS_CLR)
    movs r1, #EFR32_RAC_SYMMDCTRL_SYMMDENRSDIG
    str r1, [r0]
    ldr r0, =EFR32_SYNTH_CMD
    movs r1, #EFR32_SYNTH_CMD_SYNTHSTOP
    str r1, [r0]
    ldr r0, =(EFR32_RAC_SYEN + EFR32_ALIAS_CLR)
    ldr r1, =EFR32_RAC_SYEN_SYLODIVLDOEN
    str r1, [r0]
    ldr r1, =EFR32_RAC_SYEN_SYENVCOREG
    str r1, [r0]
    movs r0, #1
    bl efr32_seq_delay
    ldr r0, =(EFR32_RAC_SYEN + EFR32_ALIAS_CLR)
    ldr r1, =EFR32_RAC_SYEN_SYENVCOPFET
    str r1, [r0]
    ldr r2, =(EFR32_RAC_RADIOEN + EFR32_ALIAS_CLR)
    movs r1, #EFR32_RAC_RADIOEN_PRESTB100UDIS
    str r1, [r2]
    ldr r1, =EFR32_RAC_SYEN_REMAINING
    str r1, [r0]
    ldr r0, =(EFR32_SYNTH_VCDACCTRL + EFR32_ALIAS_CLR)
    ldr r1, =(EFR32_SYNTH_VCDACCTRL_VCDACEN | EFR32_SYNTH_VCDACCTRL_LPFEN | EFR32_SYNTH_VCDACCTRL_LPFQSEN)
    str r1, [r0]
    ldr r0, =(EFR32_RAC_SYNTHENCTRL + EFR32_ALIAS_CLR)
    movs r1, #EFR32_RAC_SYNTHENCTRL_VCOSTARTUP
    str r1, [r0]
    ldr r0, =EFR32_RAC_RADIOEN
    movs r1, #0
    str r1, [r0]
    ldr r0, =(EFR32_RAC_SYNTHENCTRL + EFR32_ALIAS_CLR)
    ldr r1, =EFR32_RAC_SYNTHENCTRL_SYNTHSTARTREQ
    str r1, [r0]
    pop {r4, pc}

/*
 * 6 dBm PA bring-up and ramp-down (SS7.62), in the stock TXWARM/TXPD
 * handlers' recorded order and waits.  Stock's ramp-down waits on
 * MODEM.SEQIF.TXRAMPDONE without a bound; bounded here like the other
 * waits in this file.
 */
.equ EFR32_MODEM_BASE,                0xa8014000
.equ EFR32_MODEM_RAMPCTRL,            (EFR32_MODEM_BASE + 0x0c4)
.equ EFR32_MODEM_RAMPCTRL_RAMPDIS,    (1 << 23)
.equ EFR32_MODEM_SEQIF,               (EFR32_MODEM_BASE + 0x24c)
.equ EFR32_MODEM_SEQIF_TXRAMPDONE,    (1 << 3)
.equ EFR32_RAC_CMD,                   (EFR32_RAC_BASE + 0x010)
.equ EFR32_RAC_CMD_PAENSET,           (1 << 12)
.equ EFR32_RAC_CMD_PAENCLEAR,         (1 << 13)
.equ EFR32_RAC_PAENCTRL,              (EFR32_RAC_BASE + 0x0b4)
.equ EFR32_RAC_PAENCTRL_PARAMP,       (1 << 8)
.equ EFR32_RAC_PAENCTRL_INVRAMPCLK,   (1 << 16)
.equ EFR32_RAC_APC,                   (EFR32_RAC_BASE + 0x0b8)
.equ EFR32_RAC_APC_ENAPCSW,           (1 << 2)
.equ EFR32_RAC_APC_AMPCONTROLLIMITSW_MAX, (0xff << 24)
.equ EFR32_RAC_LNAMIXEN,              (EFR32_RAC_BASE + 0x108)
.equ EFR32_RAC_LNAMIXEN_LNAMIXENLDO,  (1 << 0)
.equ EFR32_RAC_PATRIM3,               (EFR32_RAC_BASE + 0x11c)
.equ EFR32_RAC_PATRIM3_TX6DBMTRIMREGBLEEDAUTO, (1 << 11)
.equ EFR32_RAC_PATRIM3_TX6DBMTRIMREGPSR,       (1 << 14)
.equ EFR32_RAC_PACTRL,                (EFR32_RAC_BASE + 0x120)
.equ EFR32_RAC_PACTRL_TX6DBMSLICERESET, (1 << 24)
.equ EFR32_RAC_RFPATHEN,              (EFR32_RAC_BASE + 0x140)
.equ EFR32_RAC_RFPATHEN_LNAMIXRFATTDCEN, (1 << 2)
.equ EFR32_RAC_RFPATHEN_LNAMIXTRSW,   (1 << 4)
.equ EFR32_RAC_RX_LNAMIXLDOLOWCUR,    (1 << 4)
.equ EFR32_RAC_RX_LNAMIXREGLOADEN,    (1 << 5)
.equ EFR32_RAC_TX,                    (EFR32_RAC_BASE + 0x148)
.equ EFR32_RAC_TX_TX0DBMENBLEEDPREDRVREG, (1 << 0)
.equ EFR32_RAC_TX_TX0DBMENBLEEDREG,   (1 << 1)
.equ EFR32_RAC_TX_TX0DBMENPREDRV,     (1 << 2)
.equ EFR32_RAC_TX_TX0DBMENPREDRVREG,  (1 << 3)
.equ EFR32_RAC_TX_TX0DBMENPREDRVREGBIAS, (1 << 4)
.equ EFR32_RAC_TX_TX0DBMENBIAS,       (1 << 5)
.equ EFR32_RAC_TX_TX0DBMENREG,        (1 << 7)
.equ EFR32_RAC_TX_TX6DBMENBLEEDPREDRVREG, (1 << 16)
.equ EFR32_RAC_TX_TX6DBMENBLEEDREG,   (1 << 17)
.equ EFR32_RAC_TX_TX6DBMENPREDRVREG,  (1 << 19)
.equ EFR32_RAC_TX_TX6DBMENRAMPCLK,    (1 << 22)
.equ EFR32_RAC_TX_TX6DBMENREG,        (1 << 23)
.equ EFR32_RAC_TX_TX6DBMENPACORE,     (1 << 24)
.equ EFR32_RAC_TX_TX6DBMENPAOUT,      (1 << 25)
.equ EFR32_RAC_TX_ENPAPOWER,          (1 << 30)
.equ EFR32_RAC_TX_ENPASELSLICE,       (1 << 31)
.equ EFR32_RAC_TX_BLEED, (EFR32_RAC_TX_TX0DBMENBLEEDPREDRVREG | EFR32_RAC_TX_TX0DBMENBLEEDREG | \
    EFR32_RAC_TX_TX6DBMENBLEEDPREDRVREG | EFR32_RAC_TX_TX6DBMENBLEEDREG)
.equ EFR32_RAC_SYLOEN,                (EFR32_RAC_BASE + 0x160)
.equ EFR32_RAC_SYLOEN_SYLODIVTLO6DBM2G4EN, (1 << 10)
.equ EFR32_RAC_AGCOVERWRITE,          (EFR32_RAC_BASE + 0x188)
/* LNA/mixer held at manual maximum attenuation, one slice, while the PA is
 * on: ENMANLNAMIXRFATT, ENMANLNAMIXSLICE, MANLNAMIXRFATT=63,
 * MANLNAMIXSLICE=1. */
.equ EFR32_RAC_AGCOVERWRITE_TX,       ((1 << 0) | (1 << 1) | (0x3f << 4) | (1 << 10))
.equ EFR32_SEQ_RAMP_WAIT_BOUND,       0x00010000

.ltorg

.thumb_func
efr32_seq_pa_start:
    push {r4, lr}
    ldr r0, =(EFR32_RAC_APC + EFR32_ALIAS_SET)
    ldr r1, =(EFR32_RAC_APC_ENAPCSW | EFR32_RAC_APC_AMPCONTROLLIMITSW_MAX)
    str r1, [r0]
    ldr r0, =EFR32_RAC_CMD
    ldr r1, =EFR32_RAC_CMD_PAENSET
    str r1, [r0]
    ldr r4, =(EFR32_RAC_TX + EFR32_ALIAS_SET)
    ldr r1, =(EFR32_RAC_TX_ENPAPOWER | EFR32_RAC_TX_ENPASELSLICE)
    str r1, [r4]
    ldr r0, =(EFR32_RAC_PAENCTRL + EFR32_ALIAS_SET)
    ldr r1, =EFR32_RAC_PAENCTRL_INVRAMPCLK
    str r1, [r0]
    ldr r0, =EFR32_RAC_SYLOEN
    ldr r1, =EFR32_RAC_SYLOEN_SYLODIVTLO6DBM2G4EN
    str r1, [r0]
    ldr r0, =EFR32_RAC_AGCOVERWRITE
    ldr r1, =EFR32_RAC_AGCOVERWRITE_TX
    str r1, [r0]
    ldr r0, =EFR32_RAC_RFPATHEN
    movs r1, #(EFR32_RAC_RFPATHEN_LNAMIXRFATTDCEN | EFR32_RAC_RFPATHEN_LNAMIXTRSW)
    str r1, [r0]
    ldr r0, =(EFR32_RAC_RX + EFR32_ALIAS_SET)
    movs r1, #(EFR32_RAC_RX_LNAMIXLDOLOWCUR | EFR32_RAC_RX_LNAMIXREGLOADEN)
    str r1, [r0]
    ldr r0, =(EFR32_RAC_LNAMIXEN + EFR32_ALIAS_SET)
    movs r1, #EFR32_RAC_LNAMIXEN_LNAMIXENLDO
    str r1, [r0]
    ldr r0, =(EFR32_RAC_PATRIM1 + EFR32_ALIAS_CLR)
    ldr r1, =EFR32_RAC_PATRIM1_TX0DBMTRIMTAPCAP_MASK
    str r1, [r0]
    ldr r0, =(EFR32_RAC_PATRIM3 + EFR32_ALIAS_SET)
    ldr r1, =EFR32_RAC_PATRIM3_TX6DBMTRIMREGPSR
    str r1, [r0]
    ldr r1, =EFR32_RAC_TX_TX6DBMENREG
    str r1, [r4]
    movs r0, #6
    bl efr32_seq_delay
    ldr r0, =(EFR32_RAC_PACTRL + EFR32_ALIAS_SET)
    ldr r1, =EFR32_RAC_PACTRL_TX6DBMSLICERESET
    str r1, [r0]
    movs r0, #2
    bl efr32_seq_delay
    ldr r0, =(EFR32_RAC_PACTRL + EFR32_ALIAS_CLR)
    ldr r1, =EFR32_RAC_PACTRL_TX6DBMSLICERESET
    str r1, [r0]
    ldr r1, =EFR32_RAC_TX_TX6DBMENPREDRVREG
    str r1, [r4]
    movs r0, #8
    bl efr32_seq_delay
    ldr r1, =EFR32_RAC_TX_TX6DBMENPACORE
    str r1, [r4]
    movs r0, #3
    bl efr32_seq_delay
    ldr r1, =EFR32_RAC_TX_TX6DBMENPAOUT
    str r1, [r4]
    ldr r1, =EFR32_RAC_TX_TX6DBMENRAMPCLK
    str r1, [r4]
    movs r0, #1
    bl efr32_seq_delay
    ldr r0, =(EFR32_RAC_PAENCTRL + EFR32_ALIAS_SET)
    ldr r1, =EFR32_RAC_PAENCTRL_PARAMP
    str r1, [r0]
    ldr r0, =(EFR32_RAC_TX + EFR32_ALIAS_CLR)
    ldr r1, =EFR32_RAC_TX_BLEED
    str r1, [r0]
    ldr r0, =(EFR32_RAC_PATRIM3 + EFR32_ALIAS_CLR)
    ldr r1, =EFR32_RAC_PATRIM3_TX6DBMTRIMREGBLEEDAUTO
    str r1, [r0]
    ldr r0, =(EFR32_RAC_APC + EFR32_ALIAS_CLR)
    movs r1, #EFR32_RAC_APC_ENAPCSW
    str r1, [r0]
    pop {r4, pc}

.ltorg

.thumb_func
efr32_seq_pa_stop:
    push {r4, lr}
    ldr r0, =(EFR32_RAC_PATRIM3 + EFR32_ALIAS_SET)
    ldr r1, =EFR32_RAC_PATRIM3_TX6DBMTRIMREGBLEEDAUTO
    str r1, [r0]
    ldr r0, =(EFR32_RAC_TX + EFR32_ALIAS_SET)
    ldr r1, =(EFR32_RAC_TX_TX6DBMENBLEEDPREDRVREG | EFR32_RAC_TX_TX6DBMENBLEEDREG | EFR32_RAC_TX_TX6DBMENRAMPCLK)
    str r1, [r0]
    /* A PA that never ramped up (an RX2TX the M33 cancelled: the PA comes up only on RESPOND)
     * has no ramp-down to wait for; waiting out the bound held this handler, and with it the
     * RAC in TXPD, ~18 ms in QEMU. */
    ldr r0, =EFR32_RAC_PAENCTRL
    ldr r4, [r0]
    ldr r0, =(EFR32_RAC_PAENCTRL + EFR32_ALIAS_CLR)
    ldr r1, =EFR32_RAC_PAENCTRL_PARAMP
    str r1, [r0]
    tst r4, r1
    beq 2f
    ldr r0, =(EFR32_MODEM_SEQIF + EFR32_ALIAS_CLR)
    movs r1, #EFR32_MODEM_SEQIF_TXRAMPDONE
    str r1, [r0]
    ldr r0, =EFR32_MODEM_RAMPCTRL
    ldr r0, [r0]
    ldr r1, =EFR32_MODEM_RAMPCTRL_RAMPDIS
    tst r0, r1
    bne 2f
    ldr r2, =EFR32_SEQ_RAMP_WAIT_BOUND
    ldr r0, =EFR32_MODEM_SEQIF
1:  ldr r1, [r0]
    movs r3, #EFR32_MODEM_SEQIF_TXRAMPDONE
    tst r1, r3
    bne 2f
    subs r2, #1
    bne 1b
2:  movs r0, #1
    bl efr32_seq_delay
    ldr r4, =(EFR32_RAC_TX + EFR32_ALIAS_CLR)
    ldr r1, =EFR32_RAC_TX_TX6DBMENPAOUT
    str r1, [r4]
    ldr r1, =EFR32_RAC_TX_TX6DBMENRAMPCLK
    str r1, [r4]
    movs r0, #4
    bl efr32_seq_delay
    ldr r1, =(EFR32_RAC_TX_TX0DBMENPREDRV | EFR32_RAC_TX_TX0DBMENBIAS | EFR32_RAC_TX_TX6DBMENPACORE)
    str r1, [r4]
    movs r0, #1
    bl efr32_seq_delay
    ldr r1, =(EFR32_RAC_TX_TX0DBMENPREDRVREG | EFR32_RAC_TX_TX0DBMENPREDRVREGBIAS | \
        EFR32_RAC_TX_TX0DBMENREG | EFR32_RAC_TX_TX6DBMENPREDRVREG | EFR32_RAC_TX_TX6DBMENREG)
    str r1, [r4]
    ldr r1, =(EFR32_RAC_TX_TX6DBMENPACORE | EFR32_RAC_TX_TX6DBMENPAOUT | \
        EFR32_RAC_TX_ENPAPOWER | EFR32_RAC_TX_ENPASELSLICE)
    str r1, [r4]
    ldr r0, =(EFR32_RAC_PAENCTRL + EFR32_ALIAS_CLR)
    ldr r1, =EFR32_RAC_PAENCTRL_INVRAMPCLK
    str r1, [r0]
    ldr r0, =(EFR32_RAC_RFPATHEN + EFR32_ALIAS_CLR)
    movs r1, #EFR32_RAC_RFPATHEN_LNAMIXTRSW
    str r1, [r0]
    ldr r0, =(EFR32_RAC_PATRIM3 + EFR32_ALIAS_CLR)
    ldr r1, =EFR32_RAC_PATRIM3_TX6DBMTRIMREGPSR
    str r1, [r0]
    ldr r0, =EFR32_RAC_SYLOEN
    movs r1, #0
    str r1, [r0]
    ldr r0, =(EFR32_RAC_LNAMIXEN + EFR32_ALIAS_CLR)
    movs r1, #EFR32_RAC_LNAMIXEN_LNAMIXENLDO
    str r1, [r0]
    ldr r0, =(EFR32_RAC_RX + EFR32_ALIAS_CLR)
    movs r1, #(EFR32_RAC_RX_LNAMIXLDOLOWCUR | EFR32_RAC_RX_LNAMIXREGLOADEN)
    str r1, [r0]
    ldr r0, =EFR32_RAC_AGCOVERWRITE
    movs r1, #0
    str r1, [r0]
    ldr r0, =EFR32_RAC_CMD
    ldr r1, =EFR32_RAC_CMD_PAENCLEAR
    str r1, [r0]
    pop {r4, pc}
.ltorg

/*
 * TX->RX turnaround, in stock IRQ11's recorded order (SS7.63): PA down, keep
 * the RX request asserted, retune the running synthesizer for RX, power the
 * receive chain, then calibrate the IF PGA's DC offset.
 */
.equ EFR32_RAC_RXENSRCEN,              (EFR32_RAC_BASE + 0x008)
.equ EFR32_RAC_RXENSRCEN_SWRXEN,       (1 << 1)
.equ EFR32_SYNTH_CMD_ENABLEIF,         (1 << 2)
.equ EFR32_SYNTH_VCORANGE_RX,          ((1 << 3) | (3 << 6)) /* HIGHTHRESH=1, LOWTHRESH=3 */
.equ EFR32_RAC_SYMMDCTRL_RX,           (4 << 9) /* SYMMDMODE=4 */
.equ EFR32_RAC_RX_IFADCCAPRESET,       (1 << 0)
.equ EFR32_RAC_RX_IFADCENLDOSERIES,    (1 << 1)
.equ EFR32_RAC_RX_IFADCENLDOSHUNT,     (1 << 2)
.equ EFR32_RAC_RX_PGAENLDO,            (1 << 6)
.equ EFR32_RAC_RX_SYPFDCHPLPEN,        (1 << 9)
.equ EFR32_RAC_RX_SYPFDFPWEN,          (1 << 10)
/* Stock's RX value 0x64E29: the TX trims with SYCHPBIAS=1 and without
 * SYCHPSRCEN (SS7.64 corrects SS7.63's port, which kept SYCHPSRCEN). */
.equ EFR32_RAC_SYTRIM0_SYCHPSRCEN,      (1 << 13)
.equ EFR32_RAC_SYTRIM0_RX,             ((EFR32_RAC_SYTRIM0_TX & ~EFR32_RAC_SYTRIM0_SYCHPSRCEN) | (1 << 0))
.equ EFR32_RAC_SYLOEN_RX, ((1 << 0) | (1 << 1)) /* SYLODIVRLOADCCLK2G4EN, SYLODIVRLO2G4EN */
.equ EFR32_RAC_SYTRIM1_SYLODIVRLOADCCLKSEL, (1 << 17)
.equ EFR32_RAC_CMD_LNAENSET,           (1 << 14)
.equ EFR32_RAC_IFADCDBG,               (EFR32_RAC_BASE + 0x0e0)
/* IFADCENADC, IFADCENREFBUF, IFADCENSIDETONE, IFADCINVERTCLK; then IFADCENCLK. */
.equ EFR32_RAC_IFADCDBG_RX_POWER,      ((1 << 0) | (1 << 2) | (1 << 3) | (1 << 4))
.equ EFR32_RAC_IFADCDBG_IFADCENCLK,    (1 << 1)
.equ EFR32_RAC_RFPATHEN_LNAMIXEN,      (1 << 1)
.equ EFR32_RAC_PGACTRL,                (EFR32_RAC_BASE + 0x130)
/* PGAENBIAS, PGAENGHZ, PGAENPGAI, PGAENPGAQ, PGAENPKD, PGAENRCMOUT, PGAENSUBG. */
.equ EFR32_RAC_PGACTRL_RX_ENABLES, ((1 << 2) | (1 << 3) | (1 << 9) | (1 << 10) | (1 << 11) | (1 << 12) | (1 << 13))
.equ EFR32_SEQ_RX_LDO_SETTLE_US,       7

.thumb_func
efr32_seq_rx_turnaround:
    push {r4, lr}
    bl efr32_seq_pa_stop
    ldr r0, =(EFR32_RAC_RXENSRCEN + EFR32_ALIAS_SET)
    movs r1, #EFR32_RAC_RXENSRCEN_SWRXEN
    str r1, [r0]
    bl efr32_seq_rx_tune
    pop {r4, pc}

/*
 * RX warm-up from OFF (IRQ1, SWRXEN in OFF: a scan window, a Peripheral's anchor window).
 * Nothing in this event powered the synthesizer (the last RXPD/TXPD powered it down, RADIOEN
 * reads 0), and without a handler the RAC reached RXSEARCH with the synthesizer stopped and the
 * whole receive chain off (SYEN, SYLOEN, LNAMIXEN, RFPATHEN, IFADCDBG and RAC.RX all 0, measured
 * on silicon, docs/radio-tables.md SS7.80): it never detected a frame.  Stock's IRQ1 opens with
 * the same RAC.PRESC pulse as its TXWARM (SS7.2, fn_0x23a0).  Then: the synthesizer up exactly
 * as TXWARM brings it up, calibrated for this channel's TX and cached (a response from this
 * window then retunes with the cache, as after a TX->RX turnaround), and the receiver tuned and
 * powered exactly as TX2RX does.  So RXSEARCH is reached in the register state of the TX->RX
 * turnaround, the path that receives on silicon (SS7.64-SS7.66).  The common tail acknowledges
 * STATERXWARM.  Clobbers r0-r3.
 */
.thumb_func
efr32_seq_rx_warm:
    push {r4, lr}
    bl efr32_seq_presc_pulse
    bl efr32_seq_synth_power_up
    cmp r0, #0
    bne 9f
    bl efr32_seq_rx_tune
9:  pop {r4, pc}

/*
 * Stock's TXWARM and RXWARM prologue (SS7.2): the SR0 gate pulse around RAC.PRESC SET 5, the
 * wait on the SR0 wait bit bounded (the TXWARM path keeps its inline copy: its duration is part
 * of the measured TX ready delay).  Returns r0 = the wait's remaining count (0: bound hit).
 * Clobbers r0-r3.
 */
.thumb_func
efr32_seq_presc_pulse:
    ldr r0, =EFR32_RAC_SR0_SET
    ldr r1, =EFR32_RAC_SR0_GATE_BIT
    str r1, [r0]
    ldr r3, =EFR32_RAC_TXWARM_WAIT_BOUND
1:  ldr r0, =EFR32_RAC_SR0_BASE
    ldr r1, [r0]
    ldr r2, =EFR32_RAC_SR0_WAIT_BIT
    ands r1, r2
    beq 2f
    subs r3, #1
    bne 1b
2:  ldr r0, =EFR32_RAC_PRESC_SET
    movs r1, #EFR32_RAC_PRESC_SET_VALUE
    str r1, [r0]
    ldr r0, =EFR32_RAC_SR0_CLR
    ldr r1, =EFR32_RAC_SR0_GATE_BIT
    str r1, [r0]
    mov r0, r3
    bx lr

.ltorg

/*
 * The receive half of the TX->RX turnaround (SS7.63): retune the running synthesizer for RX,
 * power the receive chain, calibrate the IF PGA's DC offset.  Clobbers r0-r3.
 */
.thumb_func
efr32_seq_rx_tune:
    push {r4, lr}
    ldr r0, =EFR32_SYNTH_VCORANGE
    ldr r1, =EFR32_SYNTH_VCORANGE_RX
    str r1, [r0]
    ldr r0, =EFR32_SYNTH_CMD
    movs r1, #(EFR32_SYNTH_CMD_SYNTHSTOP | EFR32_SYNTH_CMD_ENABLEIF)
    str r1, [r0]
    ldr r0, =(EFR32_RAC_SYNTHENCTRL + EFR32_ALIAS_CLR)
    ldr r1, =EFR32_RAC_SYNTHENCTRL_LPFBWSEL
    str r1, [r0]
    ldr r0, =EFR32_RAC_SYMMDCTRL
    ldr r1, =EFR32_RAC_SYMMDCTRL_RX
    str r1, [r0]
    ldr r0, =(EFR32_RAC_RX + EFR32_ALIAS_SET)
    ldr r1, =(EFR32_RAC_RX_SYPFDCHPLPEN | EFR32_RAC_RX_SYPFDFPWEN)
    str r1, [r0]
    ldr r0, =(EFR32_RAC_SYEN + EFR32_ALIAS_SET)
    movs r1, #EFR32_RAC_SYEN_SYCHPLPEN
    str r1, [r0]
    ldr r0, =(EFR32_RAC_CHPCTRL + EFR32_ALIAS_SET)
    ldr r1, =EFR32_RAC_CHPCTRL_CHPOUTPUTTRISTATEMODE
    str r1, [r0]
    ldr r0, =EFR32_RAC_SYTRIM0
    ldr r1, =EFR32_RAC_SYTRIM0_RX
    str r1, [r0]
    ldr r0, =EFR32_RAC_VCOCTRL
    movs r1, #EFR32_RAC_VCOCTRL_TX
    str r1, [r0]
    ldr r0, =(EFR32_RAC_XORETIMECTRL + EFR32_ALIAS_CLR)
    movs r1, #EFR32_RAC_RETIME_ENRETIME
    str r1, [r0]
    ldr r0, =(EFR32_RAC_DIGCLKRETIMECTRL + EFR32_ALIAS_CLR)
    str r1, [r0]
    ldr r0, =EFR32_RAC_SYTRIM1
    ldr r1, =EFR32_RAC_SYTRIM1_STARTUP
    str r1, [r0]
    ldr r0, =(EFR32_RAC_SYNTHENCTRL + EFR32_ALIAS_SET)
    ldr r1, =EFR32_RAC_SYNTHENCTRL_SYNTHSTARTREQ
    str r1, [r0]
    movs r0, #EFR32_SEQ_SYNTH_LOCK_CAL
    bl efr32_seq_synth_lock
    cmp r0, #0
    bne 9f

    ldr r0, =(EFR32_RAC_SYLOEN + EFR32_ALIAS_SET)
    movs r1, #EFR32_RAC_SYLOEN_RX
    str r1, [r0]
    ldr r0, =(EFR32_RAC_SYTRIM1 + EFR32_ALIAS_SET)
    ldr r1, =EFR32_RAC_SYTRIM1_SYLODIVRLOADCCLKSEL
    str r1, [r0]
    ldr r0, =EFR32_RAC_CMD
    ldr r1, =EFR32_RAC_CMD_LNAENSET
    str r1, [r0]
    ldr r0, =(EFR32_RAC_LNAMIXEN + EFR32_ALIAS_SET)
    movs r1, #EFR32_RAC_LNAMIXEN_LNAMIXENLDO
    str r1, [r0]
    ldr r0, =(EFR32_RAC_RX + EFR32_ALIAS_CLR)
    movs r1, #EFR32_RAC_RX_IFADCCAPRESET
    str r1, [r0]
    ldr r0, =(EFR32_RAC_IFADCDBG + EFR32_ALIAS_SET)
    movs r1, #EFR32_RAC_IFADCDBG_RX_POWER
    str r1, [r0]
    ldr r0, =(EFR32_RAC_RX + EFR32_ALIAS_SET)
    movs r1, #(EFR32_RAC_RX_IFADCENLDOSERIES | EFR32_RAC_RX_IFADCENLDOSHUNT | EFR32_RAC_RX_LNAMIXLDOLOWCUR | EFR32_RAC_RX_PGAENLDO)
    str r1, [r0]
    movs r0, #EFR32_SEQ_RX_LDO_SETTLE_US
    bl efr32_seq_delay
    ldr r0, =(EFR32_RAC_IFADCDBG + EFR32_ALIAS_SET)
    movs r1, #EFR32_RAC_IFADCDBG_IFADCENCLK
    str r1, [r0]
    ldr r0, =(EFR32_RAC_RFPATHEN + EFR32_ALIAS_SET)
    movs r1, #(EFR32_RAC_RFPATHEN_LNAMIXEN | EFR32_RAC_RFPATHEN_LNAMIXRFATTDCEN)
    str r1, [r0]
    ldr r0, =(EFR32_RAC_PGACTRL + EFR32_ALIAS_SET)
    ldr r1, =EFR32_RAC_PGACTRL_RX_ENABLES
    str r1, [r0]
    ldr r0, =(EFR32_RAC_SYCAL + EFR32_ALIAS_CLR)
    ldr r1, =(EFR32_RAC_SYCAL_SYVCOMORECURRENT | EFR32_RAC_SYCAL_SYVCOSLOWNOISEFILTER)
    str r1, [r0]
    bl efr32_seq_pga_dc_cal
9:  pop {r4, pc}

.ltorg

/*
 * IF PGA DC-offset calibration (stock fn_0x21e8, SS7.63): with the LNA
 * isolated, measure MODEM.DCESTI at two opposite PGAOFF settings, interpolate
 * each of I and Q to the zero-offset P code, and set each N code to its
 * 6-bit two's complement.
 */
.equ EFR32_AGC_CTRL0,                  0xa800c020
.equ EFR32_AGC_CTRL0_ADCATTENCODE_MASK, (3 << 25)
/* ENMANLNAMIXRFATT, ENMANLNAMIXSLICE, ENMANPGAGAIN, MANLNAMIXRFATT=63,
 * MANLNAMIXSLICE=5 -- and the mask stock clears afterwards (all manual
 * enables and the three manual value fields). */
.equ EFR32_RAC_AGCOVERWRITE_DCCAL,     ((1 << 0) | (1 << 1) | (1 << 2) | (0x3f << 4) | (5 << 10))
.equ EFR32_RAC_AGCOVERWRITE_DCCAL_RELEASE, ((1 << 0) | (1 << 1) | (1 << 2) | (0x3f << 4) | (0x3f << 10) | (0xf << 20))
.equ EFR32_RAC_IFPGACTRL,              (EFR32_RAC_BASE + 0x0b0)
.equ EFR32_RAC_IFPGACTRL_DCCALON,      (1 << 19)
.equ EFR32_RAC_IFPGACTRL_DCRSTEN,      (1 << 20)
.equ EFR32_RAC_IFPGACTRL_DCESTIEN,     (1 << 21)
.equ EFR32_RAC_IFPGACTRL_DCCALDEC0_MASK,  (7 << 22)
.equ EFR32_RAC_IFPGACTRL_DCCALDCGEAR_MASK, (7 << 25)
.equ EFR32_RAC_PGACAL,                 (EFR32_RAC_BASE + 0x12c)
/* Fields PGAOFFNCALI[5:0], NCALQ[13:8], PCALI[21:16], PCALQ[29:24]. */
.equ EFR32_RAC_PGACAL_POINT_A,         0x30301010 /* P=48, N=16 */
.equ EFR32_RAC_PGACAL_POINT_B,         0x10103030 /* P=16, N=48 */
.equ EFR32_RAC_PGACAL_MIDSCALE,        0x20202020
.equ EFR32_RAC_PGACAL_CODE_MASK,       0x3f
.equ EFR32_RAC_PGACAL_N_FIELDS,        0x3f3f
.equ EFR32_RAC_PGACAL_N_NEGATE_ONE,    0x0101 /* +1 in each N field */
.equ EFR32_MODEM_DCCOMP,               (EFR32_MODEM_BASE + 0x0e0)
.equ EFR32_MODEM_DCCOMP_DCCOMPFREEZE,  (1 << 3)
.equ EFR32_MODEM_DCESTI,               (EFR32_MODEM_BASE + 0x0e8)
.equ EFR32_MODEM_DCESTI_Q_SHIFT,       15
.equ EFR32_SEQ_DCCAL_ISOLATE_US,       16
.equ EFR32_SEQ_DCCAL_SETTLE_US,        8
.equ EFR32_SEQ_DCCAL_FREEZE_US,        1

.thumb_func
efr32_seq_pga_dc_measure:
    /* r0 = PGACAL setting; returns MODEM.DCESTI in r0.  Clobbers r0-r3. */
    push {lr}
    ldr r1, =EFR32_RAC_PGACAL
    str r0, [r1]
    ldr r0, =(EFR32_RAC_IFPGACTRL + EFR32_ALIAS_CLR)
    ldr r1, =EFR32_RAC_IFPGACTRL_DCRSTEN
    str r1, [r0]
    movs r0, #EFR32_SEQ_DCCAL_SETTLE_US
    bl efr32_seq_delay
    ldr r0, =(EFR32_MODEM_DCCOMP + EFR32_ALIAS_SET)
    movs r1, #EFR32_MODEM_DCCOMP_DCCOMPFREEZE
    str r1, [r0]
    movs r0, #EFR32_SEQ_DCCAL_FREEZE_US
    bl efr32_seq_delay
    ldr r0, =EFR32_MODEM_DCESTI
    ldr r2, [r0]
    ldr r0, =(EFR32_MODEM_DCCOMP + EFR32_ALIAS_CLR)
    movs r1, #EFR32_MODEM_DCCOMP_DCCOMPFREEZE
    str r1, [r0]
    mov r0, r2
    pop {pc}

.thumb_func
efr32_seq_pga_dc_cal:
    push {r4-r7, lr}
    ldr r0, =(EFR32_AGC_CTRL0 + EFR32_ALIAS_CLR)
    ldr r1, =EFR32_AGC_CTRL0_ADCATTENCODE_MASK
    str r1, [r0]
    ldr r0, =EFR32_RAC_AGCOVERWRITE
    ldr r1, =EFR32_RAC_AGCOVERWRITE_DCCAL
    str r1, [r0]
    ldr r0, =(EFR32_RAC_RFPATHEN + EFR32_ALIAS_SET)
    movs r1, #EFR32_RAC_RFPATHEN_LNAMIXTRSW
    str r1, [r0]
    movs r0, #EFR32_SEQ_DCCAL_ISOLATE_US
    bl efr32_seq_delay
    ldr r0, =(EFR32_RAC_IFPGACTRL + EFR32_ALIAS_CLR)
    ldr r1, =(EFR32_RAC_IFPGACTRL_DCRSTEN | EFR32_RAC_IFPGACTRL_DCESTIEN | EFR32_RAC_IFPGACTRL_DCCALDEC0_MASK | EFR32_RAC_IFPGACTRL_DCCALDCGEAR_MASK)
    str r1, [r0]
    ldr r0, =(EFR32_RAC_IFPGACTRL + EFR32_ALIAS_SET)
    ldr r1, =(EFR32_RAC_IFPGACTRL_DCCALON | EFR32_RAC_IFPGACTRL_DCRSTEN | EFR32_RAC_IFPGACTRL_DCESTIEN | EFR32_RAC_IFPGACTRL_DCCALDCGEAR_MASK)
    str r1, [r0]
    ldr r0, =EFR32_MODEM_DCCOMP
    ldr r7, [r0]                     /* restored afterwards */
    ldr r0, =EFR32_RAC_PGACAL_POINT_A
    bl efr32_seq_pga_dc_measure
    mov r4, r0                       /* estimate at point A */
    ldr r0, =EFR32_RAC_PGACAL_POINT_B
    bl efr32_seq_pga_dc_measure
    mov r5, r0                       /* estimate at point B */
    ldr r0, =EFR32_RAC_PGACAL
    ldr r1, =EFR32_RAC_PGACAL_MIDSCALE
    str r1, [r0]
    mov r0, r5
    mov r1, r4
    bl efr32_seq_pga_dc_interp       /* I channel */
    movs r1, #EFR32_RAC_PGACAL_CODE_MASK
    ands r0, r1
    lsls r6, r0, #16                 /* PGAOFFPCALI */
    lsrs r0, r5, #EFR32_MODEM_DCESTI_Q_SHIFT
    lsrs r1, r4, #EFR32_MODEM_DCESTI_Q_SHIFT
    bl efr32_seq_pga_dc_interp       /* Q channel */
    movs r1, #EFR32_RAC_PGACAL_CODE_MASK
    ands r0, r1
    lsls r0, r0, #24                 /* PGAOFFPCALQ */
    orrs r6, r0
    lsrs r0, r6, #16                 /* P codes moved to the N positions */
    ldr r1, =EFR32_RAC_PGACAL_N_FIELDS
    eors r0, r1
    ldr r2, =EFR32_RAC_PGACAL_N_NEGATE_ONE
    adds r0, r0, r2                  /* N = -P, per 6-bit field */
    ands r0, r1
    orrs r0, r6
    ldr r1, =EFR32_RAC_PGACAL
    str r0, [r1]
    ldr r0, =EFR32_MODEM_DCCOMP
    str r7, [r0]
    ldr r0, =(EFR32_RAC_IFPGACTRL + EFR32_ALIAS_CLR)
    ldr r1, =EFR32_RAC_IFPGACTRL_DCCALON
    str r1, [r0]
    ldr r0, =EFR32_MODEM_DCCOMP
    ldr r1, [r0]
    str r1, [r0]
    ldr r0, =(EFR32_RAC_RFPATHEN + EFR32_ALIAS_CLR)
    movs r1, #EFR32_RAC_RFPATHEN_LNAMIXTRSW
    str r1, [r0]
    ldr r0, =(EFR32_RAC_AGCOVERWRITE + EFR32_ALIAS_CLR)
    ldr r1, =EFR32_RAC_AGCOVERWRITE_DCCAL_RELEASE
    str r1, [r0]
    pop {r4-r7, pc}

/*
 * Stock fn_0x2308, lifted: r0 = DC estimate at point B, r1 = at point A, each
 * a signed 15-bit value in bits [14:0].  Returns the 6-bit P code at which
 * the linear interpolation between the two crosses zero.
 */
.equ EFR32_SEQ_DCEST_SEXT_UP,          17 /* bit 14 to bit 31 */
.equ EFR32_SEQ_DCEST_SEXT_DOWN,        3  /* keep 14 fractional bits */
.equ EFR32_SEQ_DCCAL_TOP_CODE_BIT,     32

.thumb_func
efr32_seq_pga_dc_interp:
    push {r4, lr}
    lsls r1, r1, #EFR32_SEQ_DCEST_SEXT_UP
    asrs r2, r1, #EFR32_SEQ_DCEST_SEXT_DOWN   /* a */
    lsls r0, r0, #EFR32_SEQ_DCEST_SEXT_UP
    asrs r3, r0, #EFR32_SEQ_DCEST_SEXT_DOWN   /* b */
    subs r1, r3, r2                           /* step = b - a */
    movs r0, #0
    adds r2, r3, r2
    lsrs r3, r2, #31
    adds r2, r3, r2
    asrs r2, r2, #1                           /* (a + b) / 2, toward zero */
    adds r2, r2, r1
    asrs r3, r1, #6
    lsrs r3, r3, #25
    adds r3, r3, r1
    asrs r3, r3, #7                           /* step / 128, toward zero */
    adds r2, r2, r3
    movs r3, #EFR32_SEQ_DCCAL_TOP_CODE_BIT
1:  subs r4, r2, r1
    bmi 2f
    mov r2, r4
    orrs r0, r3
2:  lsrs r4, r1, #31
    adds r1, r4, r1
    asrs r1, r1, #1                           /* step / 2, toward zero */
    lsrs r3, r3, #1
    bne 1b
    pop {r4, pc}
.ltorg

/* TX synthesizer configuration, shared by the cold TXWARM start and the
 * RX->TX retune (stock uses the same block for both, SS7.61/SS7.65).
 * Clobbers r0-r1. */
.thumb_func
efr32_seq_synth_config_tx:
    ldr r0, =EFR32_SYNTH_VCORANGE
    ldr r1, =EFR32_SYNTH_VCORANGE_TX
    str r1, [r0]
    ldr r0, =EFR32_SYNTH_CMD
    movs r1, #(EFR32_SYNTH_CMD_SYNTHSTOP | EFR32_SYNTH_CMD_DISABLEIF)
    str r1, [r0]
    ldr r0, =(EFR32_RAC_SYNTHENCTRL + EFR32_ALIAS_SET)
    ldr r1, =EFR32_RAC_SYNTHENCTRL_LPFBWSEL
    str r1, [r0]
    ldr r0, =EFR32_RAC_SYMMDCTRL
    ldr r1, =EFR32_RAC_SYMMDCTRL_TX
    str r1, [r0]
    ldr r0, =EFR32_RAC_RX
    movs r1, #EFR32_RAC_RX_SYCHPBIASTRIMBUF
    str r1, [r0]
    ldr r0, =(EFR32_RAC_SYEN + EFR32_ALIAS_CLR)
    movs r1, #EFR32_RAC_SYEN_SYCHPLPEN
    str r1, [r0]
    ldr r0, =(EFR32_RAC_CHPCTRL + EFR32_ALIAS_SET)
    ldr r1, =EFR32_RAC_CHPCTRL_CHPOUTPUTTRISTATEMODE
    str r1, [r0]
    ldr r0, =EFR32_RAC_SYTRIM0
    ldr r1, =EFR32_RAC_SYTRIM0_TX
    str r1, [r0]
    ldr r0, =EFR32_RAC_VCOCTRL
    movs r1, #EFR32_RAC_VCOCTRL_TX
    str r1, [r0]
    ldr r0, =(EFR32_RAC_XORETIMECTRL + EFR32_ALIAS_CLR)
    movs r1, #EFR32_RAC_RETIME_ENRETIME
    str r1, [r0]
    ldr r0, =(EFR32_RAC_DIGCLKRETIMECTRL + EFR32_ALIAS_CLR)
    str r1, [r0]
    ldr r0, =EFR32_RAC_SYTRIM1
    ldr r1, =EFR32_RAC_SYTRIM1_STARTUP
    str r1, [r0]
    ldr r0, =(EFR32_RAC_SYNTHENCTRL + EFR32_ALIAS_SET)
    ldr r1, =EFR32_RAC_SYNTHENCTRL_SYNTHSTARTREQ
    str r1, [r0]
    bx lr

.ltorg

/*
 * Stock's RF front-end power-down (fn_0xc02, SS7.65), offsets now named from
 * the public RAC_S model.  Stock first clears the retime enables when SR0
 * bit 0 is clear; this image's other paths already clear them.
 */
.equ EFR32_RAC_RX_FRONTEND_OFF, (EFR32_RAC_RX_IFADCENLDOSERIES | EFR32_RAC_RX_IFADCENLDOSHUNT | \
    (1 << 3) /* LNAMIXENRFPKD */ | EFR32_RAC_RX_LNAMIXLDOLOWCUR | EFR32_RAC_RX_PGAENLDO | \
    EFR32_RAC_RX_SYPFDCHPLPEN | EFR32_RAC_RX_SYPFDFPWEN)
/* PGAENBIAS, PGAENGHZ, PGAENPGAI, PGAENPGAQ, PGAENPKD, PGAENRCMOUT. */
.equ EFR32_RAC_PGACTRL_FRONTEND_OFF, ((1 << 2) | (1 << 3) | (1 << 9) | (1 << 10) | (1 << 11) | (1 << 12))
.equ EFR32_RAC_IFADCDBG_FRONTEND_OFF, (EFR32_RAC_IFADCDBG_IFADCENCLK | (1 << 0)) /* + IFADCENADC */
.equ EFR32_RAC_CMD_LNAENCLEAR,        (1 << 15)

.thumb_func
efr32_seq_rx_frontend_stop:
    ldr r0, =(EFR32_RAC_RX + EFR32_ALIAS_CLR)
    ldr r1, =EFR32_RAC_RX_FRONTEND_OFF
    str r1, [r0]
    ldr r0, =(EFR32_RAC_PGACTRL + EFR32_ALIAS_CLR)
    ldr r1, =EFR32_RAC_PGACTRL_FRONTEND_OFF
    str r1, [r0]
    ldr r0, =(EFR32_RAC_SYLOEN + EFR32_ALIAS_CLR)
    movs r1, #EFR32_RAC_SYLOEN_RX
    str r1, [r0]
    ldr r0, =EFR32_RAC_RFPATHEN
    movs r1, #0
    str r1, [r0]
    ldr r0, =(EFR32_RAC_IFADCDBG + EFR32_ALIAS_CLR)
    movs r1, #EFR32_RAC_IFADCDBG_FRONTEND_OFF
    str r1, [r0]
    ldr r0, =(EFR32_RAC_LNAMIXEN + EFR32_ALIAS_CLR)
    movs r1, #EFR32_RAC_LNAMIXEN_LNAMIXENLDO
    str r1, [r0]
    ldr r0, =EFR32_RAC_CMD
    ldr r1, =EFR32_RAC_CMD_LNAENCLEAR
    str r1, [r0]
    bx lr

.ltorg

/* Open ABI word: the M33 writes PENDING when it arms RAC.CMD.TXAFTERFRAME,
 * then RESPOND after publishing the response to BUFC or CANCEL when the LLL
 * declines (SS7.65). */
.equ EFR32_SEQ_RX2TX_DECISION,        0x00004fa4
.equ EFR32_SEQ_RX2TX_PENDING,         3 /* 0 = idle: SEQRAM reset value */
.equ EFR32_SEQ_RX2TX_RESPOND,         1
.equ EFR32_SEQ_RX2TX_CANCEL,          2
/* Bound in ~1 us steps; the response must start within the 150 us tIFS. */
.equ EFR32_SEQ_RX2TX_DECISION_WAIT_US, 120
.equ EFR32_RAC_CMD_TXDIS,             (1 << 5)
.equ EFR32_RAC_CMD_TXEN_BIT,          (1 << 0)

/*
 * RX->TX (IRQ7, SS7.65).  A full responding stock IRQ7 cannot be traced (its
 * deadline expires under single-stepping and RAIL cancels), but the captured
 * part is the TXWARM configuration block and synth-lock routine with the
 * supplies left on; the front-end power-down and PA start are [I] taken from
 * the RXPD and TXWARM paths.
 */
.thumb_func
efr32_seq_tx_turnaround:
    push {r4, r5, lr}
    movs r0, #0
    bl efr32_seq_trace_stimer
    /* A connection receive with its reply staged (seq_abi.h, SS7.82): decide
     * here, at the frame's end, as the link layer would; the M33 commits the
     * same outcome later.  r5: 0 the M33 decides (decision word), 1 send the
     * staged reply, 2 send nothing. */
    movs r5, #0
    ldr r0, =EFR32_SEQ_RX2TX_DECISION
    ldr r0, [r0]
    cmp r0, #EFR32_SEQ_RX2TX_STAGED
    bne 12f
    bl efr32_seq_conn_decide
    movs r5, #2
    subs r5, r5, r0
12:
    /* The RAC holds RX2TX until this handler acknowledges, so retune for
     * TX while the M33 decides; the PA comes up only on RESPOND, so nothing
     * radiates for a declined frame (SS7.67). */
    bl efr32_seq_rx_frontend_stop
    /* After a TX->RX turnaround, and after a window opened from OFF (RXWARM brings the
     * synthesizer up as TXWARM does and caches this channel's TX calibration, SS7.80), the
     * synthesizer is up: retune with the cached calibration.  Should it be down (RADIOEN
     * zero, as efr32_seq_synth_power_down leaves it, e.g. the RXWARM calibration aborted),
     * bring it up and calibrate it as TXWARM does, PA still off. */
    ldr r0, =EFR32_RAC_RADIOEN
    ldr r0, [r0]
    movs r1, #EFR32_RAC_RADIOEN_PREEN
    tst r0, r1
    bne 10f
    bl efr32_seq_synth_power_up
    b 11f
10: bl efr32_seq_synth_config_tx
    movs r0, #EFR32_SEQ_SYNTH_LOCK_CACHED
    bl efr32_seq_synth_lock
11: movs r0, #2
    bl efr32_seq_trace_stimer
    /* A staged reply was decided above, by the link layer's rules. */
    cmp r5, #1
    beq 2f
    cmp r5, #2
    beq 4f
    /* Otherwise wait (bounded) for the M33's decision on this received
     * frame: it either published a response or cancelled; this core never
     * chooses. */
    ldr r4, =EFR32_SEQ_RX2TX_DECISION_WAIT_US
1:  ldr r0, =EFR32_SEQ_RX2TX_DECISION
    ldr r0, [r0]
    cmp r0, #EFR32_SEQ_RX2TX_RESPOND
    beq 2f
    cmp r0, #EFR32_SEQ_RX2TX_CANCEL
    beq 3f
    movs r0, #1
    bl efr32_seq_delay
    subs r4, #1
    bne 1b
    /* No decision in time: drop the response rather than send stale data. */
    movs r0, #EFR32_SEQ_TRACE_RX2TX_TIMEOUT_SLOT
    bl efr32_seq_trace_count
    b 4f
3:  movs r0, #EFR32_SEQ_TRACE_RX2TX_CANCEL_SLOT
    bl efr32_seq_trace_count
    /* Declined: stop the TX the RXFRAME request set up, as stock does. */
4:  ldr r0, =EFR32_RAC_CMD
    movs r1, #EFR32_RAC_CMD_TXDIS
    str r1, [r0]
    b 9f
2:  movs r0, #EFR32_SEQ_TRACE_RX2TX_RESPOND_SLOT
    bl efr32_seq_trace_count
    movs r0, #1
    bl efr32_seq_trace_stimer
    ldr r0, =EFR32_SEQ_TRACE_RESPONSE_PENDING
    movs r1, #1
    str r1, [r0]
    bl efr32_seq_pa_start
    movs r0, #3
    bl efr32_seq_trace_stimer
    /* The RAC enters TX ~3 us after this handler acknowledges STATERX2TX
     * (measured, SS7.68), and RAC.STIMER restarted when RX2TX began, i.e.
     * at the received frame's end.  Hold the acknowledgement so TX starts
     * near T_IFS rather than ~75 us early. */
    ldr r0, =EFR32_SEQ_RX2TX_TX_AT_STIMER
    ldr r1, =EFR32_RAC_STIMER
5:  ldr r2, [r1]
    cmp r2, r0
    blo 5b
9:  pop {r4, r5, pc}
.ltorg

/* Diagnostic only: bump byte r0 of the per-IRQ count area (slots 24-26 are
 * the RX2TX decision outcomes).  Clobbers r0-r1. */
.equ EFR32_SEQ_TRACE_RX2TX_RESPOND_SLOT, 25
.equ EFR32_SEQ_TRACE_RX2TX_CANCEL_SLOT,  26
.equ EFR32_SEQ_TRACE_RX2TX_TIMEOUT_SLOT, 27
.thumb_func
efr32_seq_trace_count:
    ldr r1, =EFR32_SEQ_TRACE_IRQ_COUNTS
    adds r1, r1, r0
    ldrb r0, [r1]
    adds r0, #1
    strb r0, [r1]
    bx lr
.ltorg

/* Diagnostic only: RAC.STIMER into word r0 of the RX2TX timing record
 * (entry, decision, locked, PA up).  Clobbers r0-r1. */
.equ EFR32_SEQ_TRACE_RX2TX_TIMES,     0x00004f40
/* The RX2TX hold (EFR32_SEQ_RX2TX_TX_AT_STIMER) and the RAC.STIMER rate are in
 * include/hal/efr32/seq_abi.h, shared with the M33 HAL, which derives its
 * RESPOND deadline from them.  The hold is matched to the stock stack on
 * silicon (SS7.71); the chain delays from MODEM EOF and SYNCSENT to the
 * antenna are the same silicon in both, so matching stock's interval
 * matches its on-air T_IFS. */
.equ EFR32_SEQ_TRACE_RESPONSE_PENDING, 0x00004f34 /* diagnostic only */
.equ EFR32_SEQ_TRACE_RESPONSE_TX_AT,   0x00004f38 /* diagnostic only */
.equ EFR32_RAC_STIMER,                0xa8020030
.equ EFR32_SYSTICK_CVR,               0xe000e018
.thumb_func
efr32_seq_trace_stimer:
    lsls r0, r0, #2
    ldr r1, =EFR32_SEQ_TRACE_RX2TX_TIMES
    adds r0, r0, r1
    ldr r1, =EFR32_RAC_STIMER
    ldr r1, [r1]
    str r1, [r0]
    /* Same instant on this core's free-running SysTick (counts down),
     * 0x20 bytes below: STIMER restarts on every RAC state change. */
    ldr r1, =EFR32_SYSTICK_CVR
    ldr r1, [r1]
    subs r0, #0x20
    str r1, [r0]
    bx lr
.ltorg

/*
 * Timed command (seq_abi.h, docs/radio-tables.md SS7.82).  The M33 has done
 * every preparation (FIFO, packet stage, SR0 handshake) and posted one command
 * with its instant as a TIMER0 count; this core waits for the instant and
 * issues it, so no M33 interrupt can delay it.  The wait polls TIMER0.CNT
 * (signed difference, wrap-safe) and the OP word, which the M33 clears to
 * cancel; past LIMIT nothing is issued.  Called from the IRQ 23 entry.
 */
.equ EFR32_SEQ_RAC_RXENSRCEN_SET,     (EFR32_RAC_RXENSRCEN + EFR32_ALIAS_SET)
.thumb_func
efr32_seq_timed_command:
    ldr r0, =(EFR32_SEQ_RDMAILBOX1_BASE + EFR32_SEQ_RDMAILBOX_FLAGS + EFR32_ALIAS_CLR)
    movs r1, #EFR32_SEQ_RDMAILBOX_TIMED_FLAG
    str r1, [r0]
    ldr r0, =EFR32_SEQ_TIMED_BLOCK
    ldr r1, [r0, #EFR32_SEQ_TIMED_OP]
    movs r2, #EFR32_SEQ_TIMED_OP_MASK
    tst r1, r2
    beq 9f
    push {r4, lr}
    ldr r2, [r0, #EFR32_SEQ_TIMED_TARGET]
    ldr r3, =EFR32_SEQ_TIMER0_CNT_NS
    ldr r4, [r3]
    str r4, [r0, #EFR32_SEQ_TIMED_ENTRY]
    movs r4, #EFR32_SEQ_TIMED_ENTRY_T1
    bl efr32_seq_timed_stamp_t1
1:  ldr r4, [r0, #EFR32_SEQ_TIMED_OP]
    cmp r4, r1
    bne 7f
    ldr r4, [r3]
    subs r4, r4, r2
    bmi 1b
    /* At or past the instant: issue the command only up to LIMIT.  Late is
     * LIMIT - CNT negative, the N flag alone: bgt on CNT - LIMIT also reads
     * V, which a difference across 0x80000000 sets. */
    ldr r2, [r0, #EFR32_SEQ_TIMED_LIMIT]
    ldr r4, [r3]
    subs r4, r2, r4
    bmi 6f
    movs r2, #EFR32_SEQ_TIMED_OP_MASK
    ands r2, r1
    cmp r2, #EFR32_SEQ_TIMED_OP_TXEN
    bne 2f
    ldr r2, =EFR32_RAC_CMD
    movs r4, #EFR32_RAC_CMD_TXEN_BIT
    str r4, [r2]
    ldr r4, [r3]
    str r4, [r0, #EFR32_SEQ_TIMED_AT]
    movs r4, #EFR32_SEQ_TIMED_AT_T1
    bl efr32_seq_timed_stamp_t1
    /* A TX that turns to RX raises its RX request right after TXEN, as the
     * M33 did before (SS7.63). */
    ldr r2, =EFR32_SEQ_TIMED_RX_AFTER_TX
    tst r1, r2
    beq 5f
    ldr r2, =EFR32_SEQ_RAC_RXENSRCEN_SET
    movs r4, #EFR32_RAC_RXENSRCEN_SWRXEN_BIT
    str r4, [r2]
    b 5f
2:  cmp r2, #EFR32_SEQ_TIMED_OP_SWRXEN
    bne 5f
    ldr r2, =EFR32_SEQ_RAC_RXENSRCEN_SET
    movs r4, #EFR32_RAC_RXENSRCEN_SWRXEN_BIT
    str r4, [r2]
    ldr r4, [r3]
    str r4, [r0, #EFR32_SEQ_TIMED_AT]
    movs r4, #EFR32_SEQ_TIMED_AT_T1
    bl efr32_seq_timed_stamp_t1
5:  str r1, [r0, #EFR32_SEQ_TIMED_DONE]
    b 8f
6:  ldr r2, =EFR32_SEQ_TIMED_LATE
    b 4f
7:  ldr r2, =EFR32_SEQ_TIMED_CANCELLED
4:  orrs r2, r1
    str r2, [r0, #EFR32_SEQ_TIMED_DONE]
8:  pop {r4, pc}
9:  bx lr
.ltorg

/* Diagnostic: TIMER1.CNT into timed-block word r4 (r0 the block) while TIMER1's bus clock is
 * on; a gated TIMER1 read would fault.  Preserves r0-r3. */
.thumb_func
efr32_seq_timed_stamp_t1:
    push {r1, r2}
    ldr r1, =EFR32_SEQ_CMU_CLKEN0_NS
    ldr r1, [r1]
    ldr r2, =EFR32_SEQ_CMU_CLKEN0_TIMER1
    tst r1, r2
    beq 1f
    ldr r1, =EFR32_SEQ_TIMER1_CNT_NS
    ldr r1, [r1]
    str r1, [r0, r4]
1:  pop {r1, r2}
    bx lr
.ltorg

/*
 * Staged connection reply (seq_abi.h, docs/radio-tables.md SS7.82).  Takes the
 * stage (bounded wait, as for the M33's decision), reads the received header
 * and the CRC trailer from the RX FIFO, decides as lll_conn.c's isr_rx() does,
 * publishes OUTCOME and, when a reply goes out, writes it to the BUFC TX FIFO
 * with its SN and NESN; when the event continues it arms the next receive.
 * Returns r0 = 1 to send the reply, 0 to send nothing.  Clobbers r0-r3.
 */
.equ EFR32_SEQ_CONN_PDU_SEQ_BITS, ((1 << EFR32_SEQ_PDU_SN_BIT) | (1 << EFR32_SEQ_PDU_NESN_BIT))
.equ EFR32_SEQ_CONN_PDU_MD,       (1 << EFR32_SEQ_PDU_MD_BIT)
.thumb_func
efr32_seq_conn_decide:
    push {r4-r7, lr}
    ldr r7, =EFR32_SEQ_CONN_BLOCK
    ldr r4, =EFR32_SEQ_RX2TX_DECISION_WAIT_US
1:  ldr r0, [r7, #EFR32_SEQ_CONN_STATE]
    movs r1, #EFR32_SEQ_CONN_STAGED
    tst r0, r1
    bne 2f
    movs r0, #1
    bl efr32_seq_delay
    subs r4, #1
    bne 1b
    /* Not staged in time: nothing is sent; the M33 finds no outcome. */
    movs r0, #EFR32_SEQ_TRACE_RX2TX_TIMEOUT_SLOT
    bl efr32_seq_trace_count
    movs r0, #0
    pop {r4-r7, pc}
2:  lsrs r6, r0, #EFR32_SEQ_CONN_GENERATION_SHIFT
    lsls r6, r6, #EFR32_SEQ_CONN_GENERATION_SHIFT
    movs r0, #0
    str r0, [r7, #EFR32_SEQ_CONN_STATE]
    ldr r0, =EFR32_RAC_STIMER
    ldr r0, [r0]
    str r0, [r7, #EFR32_SEQ_CONN_TAKEN_AT]
    ldr r5, [r7, #EFR32_SEQ_CONN_FLAGS]
    /* Header, length and trailer octets of the frame, the FIFO wrapping. */
    ldr r0, [r7, #EFR32_SEQ_CONN_RX_BASE]
    ldr r1, [r7, #EFR32_SEQ_CONN_RX_OFFSET]
    ldr r2, [r7, #EFR32_SEQ_CONN_RX_MASK]
    mov r3, r1
    ands r3, r2
    ldrb r4, [r0, r3]
    adds r3, r1, #1
    ands r3, r2
    ldrb r3, [r0, r3]
    adds r1, r1, #2
    adds r1, r1, r3
    ands r1, r2
    ldrb r1, [r0, r1]
    ldr r2, [r7, #EFR32_SEQ_CONN_RX_MAX]
    cmp r3, r2
    bhi 3f
    movs r2, #EFR32_SEQ_FRC_TRAILER_CRCOK
    tst r1, r2
    beq 3f
    movs r0, #EFR32_SEQ_CONN_OUT_CRC_OK
    orrs r6, r0
    movs r0, #EFR32_SEQ_CONN_SCAN
    tst r5, r0
    beq 21f
    b .Lscan_answer
21: movs r0, #EFR32_SEQ_CONN_ADV
    tst r5, r0
    beq 15f
    bl efr32_seq_adv_scan_req_match
    cmp r0, #0
    bne 16f
    b 14f
3:  movs r0, #EFR32_SEQ_CONN_SCAN
    tst r5, r0
    beq 22f
    b .Lscan_silent
22: movs r0, #EFR32_SEQ_CONN_ADV
    tst r5, r0
    beq 17f
    /* An advertiser answers nothing but a matching SCAN_REQ; the event then ends. */
14: movs r0, #(EFR32_SEQ_CONN_OUT_VALID | EFR32_SEQ_CONN_OUT_DONE)
    orrs r6, r0
    str r6, [r7, #EFR32_SEQ_CONN_OUTCOME]
    movs r0, #0
    pop {r4-r7, pc}
16: movs r0, #(EFR32_SEQ_CONN_OUT_VALID | EFR32_SEQ_CONN_OUT_DONE | EFR32_SEQ_CONN_OUT_REPLY)
    orrs r6, r0
    str r6, [r7, #EFR32_SEQ_CONN_OUTCOME]
    ldr r2, [r7, #EFR32_SEQ_CONN_CUR]
    ldr r3, [r7, #EFR32_SEQ_CONN_CUR_OCTETS]
    ldrb r4, [r2]
    b 18f
15:
    /* Acknowledged: received NESN differs from our SN. */
    lsrs r0, r4, #EFR32_SEQ_PDU_NESN_BIT
    lsrs r1, r5, #EFR32_SEQ_CONN_SN_SHIFT
    eors r0, r1
    movs r1, #1
    tst r0, r1
    beq 4f
    movs r0, #EFR32_SEQ_CONN_OUT_ACK
    orrs r6, r0
4:  /* New data: received SN equals our NESN, and a buffer is free. */
    lsrs r0, r4, #EFR32_SEQ_PDU_SN_BIT
    lsrs r2, r5, #EFR32_SEQ_CONN_NESN_SHIFT
    eors r0, r2
    tst r0, r1
    bne 6f
    movs r0, #EFR32_SEQ_CONN_RX_FREE
    tst r5, r0
    beq 6f
    movs r0, #EFR32_SEQ_CONN_OUT_ACCEPT
    orrs r6, r0
    b 6f
17: /* CRC failed (a frame too long counts as one): the second in a row
     * closes the event; the received MD is not used. */
    movs r0, #EFR32_SEQ_CONN_CRC_CLOSES
    tst r5, r0
    beq 7f
    movs r0, #EFR32_SEQ_CONN_OUT_DONE
    orrs r6, r0
7:  movs r4, #EFR32_SEQ_CONN_PDU_MD
6:  ldr r2, [r7, #EFR32_SEQ_CONN_CUR]
    ldr r3, [r7, #EFR32_SEQ_CONN_CUR_OCTETS]
    movs r0, #EFR32_SEQ_CONN_OUT_ACK
    tst r6, r0
    beq 8f
    ldr r2, [r7, #EFR32_SEQ_CONN_NEXT]
    ldr r3, [r7, #EFR32_SEQ_CONN_NEXT_OCTETS]
8:  /* Closed by a valid frame without MD answered by an empty PDU. */
    movs r0, #EFR32_SEQ_CONN_OUT_CRC_OK
    tst r6, r0
    beq 9f
    movs r0, #EFR32_SEQ_CONN_PDU_MD
    tst r4, r0
    bne 9f
    ldrb r0, [r2, #1]
    cmp r0, #0
    bne 9f
    movs r0, #EFR32_SEQ_CONN_OUT_DONE
    orrs r6, r0
9:  movs r0, #EFR32_SEQ_CONN_OUT_VALID
    orrs r6, r0
    /* A Central that is done sends nothing. */
    movs r0, #EFR32_SEQ_CONN_OUT_DONE
    tst r6, r0
    beq 10f
    movs r0, #EFR32_SEQ_CONN_CENTRAL
    tst r5, r0
    beq 10f
    str r6, [r7, #EFR32_SEQ_CONN_OUTCOME]
    movs r0, #0
    pop {r4-r7, pc}
10: movs r0, #EFR32_SEQ_CONN_OUT_REPLY
    orrs r6, r0
    str r6, [r7, #EFR32_SEQ_CONN_OUTCOME]
    /* Reply header: SN ^ ack, NESN ^ accept. */
    ldrb r4, [r2]
    movs r0, #EFR32_SEQ_CONN_PDU_SEQ_BITS
    bics r4, r0
    movs r1, #1
    lsrs r0, r5, #EFR32_SEQ_CONN_SN_SHIFT
    lsrs r7, r6, #EFR32_SEQ_CONN_OUT_ACK_SHIFT
    eors r0, r7
    ands r0, r1
    lsls r0, r0, #EFR32_SEQ_PDU_SN_BIT
    orrs r4, r0
    lsrs r0, r5, #EFR32_SEQ_CONN_NESN_SHIFT
    lsrs r7, r6, #EFR32_SEQ_CONN_OUT_ACCEPT_SHIFT
    eors r0, r7
    ands r0, r1
    lsls r0, r0, #EFR32_SEQ_PDU_NESN_BIT
    orrs r4, r0
    /* FRC frames the reply from its length octet: restore the length-pending
     * word count the M33's radio_pkt_configure() sets before every TX (a
     * received frame left its own there). */
18: ldr r0, =EFR32_SEQ_FRC_WCNTCMP0
    movs r1, #EFR32_SEQ_FRC_WCNTCMP0_LENGTH_PENDING
    str r1, [r0]
    /* Into the BUFC TX FIFO the M33 configured: CLEAR, then every octet. */
    ldr r0, =EFR32_SEQ_BUFC_TX_CMD
    movs r1, #EFR32_SEQ_BUFC_CMD_CLEAR
    str r1, [r0]
    ldr r0, =EFR32_SEQ_BUFC_TX_WRITEDATA
    str r4, [r0]
    movs r1, #1
11: cmp r1, r3
    bhs 12f
    ldrb r4, [r2, r1]
    str r4, [r0]
    adds r1, #1
    b 11b
12: /* The event continues: the next receive is armed and staged, as after a
     * launched TX (efr32_radio_tx_start(), SS7.63). */
    movs r0, #EFR32_SEQ_CONN_OUT_DONE
    tst r6, r0
    bne 13f
    ldr r0, =EFR32_SEQ_RX2TX_DECISION
    movs r1, #EFR32_SEQ_RX2TX_STAGED
    str r1, [r0]
    ldr r0, =(EFR32_RAC_RXENSRCEN + EFR32_ALIAS_SET)
    movs r1, #EFR32_RAC_RXENSRCEN_SWRXEN_BIT
    str r1, [r0]
13: movs r0, #1
    pop {r4-r7, pc}
.Lscan_answer:
    /* A scanner's or initiator's receive (seq_abi.h EFR32_SEQ_CONN_SCAN). */
    bl efr32_seq_scan_match
    cmp r0, #0
    beq .Lscan_silent
    ldr r0, =EFR32_SEQ_CONN_INIT
    tst r5, r0
    beq 23f
    /* The CONNECT_IND goes out unchanged and ends the event. */
    b 16b
23: /* The SCAN_REQ: RxAdd the frame's TxAdd, AdvA the frame's; the SCAN_RSP
     * receive follows it. */
    movs r0, #(EFR32_SEQ_CONN_OUT_VALID | EFR32_SEQ_CONN_OUT_REPLY)
    orrs r6, r0
    str r6, [r7, #EFR32_SEQ_CONN_OUTCOME]
    ldr r2, [r7, #EFR32_SEQ_CONN_CUR]
    ldrb r0, [r2]
    movs r1, #(1 << EFR32_SEQ_ADV_RXADD_BIT)
    bics r0, r1
    lsrs r1, r4, #EFR32_SEQ_ADV_TXADD_BIT
    movs r3, #1
    ands r1, r3
    lsls r1, r1, #EFR32_SEQ_ADV_RXADD_BIT
    orrs r0, r1
    mov r4, r0
    ldr r0, =EFR32_SEQ_FRC_WCNTCMP0
    movs r1, #EFR32_SEQ_FRC_WCNTCMP0_LENGTH_PENDING
    str r1, [r0]
    ldr r0, =EFR32_SEQ_BUFC_TX_CMD
    movs r1, #EFR32_SEQ_BUFC_CMD_CLEAR
    str r1, [r0]
    ldr r0, =EFR32_SEQ_BUFC_TX_WRITEDATA
    str r4, [r0]
    movs r1, #1
1:  ldrb r4, [r2, r1]
    str r4, [r0]
    adds r1, #1
    cmp r1, #EFR32_SEQ_SCAN_REQ_ADVA_OCTET
    blo 1b
    /* AdvA: the frame's payload octets 0-5; the FIFO wraps. */
    ldr r2, [r7, #EFR32_SEQ_CONN_RX_BASE]
    ldr r3, [r7, #EFR32_SEQ_CONN_RX_MASK]
    ldr r5, [r7, #EFR32_SEQ_CONN_RX_OFFSET]
    adds r5, #2
    movs r1, #0
2:  adds r4, r5, r1
    ands r4, r3
    ldrb r4, [r2, r4]
    str r4, [r0]
    adds r1, #1
    cmp r1, #EFR32_SEQ_ADV_ADDR_LEN
    blo 2b
    b 12b
.Lscan_silent:
    /* Nothing is sent; the scanner listens on. */
    movs r0, #EFR32_SEQ_CONN_OUT_VALID
    orrs r6, r0
    str r6, [r7, #EFR32_SEQ_CONN_OUTCOME]
    movs r0, #0
    pop {r4-r7, pc}
.ltorg

/*
 * A SCAN_REQ for this advertiser (seq_abi.h EFR32_SEQ_CONN_ADV): type, length,
 * RxAdd and AdvA, as isr_rx_pdu()'s isr_rx_sr_adva_check().  r7 the stage
 * block, r4 the received header octet, r3 its length, r5 FLAGS.  Returns r0 = 1
 * on a match.  Clobbers r0-r3; preserves r4-r7.
 */
.thumb_func
efr32_seq_adv_scan_req_match:
    push {r4-r6, lr}
    movs r0, #EFR32_SEQ_ADV_TYPE_MASK
    ands r0, r4
    cmp r0, #EFR32_SEQ_ADV_SCAN_REQ_TYPE
    bne 8f
    cmp r3, #EFR32_SEQ_ADV_SCAN_REQ_LEN
    bne 8f
    /* RxAdd of the request is the advertiser's TxAdd. */
    lsrs r0, r4, #EFR32_SEQ_ADV_RXADD_BIT
    movs r1, #1
    ands r0, r1
    movs r1, #EFR32_SEQ_CONN_ADV_TXADD
    ands r1, r5
    beq 1f
    movs r1, #1
1:  cmp r0, r1
    bne 8f
    /* AdvA, at payload octet 6, against the staged one. */
    movs r0, #(2 + EFR32_SEQ_ADV_SCAN_REQ_ADVA)
    movs r1, #EFR32_SEQ_CONN_ADVA
    bl efr32_seq_rx_addr_eq
    pop {r4-r6, pc}
8:  movs r0, #0
    pop {r4-r6, pc}
.ltorg

/*
 * Whether a scanner's or initiator's received frame is answered (seq_abi.h
 * EFR32_SEQ_CONN_SCAN), as lll_scan.c's isr_rx_pdu() decides without privacy:
 * an active scanner's SCAN_REQ, an initiator's CONNECT_IND.  r7 the stage
 * block, r4 the received header octet, r3 its length, r5 FLAGS.  Returns
 * r0 = 1 to answer.  Clobbers r0-r3; preserves r4-r7.
 */
.thumb_func
efr32_seq_scan_match:
    push {r4-r6, lr}
    /* An RX buffer for what the link layer reports. */
    movs r0, #EFR32_SEQ_CONN_RX_FREE
    tst r5, r0
    beq 8f
    movs r6, #EFR32_SEQ_ADV_TYPE_MASK
    ands r6, r4
    ldr r0, =EFR32_SEQ_CONN_INIT
    tst r5, r0
    bne 4f
    ldr r0, =EFR32_SEQ_CONN_SCAN_REQ
    tst r5, r0
    beq 8f
    /* Active scanner: an ADV_IND or ADV_SCAN_IND, with its AdvA. */
    cmp r6, #EFR32_SEQ_ADV_IND_TYPE
    beq 1f
    cmp r6, #EFR32_SEQ_ADV_SCAN_IND_TYPE
    bne 8f
1:  cmp r3, #EFR32_SEQ_ADV_ADDR_LEN
    blo 8f
    cmp r3, #EFR32_SEQ_ADV_IND_LEN_MAX
    bhi 8f
    b 9f
4:  /* Initiator: from the advertiser it connects to (TxAdd, then AdvA below). */
    lsrs r0, r4, #EFR32_SEQ_ADV_TXADD_BIT
    movs r1, #1
    ands r0, r1
    movs r1, #EFR32_SEQ_CONN_ADV_TXADD
    ands r1, r5
    beq 5f
    movs r1, #1
5:  cmp r0, r1
    bne 8f
    cmp r6, #EFR32_SEQ_ADV_IND_TYPE
    bne 6f
    cmp r3, #EFR32_SEQ_ADV_ADDR_LEN
    blo 8f
    cmp r3, #EFR32_SEQ_ADV_IND_LEN_MAX
    bhi 8f
    b 7f
6:  cmp r6, #EFR32_SEQ_ADV_DIRECT_IND_TYPE
    bne 8f
    cmp r3, #EFR32_SEQ_ADV_DIRECT_IND_LEN
    bne 8f
    /* TargetA: with the extended filter policy any resolvable private address
     * (RxAdd 1), else ours. */
    lsrs r6, r4, #EFR32_SEQ_ADV_RXADD_BIT
    ldr r0, =EFR32_SEQ_CONN_INIT_TGTA_RPA
    tst r5, r0
    beq 10f
    cmp r6, #1
    bne 10f
    ldr r0, [r7, #EFR32_SEQ_CONN_RX_BASE]
    ldr r1, [r7, #EFR32_SEQ_CONN_RX_OFFSET]
    ldr r2, [r7, #EFR32_SEQ_CONN_RX_MASK]
    adds r1, #(2 + EFR32_SEQ_ADV_DIRECT_TARGETA + EFR32_SEQ_ADDR_TYPE_OCTET)
    ands r1, r2
    ldrb r1, [r0, r1]
    movs r2, #EFR32_SEQ_ADDR_TYPE_MASK
    ands r1, r2
    cmp r1, #EFR32_SEQ_ADDR_TYPE_RPA
    beq 7f
10: movs r1, #0
    ldr r0, =EFR32_SEQ_CONN_INIT_RXADD
    tst r5, r0
    beq 11f
    movs r1, #1
11: cmp r6, r1
    bne 8f
    movs r0, #(2 + EFR32_SEQ_ADV_DIRECT_TARGETA)
    movs r1, #EFR32_SEQ_CONN_INITA
    bl efr32_seq_rx_addr_eq
    cmp r0, #0
    beq 8f
7:  movs r0, #2
    movs r1, #EFR32_SEQ_CONN_ADVA
    bl efr32_seq_rx_addr_eq
    cmp r0, #0
    beq 8f
    ldr r0, =EFR32_SEQ_CONN_INIT_TIMED
    tst r5, r0
    beq 9f
    bl efr32_seq_init_in_slot
    pop {r4-r6, pc}
9:  movs r0, #1
    pop {r4-r6, pc}
8:  movs r0, #0
    pop {r4-r6, pc}
.ltorg

/*
 * The 6-octet address at octet r0 of the received frame (the FIFO wraps)
 * equals the one at offset r1 of the stage block r7.  Returns r0 = 1 when it
 * does.  Clobbers r0-r3; preserves r4-r7.
 */
.thumb_func
efr32_seq_rx_addr_eq:
    push {r4-r6, lr}
    ldr r2, [r7, #EFR32_SEQ_CONN_RX_OFFSET]
    adds r4, r2, r0
    adds r5, r7, r1
    ldr r6, [r7, #EFR32_SEQ_CONN_RX_BASE]
    ldr r3, [r7, #EFR32_SEQ_CONN_RX_MASK]
    movs r1, #0
1:  adds r0, r4, r1
    ands r0, r3
    ldrb r0, [r6, r0]
    ldrb r2, [r5, r1]
    cmp r0, r2
    bne 8f
    adds r1, #1
    cmp r1, #EFR32_SEQ_ADV_ADDR_LEN
    blo 1b
    movs r0, #1
    pop {r4-r6, pc}
8:  movs r0, #0
    pop {r4-r6, pc}

/*
 * The initiator's frame ended early enough in its scan slot for the
 * CONNECT_IND (seq_abi.h EFR32_SEQ_CONN_INIT_TIMED): its end on TIMER0 is now
 * less RAC.STIMER (counting since RX2TX began) in TIMER0 counts, less the
 * RXFRAME exit.  r7 the stage block.  Returns r0 = 1 when in the slot.
 * Clobbers r0-r3.
 */
.thumb_func
efr32_seq_init_in_slot:
    push {r4, lr}
    ldr r0, =EFR32_RAC_STIMER
    ldr r0, [r0]
    uxth r0, r0
    ldr r1, =EFR32_SEQ_TIMER0_CNT_NS
    ldr r1, [r1]
    ldr r2, [r7, #EFR32_SEQ_SCAN_T0_PER_STIMER_Q8]
    muls r0, r2, r0
    lsrs r0, r0, #EFR32_SEQ_SCAN_Q8_SHIFT
    subs r1, r1, r0
    ldr r0, [r7, #EFR32_SEQ_SCAN_EOF_TO_EXIT]
    subs r1, r1, r0
    ldr r0, [r7, #EFR32_SEQ_SCAN_START]
    subs r0, r1, r0
    ldr r1, [r7, #EFR32_SEQ_SCAN_PERIOD]
    cmp r1, #0
    beq 1f
    bl efr32_seq_umod
1:  ldr r1, [r7, #EFR32_SEQ_SCAN_LATEST]
    cmp r0, r1
    blo 2f
    movs r0, #0
    pop {r4, pc}
2:  movs r0, #1
    pop {r4, pc}
.ltorg

/* r0 = r0 mod r1 (unsigned, r1 not 0), by shift and subtract.  Clobbers r2-r3. */
.thumb_func
efr32_seq_umod:
    mov r2, r1
1:  lsls r3, r2, #1
    bcs 2f
    cmp r3, r0
    bhi 2f
    mov r2, r3
    b 1b
2:  cmp r0, r2
    blo 3f
    subs r0, r0, r2
3:  cmp r2, r1
    beq 4f
    lsrs r2, r2, #1
    b 2b
4:  bx lr
