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
 * runs -- same precondition tests/hardware-lockstep/m33-seq-launch.c
 * documents. See README.md for the exact OpenOCD sequence.
 */
#include <stdint.h>

#define REG(addr) (*(volatile uint32_t *)(addr))

/* -- CMU: radio_setup()'s clock bring-up -- */
#define CMU_RADIOCLKCTRL_SET 0x40009280U
#define CMU_RADIOCLKCTRL_EN 0x00000001U
#define CMU_CLKEN1_SET 0x40009068U
/* AGC|MODEM|RFCRC|FRC|PROTIMER|RAC|SYNTH|BUFC, per
 * vendor/hal_silabs/.../efr32bg22_cmu.h's _CMU_CLKEN1_*_MASK values --
 * matches radio_setup()'s own radio_bus_clocks exactly. */
#define CMU_RADIO_BLOCKS 0x0000087fU

/* -- radio_setup()'s per-block EN.EN latches (all public NS +0x004) -- */
#define EFR32_RADIO_EN_OFFSET 0x004U
#define AGC_BASE 0xb800c000U
#define BUFC_BASE 0xba000000U
#define MODEM_BASE 0xb8014000U
#define RFCRC_BASE 0xb8010000U
#define FRC_BASE 0xb8004000U
#define PROTIMER_BASE 0xb801c000U
#define RAC_BASE 0xb8020000U
#define SYNTH_BASE 0xb8018000U

/* -- radio_reset()'s sequencer install/release sequence -- */
#define RAC_CTRL (RAC_BASE + 0x014U)
#define RAC_CTRL_SET (RAC_BASE + 0x1014U)
#define RAC_CTRL_CLR (RAC_BASE + 0x2014U)
#define RAC_SR0 (RAC_BASE + 0x040U)
#define RAC_SR1 (RAC_BASE + 0x044U)
#define RAC_SR2 (RAC_BASE + 0x048U)
#define RAC_SEQIF_SET (RAC_BASE + 0x1028U)
#define RAC_SEQCTRL (RAC_BASE + 0x038U)
#define RAC_STATUS (RAC_BASE + 0x00cU)
#define RAC_CMD (RAC_BASE + 0x010U)
/* BIT(7)|BIT(8)|BIT(9)|BIT(24): ACTIVE_HIGH|PA_ENABLE_HIGH|LNA_ENABLE_HIGH|SEQRESET */
#define RAC_CTRL_HOLD_VAL 0x01000380U
/* BIT(7)|BIT(8)|BIT(9)|BIT(0)|BIT(26): ...|FORCEDISABLE|CPUWAITDIS */
#define RAC_CTRL_RELEASE_VAL 0x04000381U
#define RAC_CTRL_FORCEDISABLE 0x00000001U
#define RAC_SEQIF_STIMCMPEVSEQ 0x00000002U
#define RAC_SEQCTRL_STAGE2_STARTUP 0x00000011U
#define RAC_CMD_TXEN 0x00000001U
#define RAC_STATUS_STATE_SHIFT 24
#define RAC_STATUS_STATE_MASK 0xfU
#define RAC_STATE_OFF 0U
#define RAC_STATE_SHUTDOWN 13U
#define SEQ_IDENTITY_ADDRESS 0xb0004fe0U
#define SEQ_IDENTITY_OPEN_V1 0x4f535131U
#define SEQ_IDENTITY_WAIT_BOUND 1000000U

/* -- PROTIMER real enable sequence, independently tested safe
 * (tests/protimer-probe/, SS7.32-SS7.34); EN is already 1 from
 * radio_setup() above (SS7.39), so it is not written again. -- */
#define PROTIMER_CTRL (PROTIMER_BASE + 0x008U)
#define PROTIMER_CTRL_VAL 0x00011102U
#define PROTIMER_CMD (PROTIMER_BASE + 0x00cU)
#define PROTIMER_CMD_START 0x00000001U
#define PROTIMER_REG_030 (PROTIMER_BASE + 0x030U)
#define PROTIMER_REG_030_VAL 0x00001233U
#define PROTIMER_REG_038 (PROTIMER_BASE + 0x038U)
#define PROTIMER_REG_038_VAL 0x80015558U
#define PROTIMER_IEN (PROTIMER_BASE + 0x070U)
#define PROTIMER_IF_CLR (PROTIMER_BASE + 0x2064U)
/* 0xFFFFFFFF, same blanket sweep as SS7.34 (IF/IEN bit meanings are unresolved). */
#define PROTIMER_ALL_SOURCES 0xffffffffU

#define NVIC_ISER1 0xe000e104U
#define NVIC_IRQ36_BIT (1U << 4)

/* Fresh 32KB SRAM, no Zephyr coexistence. */
#define INITIAL_SP 0x20007000U
#define FIXED_EXCEPTIONS 16
#define IRQ36 36
#define VECTORS 64 /* IRQ0..IRQ47 */

struct replay_store {
	uint32_t address;
	uint32_t value;
};

static const struct replay_store replay_table[] = {
#include "replay-table.inc"
};

void reset(void);
static void irq36_handler(void);

static void fault(void)
{
	for (;;) {
	}
}

__attribute__((section(".vectors"), used)) static void (*const vectors[VECTORS])(void) = {
	(void (*)(void))INITIAL_SP,
	reset,
	[2 ... FIXED_EXCEPTIONS + IRQ36 - 1] = fault,
	[FIXED_EXCEPTIONS + IRQ36] = irq36_handler, /* IRQ36: PROTIMER */
	[FIXED_EXCEPTIONS + IRQ36 + 1 ... VECTORS - 1] = fault,
};

static inline void dsb(void)
{
	__asm__ volatile("dsb" ::: "memory");
}

/* Linked first at 0x100: README.md's procedure breakpoints the reset handler there. */
__attribute__((section(".text.reset"))) void reset(void)
{
	static const uint32_t radio_blocks[] = {
		AGC_BASE, BUFC_BASE, MODEM_BASE, RFCRC_BASE,
		FRC_BASE, PROTIMER_BASE, RAC_BASE, SYNTH_BASE,
	};
	uint32_t state;

	__asm__ volatile("cpsid i" ::: "memory");

	/* CMU: radio_setup()'s clock bring-up. */
	REG(CMU_RADIOCLKCTRL_SET) = CMU_RADIOCLKCTRL_EN;
	REG(CMU_CLKEN1_SET) = CMU_RADIO_BLOCKS;
	dsb();

	/* radio_setup(): eight EN.EN writes, same order as radio.c. */
	for (unsigned int i = 0; i < sizeof(radio_blocks) / sizeof(radio_blocks[0]); i++) {
		REG(radio_blocks[i] + EFR32_RADIO_EN_OFFSET) = 1U;
	}

	/* radio_reset(): hold the sequencer in reset. The real M0+ image is
	 * preloaded into SEQRAM by the debugger before this image runs, so
	 * there is no copy loop here (unlike efr32_radio_sequencer_install()
	 * in radio.c, which copies it from flash at this exact point). */
	REG(RAC_CTRL) = RAC_CTRL_HOLD_VAL;
	dsb();

	/* SR0/SR1/SR2 = 0 (SR3 deliberately untouched, same as radio.c). */
	REG(RAC_SR0) = 0U;
	REG(RAC_SR1) = 0U;
	REG(RAC_SR2) = 0U;

	REG(RAC_CTRL_SET) = RAC_CTRL_FORCEDISABLE;
	REG(RAC_SEQIF_SET) = RAC_SEQIF_STIMCMPEVSEQ;

	/* Wait for RAC.STATUS.STATE to settle to OFF(0) or SHUTDOWN(13). */
	do {
		state = (REG(RAC_STATUS) >> RAC_STATUS_STATE_SHIFT) & RAC_STATUS_STATE_MASK;
	} while (state != RAC_STATE_OFF && state != RAC_STATE_SHUTDOWN);

	REG(RAC_SEQCTRL) = RAC_SEQCTRL_STAGE2_STARTUP;

	/* Release: base-register replacement, then clear FORCEDISABLE via
	 * its CLR alias, exactly as radio_reset()'s own comment describes. */
	REG(RAC_CTRL) = RAC_CTRL_RELEASE_VAL;
	REG(RAC_CTRL_CLR) = RAC_CTRL_FORCEDISABLE;
	dsb();

	/* Wait (bounded) for the open sequencer to publish its identity. */
	for (uint32_t i = 0; i < SEQ_IDENTITY_WAIT_BOUND; i++) {
		if (REG(SEQ_IDENTITY_ADDRESS) == SEQ_IDENTITY_OPEN_V1) {
			break;
		}
	}

	/* Replay the 184 real, directly-captured MMIO stores (radio_phy_set
	 * through the real TXEN store, not inclusive -- see replay-table.inc). */
	for (unsigned int i = 0; i < sizeof(replay_table) / sizeof(replay_table[0]); i++) {
		REG(replay_table[i].address) = replay_table[i].value;
	}

	/* PROTIMER's real enable sequence (EN already 1 from radio_setup()
	 * above). */
	REG(PROTIMER_CTRL) = PROTIMER_CTRL_VAL;
	REG(PROTIMER_REG_030) = PROTIMER_REG_030_VAL;
	REG(PROTIMER_REG_038) = PROTIMER_REG_038_VAL;
	REG(PROTIMER_IEN) = PROTIMER_ALL_SOURCES;
	REG(PROTIMER_CMD) = PROTIMER_CMD_START;

	/* A real vector is installed above (unlike SS7.32's deliberately
	 * masked test) -- safe to unmask IRQ36 now. */
	REG(NVIC_ISER1) = NVIC_IRQ36_BIT;
	__asm__ volatile("cpsie i" ::: "memory");

	/* The real RAC.CMD.TXEN write (not captured by the trace, which
	 * stops just before executing it). */
	REG(RAC_CMD) = RAC_CMD_TXEN;

	for (;;) {
		__asm__ volatile("wfi");
	}
}

/* Ack everything pending (blanket, same rationale as SS7.34 -- IF/IEN
 * bit meanings are unresolved) and return. */
static void irq36_handler(void)
{
	REG(PROTIMER_IF_CLR) = PROTIMER_ALL_SOURCES;
}
