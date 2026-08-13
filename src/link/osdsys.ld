/* An EE program, linked at the address it runs from: the kernel's loader
   places each segment where the ELF says and enters at e_entry. Well clear of
   the kernel at physical zero and of the stack the boot leaves. */
ENTRY(_osd_start)
SECTIONS
{
    . = 0x00200000;
    .text : { *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data : { *(.data .data.*) }
    .bss (NOLOAD) : { *(.bss .bss.*) *(COMMON) }
    /DISCARD/ : { *(.reginfo) *(.MIPS.abiflags) *(.comment) }
}
