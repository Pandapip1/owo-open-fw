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
#include <stdint.h>

#define REG(addr) (*(volatile uint32_t *)(addr))

#define SEQ_SPARE0 0x00004fe0U
#define RAC_STATUS 0xa802000cU
#define RAC_IF 0xa802001cU
#define RAC_SEQIF 0xa8020028U
#define RAC_IF_CLR 0xa802201cU
#define RAC_SEQIF_CLR 0xa8022028U
#define RAC_SEQIEN_SET 0xa802102cU
/* Stock secure startup prefix, retained as a characterization fixture. */
#define RAC_CTRL_SET 0xa8021014U
#define RAC_CTRL_CLR 0xa8022014U
#define RAC_SEQIF_SET 0xa8021028U
#define RAC_SEQCTRL 0xa8020038U
#define RAC_SR3 0xa802004cU
#define RAC_SR3_SET 0xa802104cU
#define RAC_SR3_CLR 0xa802204cU
#define RAC_CTRL_POLARITIES 0x00000380U
#define RAC_CTRL_FORCEDISABLE 0x00000001U
#define RAC_CTRL_CPUWAITDIS 0x04000000U
#define RAC_SEQIF_STIMCMPEVSEQ 0x00000002U
#define RAC_SEQIF_STATE_EVENTS 0x3fff0000U
#define OBS_RAC_SEQCTRL_STARTUP 0x00000011U
#define OBS_RAC_SR3_STARTUP 0x00f04800U
#define OBS_RAC_SR3_STARTUP_CLR 0x00004000U
#define OBS_RAC_SR3_STARTUP_SET 0x00000200U
/* IRQ8 prologue observed live through AP3 on 2026-10-27.  The vendor handler
 * pulses bit 18 through the SET alias, waits for the base bit to clear, and
 * then writes the same bit through the CLR alias before radio setup. */
#define OBS_IRQ8_GATE_BASE 0xa8020040U
#define OBS_IRQ8_GATE_SET 0xa8021040U
#define OBS_IRQ8_GATE_CLR 0xa8022040U
#define OBS_IRQ8_GATE_BIT 0x00040000U
/*
 * The bit the wait actually polls in the base register: bit 2, not OBS_IRQ8_GATE_BIT,
 * as in the original asm (movs r7, #4; tst).
 */
#define OBS_IRQ8_GATE_POLL 0x00000004U
/* Write-only IRQ8 actions observed at proprietary routine 0x1bda.  Their
 * semantic field names are not yet established, so keep that uncertainty
 * explicit instead of pretending these are production register definitions. */
#define OBS_IRQ8_CLR_2044 0xa8022044U
#define OBS_IRQ8_CLR_2040 0xa8022040U
#define OBS_IRQ8_CLR_2008 0xa8022008U
#define NVIC_ISER 0xe000e100U
#define NVIC_ICPR 0xe000e280U
#define TRACE_BASE 0x00003000U
#define TRACE_INDEX 0x00004fdcU
#define TRACE_RECORDS 16U
#define TRACE_RECORD_WORDS 4U

#define RST_MARKER 0x52535421U /* "RST!" */
#define ALL_IRQS 0xffffffffU
#define EXC_EXTERNAL_IRQ8 24U /* TXWARM */
#define EXC_EXTERNAL_IRQ9 25U /* TX */

#define STACK_TOP 0x00003e00U
#define VECTORS 64

#ifndef STOP_EXCEPTION
#define STOP_EXCEPTION 0
#endif

void reset(void);
void exception(void);

__attribute__((section(".vectors"), used)) static void (*const vectors[VECTORS])(void) = {
	(void (*)(void))STACK_TOP,
	reset,
	[2 ... VECTORS - 1] = exception,
};

void reset(void)
{
	__asm__ volatile("cpsid i" ::: "memory");
	/* Stock reaches OFF at this exact M0-owned peripheral boundary.  These
	 * are the public RAC sequencer state-event flags, not an M33 scheduling
	 * operation; clear stale state indications before enabling handlers. */
	REG(RAC_SEQIF_CLR) = RAC_SEQIF_STATE_EVENTS;
	REG(RAC_SEQIEN_SET) = RAC_SEQIF_STATE_EVENTS;
#if !SKIP_WAKE_PREFIX
	/* Execute the secure-address wake prefix from the sequencer core.  This
	 * tests ownership of SHUTDOWN exit; these observed presets are not a
	 * production interface. */
	REG(RAC_SR3) = OBS_RAC_SR3_STARTUP;
	REG(RAC_SR3_CLR) = OBS_RAC_SR3_STARTUP_CLR;
	REG(RAC_CTRL_SET) = RAC_CTRL_POLARITIES;
	REG(RAC_SR3_SET) = OBS_RAC_SR3_STARTUP_SET;
	REG(RAC_CTRL_SET) = RAC_CTRL_FORCEDISABLE;
	REG(RAC_SEQIF_SET) = RAC_SEQIF_STIMCMPEVSEQ;
	REG(RAC_SEQCTRL) = OBS_RAC_SEQCTRL_STARTUP;
	REG(RAC_CTRL_SET) = RAC_CTRL_CPUWAITDIS;
	REG(RAC_CTRL_CLR) = RAC_CTRL_FORCEDISABLE;
#endif

	REG(SEQ_SPARE0) = RST_MARKER;
	REG(TRACE_INDEX) = 0U;

	REG(NVIC_ISER) = ALL_IRQS;
	REG(NVIC_ISER + 4U) = ALL_IRQS;
	REG(NVIC_ICPR) = ALL_IRQS;
	REG(NVIC_ICPR + 4U) = ALL_IRQS;
	__asm__ volatile("cpsie i" ::: "memory");

	for (;;) {
		__asm__ volatile("wfi");
	}
}

/* Preserve the ordering of vendor helpers 0x7ac and 0x7be.  Endpoint
 * snapshots miss this pulse because SET/CLR are write-only aliases. */
static void irq8_txwarm(void)
{
	REG(OBS_IRQ8_GATE_SET) = OBS_IRQ8_GATE_BIT;
	while (REG(OBS_IRQ8_GATE_BASE) & OBS_IRQ8_GATE_POLL) {
	}
	REG(OBS_IRQ8_GATE_CLR) = OBS_IRQ8_GATE_BIT;
	REG(0xa8020014U) = 0x04000380U;
	REG(0xa802005cU) = 0x00070001U;
	REG(0xa802012cU) = 0x1e23221dU;
	REG(0xa802013cU) = 5U;
	REG(0xa800c00cU) = 0x00008880U;
	REG(0xa800c018U) = 0x0000c940U;
	REG(0xa8014008U) = 0U;
	REG(0xa80140e8U) = 0x3ffb0001U;
	REG(0xa801804cU) = 0x000081e7U;
	/* Endpoint snapshots cannot reveal writes through the CLR alias.  These
	 * three actions reproduce the directly disassembled IRQ8 helper. */
	REG(OBS_IRQ8_CLR_2044) = 3U;
	REG(OBS_IRQ8_CLR_2040) = 168U;
	REG(OBS_IRQ8_CLR_2008) = 16U;
}

static void irq9_tx(void)
{
	REG(0xa8020040U) = 0x00010000U;
	REG(0xa8020044U) = 0x00020000U;
	REG(0xa8020094U) = 0x00100200U;
	REG(0xa80200a8U) = 1U;
	REG(0xa80200acU) = 0x00009408U;
	REG(0xa80200b4U) = 0x10000000U;
	REG(0xa8020108U) = 1U;
	REG(0xa8020114U) = 0x00038997U;
	REG(0xa802011cU) = 0x00322d2aU;
	REG(0xa802012cU) = 0x1e22221eU;
	REG(0xa802013cU) = 7U;
	REG(0xa8020140U) = 20U;
	REG(0xa8020144U) = 16U;
	REG(0xa8020148U) = 0U;
	REG(0xa8020150U) = 0x00066e28U;
	REG(0xa8020154U) = 0x00003fc4U;
	REG(0xa802015cU) = 0x00001fbdU;
	REG(0xa8020160U) = 0x00000400U;
	REG(0xa8020168U) = 0x00000400U;
	REG(0xa8020188U) = 0x000007f3U;

	REG(0xa8004008U) = 0x01700220U;
	REG(0xa8004070U) = 0x01380000U;
	REG(0xa80040b4U) = 0x40020e01U;
	REG(0xa800c018U) = 0x0000d800U;
	REG(0xa80140e8U) = 0x3fda8090U;
	REG(0xa8018008U) = 0x04000005U;
	REG(0xa801802cU) = 165U;
	REG(0xa801804cU) = 0x00008335U;
	REG(0xa8018054U) = 0x00000158U;
	REG(0xa801808cU) = 0x00000fa4U;
	REG(0xa8018090U) = 0x00000fa3U;
	REG(0xa801001cU) = 0x00aaaaaaU;
	REG(0xaa000018U) = 36U;
	REG(0xaa000020U) = 8U;
	REG(0xaa00002cU) = 0U;
	/* The live stage-2 TX handler then requests its post-advertising receive
	 * state and applies the corresponding analog tail before STATETX is
	 * acknowledged. */
	REG(0xa8020008U) = 2U;
	REG(0xa802012cU) = 0x1e23221dU;
	REG(0xa800c018U) = 0x0000c700U;
	REG(0xa80140e8U) = 0x3fff8008U;
	REG(0xa801808cU) = 0x00000fa5U;
}

void exception(void)
{
	volatile uint32_t *spare = (volatile uint32_t *)SEQ_SPARE0;
	volatile uint32_t *record;
	uint32_t exc, seqif, status, ifs, index;

	/* Stays asm: the active exception number has no C expression. */
	__asm__ volatile("mrs %0, ipsr" : "=r"(exc));
	spare[0] = exc;
	spare[1] = REG(RAC_STATUS);
	spare[2] = REG(RAC_IF);
	seqif = REG(RAC_SEQIF);
	spare[3] = seqif;
	index = REG(TRACE_INDEX);
	REG(TRACE_INDEX) = index + 1U;
	record = (volatile uint32_t *)TRACE_BASE +
		 (index % TRACE_RECORDS) * TRACE_RECORD_WORDS;
	record[0] = exc;
	record[1] = seqif;
	status = REG(RAC_STATUS);
	ifs = REG(RAC_IF);
	record[2] = status;
	record[3] = ifs;

	if (exc == STOP_EXCEPTION) {
		for (;;) {
		}
	}

	if (exc == EXC_EXTERNAL_IRQ8) {
		irq8_txwarm();
		/*
		 * The asm original's gate poll reused the register that held RAC.IF, so after
		 * this path it acknowledged RAC.IF with the poll mask instead. Kept, so the
		 * probe behaves as it did on silicon.
		 */
		ifs = OBS_IRQ8_GATE_POLL;
	}
	if (exc == EXC_EXTERNAL_IRQ9) {
		irq9_tx();
	}

	/* Acknowledge only the register-visible sources, then observe whether
	 * silicon advances to another state/event without vendor firmware. */
	REG(RAC_SEQIF_CLR) = seqif;
	REG(RAC_IF_CLR) = ifs;
}
