/* SPDX-License-Identifier: Apache-2.0 */
ENTRY(reset)
SECTIONS
{
  . = 0;
  .vectors : { KEEP(*(.vectors)) }
  .text : { *(.text*) *(.rodata*) }
  /DISCARD/ : { *(.ARM.attributes) *(.comment) }
}
ASSERT(SIZEOF(.vectors) + SIZEOF(.text) <= 0x3e00,
       "sequencer probe overlaps its stack")
