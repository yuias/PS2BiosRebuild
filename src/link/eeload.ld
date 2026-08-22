/* EELOAD is raw code at the fixed address the kernel stages it to
   (docs/analysis/41, docs/spec/04 EE-9a): linked there, lifted out as bytes,
   and entered at its first word. The bss is not in the file; the entry
   clears it between the symbols below. */
ENTRY(_eeload_start)
SECTIONS
{
    . = 0x00082000;
    .text : { *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data : { *(.data .data.*) }
    _gp = . + 0x7FF0;
    _bss_start = ALIGN(16);
    .bss (NOLOAD) : { *(.bss .bss.*) *(COMMON) }
    _bss_end = ALIGN(16);
    /DISCARD/ : { *(.reginfo) *(.MIPS.abiflags) *(.comment) }
}
