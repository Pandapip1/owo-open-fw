/* SPDX-License-Identifier: Apache-2.0 */
/*
 * EFR32BG22 PROTIMER real-IRQ-servicing probe.
 *
 * Follow-up to probe.c/probe-ien-sweep.c (see docs/radio-tables.md
 * SS7.32-7.34). Those ruled out "PROTIMER just needs to be clocked" and
 * "PROTIMER just needs to be pending" -- this tests the remaining
 * question: does FRC need the CPU to actually take the IRQ36 exception
 * and run real handler logic, not just have the condition latched?
 *
 * Zephyr's vector table here is flash-resident (VTOR_NS reads 0, the
 * linked address of `_vector_table`) and this build has no
 * CONFIG_DYNAMIC_INTERRUPTS (`_sw_isr_table` is also flash/const), so
 * there is no simple runtime patch target. This probe instead copies the
 * live 64-entry (256-byte) vector table into SRAM, overwrites only the
 * IRQ36 entry (index 16+36=52) to point at a trivial handler also placed
 * in SRAM, and repoints VTOR_NS at the copy -- so every other exception
 * (SysTick, UART, etc.) keeps using the exact same handlers as before,
 * unaffected.
 *
 * RAM CAVEAT, recorded honestly rather than silently assumed safe: this
 * firmware links at ~99.68% SRAM utilization (ninja's own build output).
 * There is no genuinely free scratch region. 0x200007d0 (this probe's
 * own code) matches `bt_workq`, the same region every probe in this
 * project uses. The 256-byte copied table was first tried at
 * 0x20000900 -- that landed inside the GATT-related `acl_conns`/
 * `db_hash`/`gatt_sc` globals and produced a confounded, invalidated
 * result (the TX attempt aborted regardless of whether the interrupt
 * was ever taken -- docs/radio-tables.md SS7.35). Moved to
 * 0x20007d00: 256-byte-aligned (VTOR's low bits are hardware-forced to
 * 0 for anything less) and entirely within the HCI-command/ACL-TX
 * net_buf pools (`_net_buf_hci_cmd_pool`/`_net_buf_fragments`/
 * `net_buf_data_acl_tx_pool`/`_net_buf_acl_tx_pool`, confirmed via
 * `nm -S`), which this connectionless, no-HCI-command, no-connection
 * smoke test should never touch. Still a real, not fully eliminated
 * risk (there is no genuinely free SRAM anywhere in this firmware),
 * not a verified absence of side effects.
 *
 * The handler's address is taken at link time, so this probe links at
 * its injection address (link-loaded-at-0x200007d0.ld).
 */
#include "probe.h"

#ifndef PROBE_IRQ36_UNMASKED
#define PROBE_IRQ36_UNMASKED 1
#define PROBE_ENTRY_NAME protimer_probe_irq_handler
#endif

/*
 * Ack everything pending (blanket, same rationale as the CLR-alias test in SS7.34 -- bit
 * meanings are unresolved) and return.
 */
static __attribute__((used)) void trivial_irq36_handler(void)
{
	REG(PROTIMER_IF_CLR) = PROTIMER_ALL_SOURCES;
}

static __attribute__((used, noinline)) uint32_t protimer_probe_irq_handler_body(void)
{
	volatile uint32_t *table = copy_vector_table();

	/* Patch index 52 (IRQ36) to point at the trivial handler above. */
	table[IRQ36_VECTOR_INDEX] = (uint32_t)(uintptr_t)trivial_irq36_handler;
	/* Repoint VTOR_NS at the SRAM copy. */
	REG(VTOR) = NEW_VECTOR_TABLE;
	__asm__ volatile("dsb\n\tisb" ::: "memory");

	/* PROTIMER enable sequence (same values as probe.c/probe-ien-sweep.c). */
	protimer_configure();
	REG(PROTIMER_IEN) = PROTIMER_ALL_SOURCES;
	REG(PROTIMER_CMD) = PROTIMER_CMD_START;

#if PROBE_IRQ36_UNMASKED
	/* Now safe to unmask IRQ36: a real handler is installed above. */
	REG(NVIC_ISER1) = NVIC_IRQ36_BIT;
#else
	/*
	 * Isolation variant: deliberately do NOT unmask IRQ36 here, to check whether the VTOR
	 * redirect/table-copy alone (without ever taking the exception) reproduces
	 * probe-irq-handler.c's abort result -- see docs/radio-tables.md SS7.35.
	 */
#endif
	return SCRATCH_DONE;
}

PROBE_ENTRY(PROBE_ENTRY_NAME, protimer_probe_irq_handler_body, "cpsid i\n")
