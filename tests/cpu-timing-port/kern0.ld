/* SPDX-License-Identifier: Apache-2.0 */
/* kern0.c: the M0+ image, at the M0+'s address 0, whole words (main.c copies words). */
ENTRY(k0_reset)
SECTIONS
{
  . = 0;
  .vectors : { KEEP(*(.vectors)) }
  .text : { *(.text*) *(.rodata*) . = ALIGN(4); }
  /DISCARD/ : { *(.ARM.attributes) *(.comment) }
}

/* main.c clears the results from 0x3000 before the release. */
ASSERT(SIZEOF(.vectors) + SIZEOF(.text) <= 0x3000, "M0+ kernel image overlaps its results")
