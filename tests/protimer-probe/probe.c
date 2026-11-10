/* SPDX-License-Identifier: Apache-2.0 */
/*
 * EFR32BG22 PROTIMER enable probe.
 *
 * This is standalone Cortex-M33 code, independent of Zephyr and the real
 * driver (modules/efr32-radio/hal/radio.c). It is NOT a bootable image: it
 * has no vector table and is meant to be injected into SRAM and executed
 * from a halt-point inside the already-running, already-correctly-
 * configured production firmware (same SRAM-injection technique
 * scripts/bg22-openocd-flash.cfg's bg22_require_sram_execution already uses
 * reliably), right before that firmware's own RAC.CMD.TXEN write.
 *
 * WHY a separate probe instead of extending radio.c directly: the question
 * being tested ("does enabling PROTIMER, with no scheduling logic at all,
 * change FRC's behavior") doesn't need the real driver's own TX-scheduling
 * path touched at all, and hand-replicating the ~30-write PHY config table
 * in a from-scratch bare-metal image would risk exactly the kind of
 * transcription error this project has caught before (see
 * docs/radio-tables.md SS7.13's PROTIMER_0x38/FRC_0x1C mixup). Reusing the
 * real, already-correct firmware for everything except the one new
 * variable (PROTIMER) isolates that variable cleanly.
 *
 * Deliberately leaves NVIC IRQ36 (PROTIMER) masked: this probe tests only
 * whether a *running, clocked* PROTIMER changes FRC's behavior, not
 * whether servicing its interrupt does. Enabling the IRQ without a real
 * vector-table entry (Zephyr's table is flash-resident; this probe cannot
 * safely patch it at runtime) is what crashed the earlier live register
 * poke (docs/radio-tables.md SS7.32). A follow-up probe that *does* want to
 * test the IRQ-handshake sub-hypothesis needs a real installed handler,
 * which needs either a from-scratch bootable image (like
 * tests/hardware-lockstep/m33-seq-txwarm.c) or Zephyr's own
 * IRQ_CONNECT()/software-ISR-table mechanism -- not attempted here.
 *
 * Register values below are the exact sequence stock is observed to issue
 * at PROTIMER bring-up (docs/radio-tables.md SS7.9's "radio-trace.log:64-67"
 * citation and generic_phy.o's enable helper at flash offset 0x29a3a,
 * cross-confirmed independently this session via tests/hardware-lockstep's
 * stage2-pre-txen-protimer.bin capture, see SS7.32). What each bit/field
 * actually means is NOT established (the cited docs are explicit that
 * PROTIMER's command/IF/IEN semantics remain unresolved) -- this probe
 * replicates the observed write *sequence* as a best-effort "do what stock
 * does", not a field-decoded, understood configuration. Do not copy these
 * values into production code without first resolving that gap.
 */
#include "probe.h"

static __attribute__((used, noinline)) uint32_t protimer_probe_enable_body(void)
{
	protimer_configure();
	REG(PROTIMER_CMD) = PROTIMER_CMD_START;
	/* Deliberately no NVIC enable here -- see file header. */
	return SCRATCH_DONE;
}

PROBE_ENTRY(protimer_probe_enable, protimer_probe_enable_body, "cpsid i\n")
