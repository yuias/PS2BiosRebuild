/* An IOP module is linked at virtual address 0 and relocated by the loader
   (spec/02 IRX-1). The linker is asked to keep its fixups with --emit-relocs;
   `tools/mkirx.py` turns what comes out into the module container. */
ENTRY(_module_start)
SECTIONS
{
    . = 0;
    /* The export tables come first, at offset zero, because BOOT-8c makes a
       module's load base and its first export table the same address: that is
       how the boot block tells the loader where SYSMEM's is, having no other
       way to name it. The import tables follow the code. Both are inside the
       text segment, which is the only region a loader scans for them
       (spec/02 IRX-8b). */
    .iopexport : { *(.iopexport) }
    .text : { *(.text .text.*) }
    .iopimport : { *(.iopimport) }
    .rodata : { *(.rodata .rodata.*) }
    .data : { *(.data .data.*) }
    .bss (NOLOAD) : { *(.bss .bss.*) *(COMMON) }
    /* What a compiler adds for debuggers and ABI bookkeeping is not part of
       the module: a fixup into it would point at nothing once loaded. */
    /DISCARD/ : { *(.reginfo) *(.MIPS.abiflags) *(.comment) *(.pdr)
                  *(.mdebug*) *(.gnu.attributes) *(.llvm_addrsig) }
}
