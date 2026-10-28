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

.syntax unified
.cpu cortex-m0plus
.thumb

.equ EFR32_SEQ_STACK_TOP,             0x00003e00
.equ EFR32_SEQ_IDENTITY_WORD,         0x00004fe0
.equ EFR32_SEQ_IDENTITY_OPEN_V1,      0x4f535131 /* "OSQ1" */
.equ EFR32_SEQ_TRACE_TXWARM,          0x00004fc0
.equ EFR32_SEQ_TRACE_TX,              0x00004fd0
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
.equ EFR32_RAC_IRQ_TXWARM,            (1 << 8)
.equ EFR32_RAC_IRQ_TX,                (1 << 9)
.equ EFR32_EXCEPTION_TXWARM,          (16 + 8)
.equ EFR32_EXCEPTION_TX,              (16 + 9)

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
.equ EFR32_PROTIMER_0X38,              0xa801c038
.equ EFR32_FRC_0X1C,                   0xa800401c
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

    /* Live AP3 traces identify external IRQ8 and IRQ9 as the TXWARM and TX
     * state-service entries.  Enable only those bounded peripheral events;
     * the M33 scheduler remains the sole source of radio-job policy. */
    ldr r0, =EFR32_NVIC_ISER0
    ldr r1, =(EFR32_RAC_IRQ_TXWARM | EFR32_RAC_IRQ_TX)
    str r1, [r0]

    cpsie i
1:  wfi
    b 1b

.global efr32_radio_seq_exception
.thumb_func
efr32_radio_seq_exception:
    /* Acknowledge only enabled state-transition events.  The M0+ does not
     * turn them into a new radio command; policy remains on the M33. */
    mrs r3, ipsr
    cmp r3, #EFR32_EXCEPTION_TXWARM
    beq 1f
    cmp r3, #EFR32_EXCEPTION_TX
    bne 2f
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
12: ldr r0, =EFR32_RAC_PRESC_SET
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
     * above. r3 = (1 << ((PROTIMER+0x38 bits[31:29]) + 6)) - (FRC+0x1C
     * & 0x1fff), truncated to 16 bits. */
    ldr r0, =EFR32_PROTIMER_0X38
    ldr r1, [r0]
    lsrs r1, r1, #29
    adds r1, r1, #6
    movs r2, #1
    lsls r2, r2, r1
    ldr r0, =EFR32_FRC_0X1C
    ldr r1, [r0]
    ldr r3, =0x1fff
    ands r1, r3
    subs r3, r2, r1
    ldr r2, =0xffff
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
     * loop spinning uselessly to its timeout instead of self-correcting. */
    ldr r3, =EFR32_RAC_TXWARM_WAIT_BOUND
11: ldr r0, =EFR32_RAC_SEQIF_CLEAR
    ldr r1, =EFR32_RAC_SEQIF_TXWARM_ACK_MASK
    str r1, [r0]
    ldr r0, =EFR32_RAC_SEQIF
    ldr r1, [r0]
    ldr r2, =EFR32_RAC_SEQIF_TXWARM_ACK_MASK
    ands r1, r2
    beq 2f
    subs r3, #1
    bne 11b
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
