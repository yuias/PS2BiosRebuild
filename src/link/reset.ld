/* The boot block is mapped at the reset vector on both CPUs (spec/03 BOOT-1). */
ENTRY(_reset)
SECTIONS
{
    . = 0xBFC00000;
    .text : { *(.text .text.*) }
    /* ARC-4: the scan that finds the archive's table accepts a candidate only
       when the RESET entry's size is a multiple of sixteen, so this file's own
       size has to be one. Nothing padded it before; it happened to be aligned. */
    .rodata : { *(.rodata .rodata.*) . = ALIGN(16); }
    /DISCARD/ : { *(.reginfo) *(.MIPS.abiflags) *(.comment) }
}
