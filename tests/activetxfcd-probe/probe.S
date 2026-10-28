/* SPDX-License-Identifier: Apache-2.0 */
/*
 * EFR32BG22 ACTIVETXFCD / PROTIMER IRQ-handshake probe.
 *
 * A from-scratch, standalone bootable M33 image (no Zephyr), with its own
 * real vector table including a genuine IRQ36 (PROTIMER) handler. Built to
 * test the one sub-hypothesis the rest of this investigation couldn't
 * (docs/radio-tables.md SS7.25-SS7.38): does FRC's TX frame coder need the
 * CPU to actually take a serviced PROTIMER interrupt, not just have
 * PROTIMER clocked and pending, before it will advance BUFC's TX
 * READOFFSET past the 2-byte header?
 *
 * Avoids the two dead ends already found on the real running firmware:
 * - Redirecting VTOR to install a handler on the live Zephyr image
 *   reliably aborts the TX attempt via an unrooted mechanism (SS7.36) --
 *   this image has its own vector table from reset, so there is no VTOR
 *   redirect at all.
 * - Hand-transcribing the ~150-write PHY config table risks exactly the
 *   kind of error SS7.13 already got burned by once -- replay-table.inc
 *   is generated from two real execution traces (SS7.37/SS7.38), not
 *   retyped by hand.
 *
 * What this image does NOT replicate: Zephyr's own boot path, the BLE host
 * stack, or anything after the real TXEN write. It replicates exactly the
 * sequence real_setup()/radio_reset()/radio_phy_set()/chan_prepare()/
 * efr32_packet_stage_arm()/radio_tx_enable() perform on real hardware (per
 * modules/efr32-radio/hal/radio.c and the two trace captures), then adds
 * PROTIMER's real enable sequence (already independently tested safe in
 * tests/protimer-probe/, SS7.32-SS7.34) with a real installed IRQ36
 * vector this time, then issues the real TXEN.
 *
 * REQUIRES the real open M0+ sequencer image pre-loaded into SEQRAM
 * (0xB0000000) via a separate debugger `load_image` step BEFORE this image
 * runs -- same precondition tests/hardware-lockstep/m33-seq-launch.S
 * documents. See README.md for the exact OpenOCD sequence.
 */
.syntax unified
.cpu cortex-m33
.thumb

/* -- CMU: radio_setup()'s clock bring-up -- */
.equ CMU_RADIOCLKCTRL_SET, 0x40009280
.equ CMU_RADIOCLKCTRL_EN,  0x00000001
.equ CMU_CLKEN1_SET,       0x40009068
/* AGC|MODEM|RFCRC|FRC|PROTIMER|RAC|SYNTH|BUFC, per
 * vendor/hal_silabs/.../efr32bg22_cmu.h's _CMU_CLKEN1_*_MASK values --
 * matches radio_setup()'s own radio_bus_clocks exactly. */
.equ CMU_RADIO_BLOCKS,     0x0000087f

/* -- radio_setup()'s per-block EN.EN latches (all public NS +0x004) -- */
.equ EFR32_RADIO_EN_OFFSET, 0x004
.equ AGC_BASE,      0xb800c000
.equ BUFC_BASE,     0xba000000
.equ MODEM_BASE,    0xb8014000
.equ RFCRC_BASE,    0xb8010000
.equ FRC_BASE,      0xb8004000
.equ PROTIMER_BASE, 0xb801c000
.equ RAC_BASE,      0xb8020000
.equ SYNTH_BASE,    0xb8018000

/* -- radio_reset()'s sequencer install/release sequence -- */
.equ RAC_CTRL,       RAC_BASE + 0x014
.equ RAC_CTRL_SET,   RAC_BASE + 0x1014
.equ RAC_CTRL_CLR,   RAC_BASE + 0x2014
.equ RAC_SR0,        RAC_BASE + 0x040
.equ RAC_SR1,        RAC_BASE + 0x044
.equ RAC_SR2,        RAC_BASE + 0x048
.equ RAC_SEQIF_SET,  RAC_BASE + 0x1028
.equ RAC_SEQCTRL,    RAC_BASE + 0x038
.equ RAC_STATUS,     RAC_BASE + 0x00c
.equ RAC_CMD,        RAC_BASE + 0x010
/* BIT(7)|BIT(8)|BIT(9)|BIT(24): ACTIVE_HIGH|PA_ENABLE_HIGH|LNA_ENABLE_HIGH|SEQRESET */
.equ RAC_CTRL_HOLD_VAL,    0x01000380
/* BIT(7)|BIT(8)|BIT(9)|BIT(0)|BIT(26): ...|FORCEDISABLE|CPUWAITDIS */
.equ RAC_CTRL_RELEASE_VAL, 0x04000381
.equ RAC_CTRL_FORCEDISABLE, 0x00000001
.equ RAC_SEQIF_STIMCMPEVSEQ, 0x00000002
.equ RAC_SEQCTRL_STAGE2_STARTUP, 0x00000011
.equ RAC_CMD_TXEN, 0x00000001
.equ SEQ_IDENTITY_ADDRESS, 0xb0004fe0
.equ SEQ_IDENTITY_OPEN_V1, 0x4f535131
.equ SEQ_IDENTITY_WAIT_BOUND, 1000000

/* -- PROTIMER real enable sequence, independently tested safe
 * (tests/protimer-probe/, SS7.32-SS7.34); EN is already 1 from
 * radio_setup() above (SS7.39), written again here for clarity. -- */
.equ PROTIMER_EN,          PROTIMER_BASE + 0x004
.equ PROTIMER_CTRL,        PROTIMER_BASE + 0x008
.equ PROTIMER_CTRL_VAL,    0x00011102
.equ PROTIMER_CMD,         PROTIMER_BASE + 0x00c
.equ PROTIMER_CMD_START,   0x00000001
.equ PROTIMER_REG_030,     PROTIMER_BASE + 0x030
.equ PROTIMER_REG_030_VAL, 0x00001233
.equ PROTIMER_REG_038,     PROTIMER_BASE + 0x038
.equ PROTIMER_REG_038_VAL, 0x80015558
.equ PROTIMER_IEN,         PROTIMER_BASE + 0x070
.equ PROTIMER_IF_CLR,      PROTIMER_BASE + 0x2064

.equ NVIC_ISER1,      0xe000e104
.equ NVIC_IRQ36_BIT,  (1 << 4)

.section .vectors, "a", %progbits
.word 0x20007000      /* initial SP: fresh 32KB SRAM, no Zephyr coexistence */
.word reset + 1
.rept 14
.word fault + 1
.endr
.rept 36               /* IRQ0..IRQ35 */
.word fault + 1
.endr
.word irq36_handler + 1 /* IRQ36: PROTIMER */
.rept 11                /* IRQ37..IRQ47 */
.word fault + 1
.endr

.section .text, "ax", %progbits
.thumb_func
reset:
	cpsid	i

	/* CMU: radio_setup()'s clock bring-up. */
	ldr	r0, =CMU_RADIOCLKCTRL_SET
	ldr	r1, =CMU_RADIOCLKCTRL_EN
	str	r1, [r0]
	ldr	r0, =CMU_CLKEN1_SET
	ldr	r1, =CMU_RADIO_BLOCKS
	str	r1, [r0]
	dsb

	/* radio_setup(): eight EN.EN writes, same order as radio.c. */
	movs	r1, #1
	ldr	r0, =AGC_BASE + EFR32_RADIO_EN_OFFSET
	str	r1, [r0]
	ldr	r0, =BUFC_BASE + EFR32_RADIO_EN_OFFSET
	str	r1, [r0]
	ldr	r0, =MODEM_BASE + EFR32_RADIO_EN_OFFSET
	str	r1, [r0]
	ldr	r0, =RFCRC_BASE + EFR32_RADIO_EN_OFFSET
	str	r1, [r0]
	ldr	r0, =FRC_BASE + EFR32_RADIO_EN_OFFSET
	str	r1, [r0]
	ldr	r0, =PROTIMER_BASE + EFR32_RADIO_EN_OFFSET
	str	r1, [r0]
	ldr	r0, =RAC_BASE + EFR32_RADIO_EN_OFFSET
	str	r1, [r0]
	ldr	r0, =SYNTH_BASE + EFR32_RADIO_EN_OFFSET
	str	r1, [r0]

	/* radio_reset(): hold the sequencer in reset. The real M0+ image is
	 * preloaded into SEQRAM by the debugger before this image runs, so
	 * there is no copy loop here (unlike efr32_radio_sequencer_install()
	 * in radio.c, which copies it from flash at this exact point). */
	ldr	r0, =RAC_CTRL
	ldr	r1, =RAC_CTRL_HOLD_VAL
	str	r1, [r0]
	dsb

	/* SR0/SR1/SR2 = 0 (SR3 deliberately untouched, same as radio.c). */
	movs	r1, #0
	ldr	r0, =RAC_SR0
	str	r1, [r0]
	ldr	r0, =RAC_SR1
	str	r1, [r0]
	ldr	r0, =RAC_SR2
	str	r1, [r0]

	ldr	r0, =RAC_CTRL_SET
	ldr	r1, =RAC_CTRL_FORCEDISABLE
	str	r1, [r0]
	ldr	r0, =RAC_SEQIF_SET
	ldr	r1, =RAC_SEQIF_STIMCMPEVSEQ
	str	r1, [r0]

	/* Wait for RAC.STATUS.STATE to settle to OFF(0) or SHUTDOWN(13). */
wait_quiescent:
	ldr	r0, =RAC_STATUS
	ldr	r2, [r0]
	ubfx	r2, r2, #24, #4
	cmp	r2, #0
	beq	quiescent
	cmp	r2, #13
	beq	quiescent
	b	wait_quiescent
quiescent:

	ldr	r0, =RAC_SEQCTRL
	ldr	r1, =RAC_SEQCTRL_STAGE2_STARTUP
	str	r1, [r0]

	/* Release: base-register replacement, then clear FORCEDISABLE via
	 * its CLR alias, exactly as radio_reset()'s own comment describes. */
	ldr	r0, =RAC_CTRL
	ldr	r1, =RAC_CTRL_RELEASE_VAL
	str	r1, [r0]
	ldr	r0, =RAC_CTRL_CLR
	ldr	r1, =RAC_CTRL_FORCEDISABLE
	str	r1, [r0]
	dsb

	/* Wait (bounded) for the open sequencer to publish its identity. */
	ldr	r4, =SEQ_IDENTITY_ADDRESS
	ldr	r5, =SEQ_IDENTITY_OPEN_V1
	ldr	r7, =SEQ_IDENTITY_WAIT_BOUND
	movs	r6, #0
wait_identity:
	ldr	r2, [r4]
	cmp	r2, r5
	beq	identity_ready
	adds	r6, r6, #1
	cmp	r6, r7
	bge	identity_ready
	b	wait_identity
identity_ready:

	/* Replay the 200 real, directly-captured MMIO stores (radio_phy_set
	 * through the real TXEN store, not inclusive -- see replay-table.inc). */
	ldr	r0, =replay_table
	ldr	r1, =replay_table_end
replay_loop:
	cmp	r0, r1
	bhs	replay_done
	ldr	r2, [r0]
	ldr	r3, [r0, #4]
	str	r3, [r2]
	adds	r0, r0, #8
	b	replay_loop
replay_done:

	/* PROTIMER's real enable sequence (EN already 1 from radio_setup()
	 * above). */
	ldr	r0, =PROTIMER_CTRL
	ldr	r1, =PROTIMER_CTRL_VAL
	str	r1, [r0]
	ldr	r0, =PROTIMER_REG_030
	ldr	r1, =PROTIMER_REG_030_VAL
	str	r1, [r0]
	ldr	r0, =PROTIMER_REG_038
	ldr	r1, =PROTIMER_REG_038_VAL
	str	r1, [r0]
	ldr	r0, =PROTIMER_IEN
	movs	r1, #0
	mvns	r1, r1          /* 0xFFFFFFFF, same blanket sweep as SS7.34 */
	str	r1, [r0]
	ldr	r0, =PROTIMER_CMD
	ldr	r1, =PROTIMER_CMD_START
	str	r1, [r0]

	/* A real vector is installed above (unlike SS7.32's deliberately
	 * masked test) -- safe to unmask IRQ36 now. */
	ldr	r0, =NVIC_ISER1
	ldr	r1, =NVIC_IRQ36_BIT
	str	r1, [r0]
	cpsie	i

	/* The real RAC.CMD.TXEN write (not captured by the trace, which
	 * stops just before executing it). */
	ldr	r0, =RAC_CMD
	ldr	r1, =RAC_CMD_TXEN
	str	r1, [r0]

spin:
	wfi
	b	spin

.thumb_func
irq36_handler:
	/* Ack everything pending (blanket, same rationale as SS7.34 -- IF/IEN
	 * bit meanings are unresolved) and return. */
	ldr	r0, =PROTIMER_IF_CLR
	movs	r1, #0
	mvns	r1, r1
	str	r1, [r0]
	bx	lr

.thumb_func
fault:
	b	fault

.align 2
replay_table:
.include "replay-table.inc"
replay_table_end:
