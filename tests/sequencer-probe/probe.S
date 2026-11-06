/* SPDX-License-Identifier: Apache-2.0 */
/*
 * EFR32BG22 radio-sequencer interrupt probe.
 *
 * This is independent ARMv6-M code for the radio's private Cortex-M0+ class
 * core, not code for the application M33.  Every external vector records the
 * exception number and a few radio registers in SEQ's public scratch words.
 *
 * Raw values below are temporary silicon-characterization observations.  Do
 * not copy them into the production driver: first express them as named
 * register fields or typed PHY data and retain their provenance.  See README.
 */

.syntax unified
.cpu cortex-m0plus
.thumb

.equ SEQ_SPARE0,  0x00004fe0
.equ RAC_STATUS,  0xa802000c
.equ RAC_IF,      0xa802001c
.equ RAC_SEQIF,   0xa8020028
.equ RAC_IF_CLR,  0xa802201c
.equ RAC_SEQIF_CLR, 0xa8022028
.equ RAC_SEQIEN_SET, 0xa802102c
/* Stock secure startup prefix, retained as a characterization fixture. */
.equ RAC_CTRL_SET, 0xa8021014
.equ RAC_CTRL_CLR, 0xa8022014
.equ RAC_SEQIF_SET, 0xa8021028
.equ RAC_SEQCTRL, 0xa8020038
.equ RAC_SR3, 0xa802004c
.equ RAC_SR3_SET, 0xa802104c
.equ RAC_SR3_CLR, 0xa802204c
.equ RAC_CTRL_POLARITIES, 0x00000380
.equ RAC_CTRL_FORCEDISABLE, 0x00000001
.equ RAC_CTRL_CPUWAITDIS, 0x04000000
.equ RAC_SEQIF_STIMCMPEVSEQ, 0x00000002
.equ RAC_SEQIF_STATE_EVENTS, 0x3fff0000
.equ OBS_RAC_SEQCTRL_STARTUP, 0x00000011
.equ OBS_RAC_SR3_STARTUP, 0x00f04800
/* IRQ8 prologue observed live through AP3 on 2026-10-27.  The vendor handler
 * pulses bit 18 through the SET alias, waits for the base bit to clear, and
 * then writes the same bit through the CLR alias before radio setup. */
.equ OBS_IRQ8_GATE_BASE, 0xa8020040
.equ OBS_IRQ8_GATE_SET,  0xa8021040
.equ OBS_IRQ8_GATE_CLR,  0xa8022040
.equ OBS_IRQ8_GATE_BIT,  0x00040000
/* Write-only IRQ8 actions observed at proprietary routine 0x1bda.  Their
 * semantic field names are not yet established, so keep that uncertainty
 * explicit instead of pretending these are production register definitions. */
.equ OBS_IRQ8_CLR_2044, 0xa8022044
.equ OBS_IRQ8_CLR_2040, 0xa8022040
.equ OBS_IRQ8_CLR_2008, 0xa8022008
.equ NVIC_ISER,   0xe000e100
.equ NVIC_ICPR,   0xe000e280
.equ TRACE_BASE,  0x00003000
.equ TRACE_INDEX, 0x00004fdc
.ifndef STOP_EXCEPTION
.equ STOP_EXCEPTION, 0
.endif

.section .vectors, "a", %progbits
.word 0x00003e00
.word reset + 1
.rept 14
.word exception + 1
.endr
.rept 48
.word exception + 1
.endr

.section .text, "ax", %progbits
.global reset
.thumb_func
reset:
    cpsid i
    /* Stock reaches OFF at this exact M0-owned peripheral boundary.  These
     * are the public RAC sequencer state-event flags, not an M33 scheduling
     * operation; clear stale state indications before enabling handlers. */
    ldr r0, =RAC_SEQIF_CLR
    ldr r1, =RAC_SEQIF_STATE_EVENTS
    str r1, [r0]
    ldr r0, =RAC_SEQIEN_SET
    str r1, [r0]
#if !SKIP_WAKE_PREFIX
    /* Execute the secure-address wake prefix from the sequencer core.  This
     * tests ownership of SHUTDOWN exit; these observed presets are not a
     * production interface. */
    ldr r0, =RAC_SR3
    ldr r1, =OBS_RAC_SR3_STARTUP
    str r1, [r0]
    ldr r0, =RAC_SR3_CLR
    ldr r1, =0x00004000
    str r1, [r0]
    ldr r0, =RAC_CTRL_SET
    ldr r1, =RAC_CTRL_POLARITIES
    str r1, [r0]
    ldr r0, =RAC_SR3_SET
    movs r1, #2
    lsls r1, r1, #8
    str r1, [r0]
    ldr r0, =RAC_CTRL_SET
    movs r1, #RAC_CTRL_FORCEDISABLE
    str r1, [r0]
    ldr r0, =RAC_SEQIF_SET
    movs r1, #RAC_SEQIF_STIMCMPEVSEQ
    str r1, [r0]
    ldr r0, =RAC_SEQCTRL
    movs r1, #OBS_RAC_SEQCTRL_STARTUP
    str r1, [r0]
    ldr r0, =RAC_CTRL_SET
    ldr r1, =RAC_CTRL_CPUWAITDIS
    str r1, [r0]
    ldr r0, =RAC_CTRL_CLR
    movs r1, #RAC_CTRL_FORCEDISABLE
    str r1, [r0]
#endif

    ldr r0, =SEQ_SPARE0
    ldr r1, =0x52535421       /* "RST!" */
    str r1, [r0]

    ldr r0, =TRACE_INDEX
    movs r1, #0
    str r1, [r0]

    ldr r0, =NVIC_ISER
    movs r1, #1
    rsbs r1, r1, #0          /* 0xffffffff */
    str r1, [r0]
    str r1, [r0, #4]
    ldr r0, =NVIC_ICPR
    str r1, [r0]
    str r1, [r0, #4]
    cpsie i

1:  wfi
    b 1b

.global exception
.thumb_func
exception:
    push {r4-r7}
    mrs r0, ipsr
    ldr r1, =SEQ_SPARE0
    str r0, [r1]
    ldr r2, =RAC_STATUS
    ldr r3, [r2]
    str r3, [r1, #4]
    ldr r2, =RAC_IF
    ldr r3, [r2]
    str r3, [r1, #8]
    ldr r2, =RAC_SEQIF
    ldr r3, [r2]
    str r3, [r1, #12]
    ldr r4, =TRACE_INDEX
    ldr r5, [r4]
    adds r6, r5, #1
    str r6, [r4]
    movs r6, #15
    ands r5, r6
    lsls r5, r5, #4
    ldr r6, =TRACE_BASE
    adds r5, r6, r5
    stmia r5!, {r0, r3}
    ldr r2, =RAC_STATUS
    ldr r6, [r2]
    ldr r2, =RAC_IF
    ldr r7, [r2]
    stmia r5!, {r6, r7}

    cmp r0, #STOP_EXCEPTION
    bne 9f
8:  b 8b
9:

    cmp r0, #24              /* external IRQ 8: TXWARM */
    bne 2f
    /* Preserve the ordering of vendor helpers 0x7ac and 0x7be.  Endpoint
     * snapshots miss this pulse because SET/CLR are write-only aliases. */
    ldr r2, =OBS_IRQ8_GATE_SET
    ldr r5, =OBS_IRQ8_GATE_BIT
    str r5, [r2]
0:
    ldr r2, =OBS_IRQ8_GATE_BASE
    ldr r6, [r2]
    movs r7, #4
    tst r6, r7
    bne 0b
    ldr r2, =OBS_IRQ8_GATE_CLR
    str r5, [r2]
    ldr r2, =0xa8020014
    ldr r5, =0x04000380
    str r5, [r2]
    ldr r2, =0xa802005c
    ldr r5, =0x00070001
    str r5, [r2]
    ldr r2, =0xa802012c
    ldr r5, =0x1e23221d
    str r5, [r2]
    ldr r2, =0xa802013c
    movs r5, #5
    str r5, [r2]
    ldr r2, =0xa800c00c
    ldr r5, =0x00008880
    str r5, [r2]
    ldr r2, =0xa800c018
    ldr r5, =0x0000c940
    str r5, [r2]
    ldr r2, =0xa8014008
    movs r5, #0
    str r5, [r2]
    ldr r2, =0xa80140e8
    ldr r5, =0x3ffb0001
    str r5, [r2]
    ldr r2, =0xa801804c
    ldr r5, =0x000081e7
    str r5, [r2]
    /* Endpoint snapshots cannot reveal writes through the CLR alias.  These
     * three actions reproduce the directly disassembled IRQ8 helper. */
    ldr r2, =OBS_IRQ8_CLR_2044
    movs r5, #3
    str r5, [r2]
    ldr r2, =OBS_IRQ8_CLR_2040
    movs r5, #168
    str r5, [r2]
    ldr r2, =OBS_IRQ8_CLR_2008
    movs r5, #16
    str r5, [r2]
2:
    cmp r0, #25              /* external IRQ 9: TX */
    bne 3f
    ldr r2, =0xa8020040
    ldr r5, =0x00010000
    str r5, [r2]
    ldr r2, =0xa8020044
    ldr r5, =0x00020000
    str r5, [r2]
    ldr r2, =0xa8020094
    ldr r5, =0x00100200
    str r5, [r2]
    ldr r2, =0xa80200a8
    movs r5, #1
    str r5, [r2]
    ldr r2, =0xa80200ac
    ldr r5, =0x00009408
    str r5, [r2]
    ldr r2, =0xa80200b4
    ldr r5, =0x10000000
    str r5, [r2]
    ldr r2, =0xa8020108
    movs r5, #1
    str r5, [r2]
    ldr r2, =0xa8020114
    ldr r5, =0x00038997
    str r5, [r2]
    ldr r2, =0xa802011c
    ldr r5, =0x00322d2a
    str r5, [r2]
    ldr r2, =0xa802012c
    ldr r5, =0x1e22221e
    str r5, [r2]
    ldr r2, =0xa802013c
    movs r5, #7
    str r5, [r2]
    ldr r2, =0xa8020140
    movs r5, #20
    str r5, [r2]
    ldr r2, =0xa8020144
    movs r5, #16
    str r5, [r2]
    ldr r2, =0xa8020148
    movs r5, #0
    str r5, [r2]
    ldr r2, =0xa8020150
    ldr r5, =0x00066e28
    str r5, [r2]
    ldr r2, =0xa8020154
    ldr r5, =0x00003fc4
    str r5, [r2]
    ldr r2, =0xa802015c
    ldr r5, =0x00001fbd
    str r5, [r2]
    ldr r2, =0xa8020160
    ldr r5, =0x00000400
    str r5, [r2]
    ldr r2, =0xa8020168
    str r5, [r2]
    ldr r2, =0xa8020188
    ldr r5, =0x000007f3
    str r5, [r2]

    ldr r2, =0xa8004008
    ldr r5, =0x01700220
    str r5, [r2]
    ldr r2, =0xa8004070
    ldr r5, =0x01380000
    str r5, [r2]
    ldr r2, =0xa80040b4
    ldr r5, =0x40020e01
    str r5, [r2]
    ldr r2, =0xa800c018
    ldr r5, =0x0000d800
    str r5, [r2]
    ldr r2, =0xa80140e8
    ldr r5, =0x3fda8090
    str r5, [r2]
    ldr r2, =0xa8018008
    ldr r5, =0x04000005
    str r5, [r2]
    ldr r2, =0xa801802c
    movs r5, #165
    str r5, [r2]
    ldr r2, =0xa801804c
    ldr r5, =0x00008335
    str r5, [r2]
    ldr r2, =0xa8018054
    ldr r5, =0x00000158
    str r5, [r2]
    ldr r2, =0xa801808c
    ldr r5, =0x00000fa4
    str r5, [r2]
    ldr r2, =0xa8018090
    ldr r5, =0x00000fa3
    str r5, [r2]
    ldr r2, =0xa801001c
    ldr r5, =0x00aaaaaa
    str r5, [r2]
    ldr r2, =0xaa000018
    movs r5, #36
    str r5, [r2]
    ldr r2, =0xaa000020
    movs r5, #8
    str r5, [r2]
    ldr r2, =0xaa00002c
    movs r5, #0
    str r5, [r2]
    /* The live stage-2 TX handler then requests its post-advertising receive
     * state and applies the corresponding analog tail before STATETX is
     * acknowledged. */
    ldr r2, =0xa8020008
    movs r5, #2
    str r5, [r2]
    ldr r2, =0xa802012c
    ldr r5, =0x1e23221d
    str r5, [r2]
    ldr r2, =0xa800c018
    ldr r5, =0x0000c700
    str r5, [r2]
    ldr r2, =0xa80140e8
    ldr r5, =0x3fff8008
    str r5, [r2]
    ldr r2, =0xa801808c
    ldr r5, =0x00000fa5
    str r5, [r2]
3:

    /* Acknowledge only the register-visible sources, then observe whether
     * silicon advances to another state/event without vendor firmware. */
    ldr r2, =RAC_SEQIF_CLR
    str r3, [r2]
    ldr r2, =RAC_IF_CLR
    str r7, [r2]
    pop {r4-r7}
    bx lr
