/* SPDX-License-Identifier: Apache-2.0 */
ENTRY(protimer_probe_enable)
SECTIONS
{
  . = 0;
  .text : { KEEP(*(.text.entry)) *(.text*) *(.rodata*) }
  /DISCARD/ : { *(.ARM.attributes) *(.comment) }
}
