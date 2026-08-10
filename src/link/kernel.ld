/* KERNEL is copied to physical zero and entered at 0x80001000, so its first
   0x1000 bytes are the EE exception vector page (spec/04 EE-4). */
ENTRY(_kernel_entry)
SECTIONS
{
    . = 0x80000000;
    .vectors : { *(.vectors) }
    . = 0x80001000;
    .text : { *(.text) }
    /DISCARD/ : { *(.reginfo) *(.MIPS.abiflags) *(.comment) }
}
