/* SPDX-License-Identifier: Apache-2.0 */
ENTRY(reset)
SECTIONS
{
  . = 0x00000000;
  .vectors : { KEEP(*(.vectors)) } = 0
  .text 0x00000100 : { *(.text*) *(.rodata*) }
  /DISCARD/ : { *(.ARM.attributes) *(.comment) }
}
