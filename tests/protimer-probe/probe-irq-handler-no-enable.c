/* SPDX-License-Identifier: Apache-2.0 */
/*
 * probe-irq-handler.c's isolation variant: the same table copy, VTOR redirect and PROTIMER
 * sequence, but IRQ36 stays masked (docs/radio-tables.md SS7.35).
 */
#define PROBE_IRQ36_UNMASKED 0
#define PROBE_ENTRY_NAME protimer_probe_irq_no_enable
#include "probe-irq-handler.c"
