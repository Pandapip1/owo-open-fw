/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Tiny diagnostic: read VTOR from the CPU's own (Non-secure) perspective
 * and report it via the usual marker mechanism, rather than trusting a
 * debugger-side read of 0xe000ed08 (which may resolve to the Secure-banked
 * copy instead of the Non-secure one this firmware actually runs under).
 * Standalone, no Zephyr. See tests/protimer-probe/README.md.
 */
#include "probe.h"

/* The marker carries VTOR's value instead of SCRATCH_DONE. */
static __attribute__((used, noinline)) uint32_t protimer_probe_read_vtor_body(void)
{
	return REG(VTOR);
}

PROBE_ENTRY(protimer_probe_read_vtor, protimer_probe_read_vtor_body, "")
