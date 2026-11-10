/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Isolation probe: does ONLY the vector-table copy + VTOR redirect from
 * probe-irq-handler.c, with NO PROTIMER touches and NO NVIC changes at
 * all. Checks whether the abort-to-OFF result seen at two different
 * copy destinations (0x20000900, then 0x20007d00) is caused by the
 * VTOR mechanism itself rather than by corrupting whatever memory
 * happens to sit at the destination -- see docs/radio-tables.md SS7.36.
 */
#include "probe.h"

static __attribute__((used, noinline)) uint32_t protimer_probe_vtor_only_body(void)
{
	copy_vector_table();
	REG(VTOR) = NEW_VECTOR_TABLE;
	__asm__ volatile("dsb\n\tisb" ::: "memory");
	return SCRATCH_DONE;
}

PROBE_ENTRY(protimer_probe_vtor_only, protimer_probe_vtor_only_body, "cpsid i\n")
