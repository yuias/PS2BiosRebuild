/* The boot block is mapped at the reset vector on both CPUs (spec/03 BOOT-1). */
ENTRY(_reset)
SECTIONS
{
    . = 0xBFC00000;
    .text : { *(.text) }
    /DISCARD/ : { *(.reginfo) *(.MIPS.abiflags) *(.comment) }
}
