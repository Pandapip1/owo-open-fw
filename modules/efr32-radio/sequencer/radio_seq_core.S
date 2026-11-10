/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Open firmware for the EFR32BG22 private radio Cortex-M0+: the parts whose instruction sequence
 * is the point.  Every state-event handler below runs while the RAC holds its transition (a state
 * event enabled in SEQIEN holds it until acknowledged, SS7.69), so its cycle count is part of the
 * warm-up and turnaround times the M33 relies on (T_IFS, SS7.71/7.91); the timed command polls
 * TIMER0 for its instant and the busy wait is calibrated per pass.  The bounded logic (reset,
 * the staged reply's decision) is C, in radio_seq.c.  docs/radio-tables.md explains each
 * register-level contract.
 */

#include "seq_regs.h"

.syntax unified
.cpu cortex-m0plus
.thumb

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

.global efr32_radio_seq_exception
.ltorg

.thumb_func
efr32_radio_seq_exception:
#if defined(EFR32_SEQ_WAKE_PROBE)
    /* Diagnostic only (seq_abi.h EFR32_SEQ_TIMED_EXC_ST): SysTick at entry. */
    ldr r0, =EFR32_SYSTICK_CVR
    ldr r0, [r0]
    ldr r1, =EFR32_SEQ_TIMED_BLOCK
    str r0, [r1, #EFR32_SEQ_TIMED_EXC_ST]
#endif
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

.ltorg

.global efr32_seq_delay
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

.global efr32_seq_trace_count
.thumb_func
efr32_seq_trace_count:
    ldr r1, =EFR32_SEQ_TRACE_IRQ_COUNTS
    adds r1, r1, r0
    ldrb r0, [r1]
    adds r0, #1
    strb r0, [r1]
    bx lr
.ltorg

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
#if defined(EFR32_SEQ_WAKE_PROBE)
    ldr r4, =EFR32_SYSTICK_CVR
    ldr r4, [r4]
    str r4, [r0, #EFR32_SEQ_TIMED_POLL_ST]
    ldr r4, [r0, #EFR32_SEQ_TIMED_EXC_ST]
    str r4, [r0, #EFR32_SEQ_TIMED_ENTRY_ST]
#endif
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

