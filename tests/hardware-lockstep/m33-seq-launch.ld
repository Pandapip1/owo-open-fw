ENTRY(reset)
MEMORY
{
    FLASH (rx) : ORIGIN = 0x00000000, LENGTH = 512K
}
SECTIONS
{
    .vectors 0x00000000 : { KEEP(*(.vectors)) } > FLASH
    .text 0x00000100 : { *(.text*) *(.rodata*) } > FLASH
}
