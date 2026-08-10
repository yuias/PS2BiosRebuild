/* An IOP module is linked at virtual address 0 and relocated by the loader
   (spec/02 IRX-1). The linker is asked to keep its fixups with --emit-relocs;
   `tools/mkirx.py` turns what comes out into the module container. */
ENTRY(_module_start)
SECTIONS
{
    . = 0;
    .text : { *(.text) }
    .rodata : { *(.rodata) }
    .data : { *(.data) }
    .bss (NOLOAD) : { *(.bss) *(COMMON) }
    /DISCARD/ : { *(.reginfo) *(.MIPS.abiflags) *(.comment) }
}
