/* IOPBOOT is linked at the address its own archive offset gives it. That
   offset depends only on the entries stored before it, so CMakeLists.txt
   computes it with a pre-pass over the manifest -- before this file's own
   build exists to be measured -- and hands it in as the `IOPBOOT_BASE`
   `--defsym` (spec/03 BOOT-7a). A post-build check in `tools/mkromdir.py`
   confirms the final image agrees. */
ENTRY(iopboot)
SECTIONS
{
    . = IOPBOOT_BASE;
    .text : { *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    /DISCARD/ : { *(.reginfo) *(.MIPS.abiflags) *(.comment) *(.pdr)
                  *(.mdebug*) *(.gnu.attributes) *(.llvm_addrsig) }
}
