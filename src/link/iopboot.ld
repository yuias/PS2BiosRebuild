/* IOPBOOT runs in place from the ROM window at whatever address the archive
   gives it, so the code is position-independent and this address is only a
   link-time convenience (spec/03 BOOT-7). */
ENTRY(_iopboot)
SECTIONS
{
    . = 0;
    .text : { *(.text) }
    /DISCARD/ : { *(.reginfo) *(.MIPS.abiflags) *(.comment) }
}
