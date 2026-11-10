/* SPDX-License-Identifier: Apache-2.0 */
/*
 * EFR32BG22 PROTIMER IEN sweep probe.
 *
 * Follow-up to probe.c (see that file's header and
 * docs/radio-tables.md SS7.33 for the base enable sequence and its
 * clean-negative result). PROTIMER.IEN (+0x070) reads 0 at baseline --
 * this probe additionally writes it all-ones, to check whether PROTIMER
 * ever latches an NVIC-pending IRQ36 request at all once *something* in
 * its interrupt-enable mask is set, before investing in the much larger
 * "install a real handler via a VTOR-redirected SRAM vector table" probe
 * that full IRQ-servicing testing would need.
 *
 * All-ones is a blanket sweep, not a field-decoded value: PROTIMER's
 * IF/IEN bit meanings are explicitly unresolved (docs/radio-tables.md
 * SS7.9), so this does not claim to understand or correctly configure
 * any specific named interrupt source -- it only answers "does anything
 * latch pending at all", which is checked via NVIC's pending register
 * (safe to read regardless of enable state) rather than by actually
 * unmasking the interrupt.
 *
 * Standalone, no Zephyr, no vector table -- same SRAM-injection usage as
 * probe.c (see tests/protimer-probe/README.md).
 */
#include "probe.h"

static __attribute__((used, noinline)) uint32_t protimer_probe_ien_sweep_body(void)
{
	protimer_configure();
	REG(PROTIMER_IEN) = PROTIMER_ALL_SOURCES;
	REG(PROTIMER_CMD) = PROTIMER_CMD_START;
	/* Deliberately no NVIC enable -- see file header. */
	return SCRATCH_DONE;
}

PROBE_ENTRY(protimer_probe_ien_sweep, protimer_probe_ien_sweep_body, "cpsid i\n")
