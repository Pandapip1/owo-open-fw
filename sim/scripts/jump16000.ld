/* SPDX-License-Identifier: Apache-2.0 */
/* jump16000.c: its two-word vector table at 0, then the code. */
ENTRY(reset)
SECTIONS
{
  .text 0 : { KEEP(*(.vectors)) *(.text*) *(.rodata*) }
  /DISCARD/ : { *(.ARM.attributes) *(.comment) }
}
