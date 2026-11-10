/* SPDX-License-Identifier: Apache-2.0 */
/* The harness entry at HARNESS_ENTRY (src/harness.h), below HARNESS_BLOCK. */
ENTRY(harness_irq)
SECTIONS
{
  . = 0x3000;
  .text : { KEEP(*(.entry)) *(.text*) *(.rodata*) }
  /DISCARD/ : { *(.ARM.attributes) *(.comment) }
}
ASSERT(. <= 0x3400, "harness entry overlaps its control block")
