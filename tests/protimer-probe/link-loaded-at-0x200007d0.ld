/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Unlike link.ld (based at 0, for probes with no internal self-address
 * references), probe-irq-handler.c takes the real runtime address of
 * its own trivial_irq36_handler, so this script must base .text at
 * its actual injection address (0x200007d0, the same scratch address
 * every probe in this project loads at) for that to resolve correctly.
 */
ENTRY(protimer_probe_irq_handler)
SECTIONS
{
  . = 0x200007d0;
  .text : { KEEP(*(.text.entry)) *(.text*) *(.rodata*) }
  /DISCARD/ : { *(.ARM.attributes) *(.comment) }
}
