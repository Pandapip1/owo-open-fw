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
 */
.equ EFR32_RAC_TXWARM_GATE_BASE,      0xa8020040
.equ EFR32_RAC_TXWARM_GATE_SET,       0xa8021040
.equ EFR32_RAC_TXWARM_GATE_CLR,       0xa8022040
.equ EFR32_RAC_TXWARM_GATE_BIT,       (1 << 18)
.equ EFR32_RAC_TXWARM_GATE_WAIT_BIT,  (1 << 2)
/* Observed at 0xa802113c (SET alias of 0xa802003c); semantics unestablished. */
.equ EFR32_RAC_0X3C_SET,              0xa802113c
.equ EFR32_RAC_0X3C_SET_VALUE,        5
/* Three observed CLR-alias writes; semantics unestablished. */
.equ EFR32_RAC_0X44_CLR,              0xa8022044
.equ EFR32_RAC_0X44_CLR_MASK,         0x03
.equ EFR32_RAC_TXWARM_GATE_CLR_MASK2, 0xa8
.equ EFR32_RAC_0X08_CLR,              0xa8022008
.equ EFR32_RAC_0X08_CLR_MASK,         0x10
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

    mrs r3, ipsr
    cmp r3, #EFR32_EXCEPTION_TXWARM
    bne 2f
    /* Bounded TXWARM gate pulse (docs/radio-tables.md SS7.2). */
    ldr r0, =EFR32_RAC_TXWARM_GATE_SET
    ldr r1, =EFR32_RAC_TXWARM_GATE_BIT
    str r1, [r0]
    ldr r3, =EFR32_RAC_TXWARM_WAIT_BOUND
10: ldr r0, =EFR32_RAC_TXWARM_GATE_BASE
    ldr r1, [r0]
    ldr r2, =EFR32_RAC_TXWARM_GATE_WAIT_BIT
    ands r1, r2
    beq 12f
    subs r3, #1
    bne 10b
12: ldr r0, =EFR32_RAC_0X3C_SET
    movs r1, #EFR32_RAC_0X3C_SET_VALUE
    str r1, [r0]
    ldr r0, =EFR32_RAC_TXWARM_GATE_CLR
    ldr r1, =EFR32_RAC_TXWARM_GATE_BIT
    str r1, [r0]
    ldr r0, =EFR32_RAC_0X44_CLR
    movs r1, #EFR32_RAC_0X44_CLR_MASK
    str r1, [r0]
    ldr r0, =EFR32_RAC_TXWARM_GATE_CLR
    movs r1, #EFR32_RAC_TXWARM_GATE_CLR_MASK2
    str r1, [r0]
    ldr r0, =EFR32_RAC_0X08_CLR
    movs r1, #EFR32_RAC_0X08_CLR_MASK
    str r1, [r0]
    ldr r0, =EFR32_RAC_SEQIF_CLEAR
    ldr r1, =EFR32_RAC_SEQIF_TXWARM_ACK_MASK
    str r1, [r0]
    ldr r3, =EFR32_RAC_TXWARM_WAIT_BOUND
11: ldr r0, =EFR32_RAC_SEQIF
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
