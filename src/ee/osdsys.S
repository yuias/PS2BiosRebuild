// The entry contract of an EE program, and nothing else.
//
// The kernel's loader enters at `e_entry` with `argc`/`argv` in place but no
// stack this program owns (spec/04 EE-9). Establishing one is the only thing
// here that cannot be written in C++, because a compiled function needs the
// stack to already exist. Everything the program does is `osdsys.cpp`.

    .set noreorder

    .globl _osd_start
    .section .text, "ax"

_osd_start:
    lui   $sp, %hi(osd_stack + 0x1000)
    addiu $sp, $sp, %lo(osd_stack + 0x1000)
    j     osdMain
    nop
