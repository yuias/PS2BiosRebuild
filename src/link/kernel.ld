/* KERNEL is copied to physical zero and entered at 0x80001000, so its first
   0x1000 bytes are the EE exception vector page (spec/04 EE-4). The tables
   below sit at the addresses EE-6f fixes; the gaps between sections are what
   objcopy fills with zeroes, and the file is sized by the last of them. */
ENTRY(_kernel_entry)
SECTIONS
{
    . = 0x80000000;
    .vectors : { *(.vectors) }

    . = 0x80001000;
    .text : { *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }

    . = 0x80014F40;                 /* EE-8a: the syscall table */
    .syscalls : { *(.syscalls) }

    . = 0x80015340;                 /* EE-6a: the exception table */
    .exctable : { *(.exctable) }

    . = 0x80015380;                 /* EE-6d: the interrupt table */
    .inttable : { *(.inttable) }

    . = 0x800154A8;                 /* EE-6f: the CP0 register read stubs */
    .cop0reads : { *(.cop0reads) }

    /* Beyond the image: scratch the kernel uses but does not ship. */
    .bss (NOLOAD) : { *(.bss .bss.*) *(COMMON) }

    /DISCARD/ : { *(.reginfo) *(.MIPS.abiflags) *(.comment) }
}
