/* SPDX-License-Identifier: Apache-2.0 */
ENTRY(efr32_radio_seq_reset)
SECTIONS
{
  . = 0;
  .vectors : { KEEP(*(.vectors)) }
  .text : { *(.text*) *(.rodata*) }
  /DISCARD/ : { *(.ARM.attributes) *(.comment) }
}

/* The top 512 bytes are reserved for the stack and shared ABI words. */
ASSERT(SIZEOF(.vectors) + SIZEOF(.text) <= 0x3e00,
       "open radio sequencer firmware overlaps its stack")
