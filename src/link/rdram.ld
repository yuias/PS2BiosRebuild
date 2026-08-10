/* Reached by name and called where it lies in the ROM, so its address is not
   fixed and the code must not depend on one. */
ENTRY(_rdram)
SECTIONS
{
    . = 0;
    .text : { *(.text) }
    /DISCARD/ : { *(.reginfo) *(.MIPS.abiflags) *(.comment) }
}
