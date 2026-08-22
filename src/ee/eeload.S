// EELOAD's entry: the shape an emulator's hook expects, then a C++ main.
//
// docs/analysis/41: EELOAD is raw R5900 code at 0x00082000, entered by the
// kernel with EPC pointed at it and the launcher's registers. PCSX2 does not
// load it or look for it in the archive; it watches the CPU reach 0x82000,
// reads the instruction at +0x9C, and if it is a `jal` takes its target as
// `main` -- the function it hooks to learn, or to substitute, the program's
// name. So this file's one hard requirement is that the word at +0x9C is
// `jal main`, and that `main` is reached with argc in $a0 and argv in $a1.
//
// Everything before it is what any EE program does first (spec/05 SYS-8a/b):
// ask slot 0x3C for a stack and the argument block, clear the bss -- a raw
// image carries none -- and read argc/argv out of the block.

    .set noreorder
    .set EELOAD_STACK_SIZE, 0x4000

    .globl _eeload_start
    .section .text, "ax"

_eeload_start:
    // The bss: from its start to its end, sixteen bytes at a time, before
    // anything that lives in it is touched.
    lui   $t0, %hi(_bss_start)
    addiu $t0, $t0, %lo(_bss_start)
    lui   $t1, %hi(_bss_end)
    addiu $t1, $t1, %lo(_bss_end)
1:
    sltu  $t2, $t0, $t1
    beq   $t2, $zero, 2f
    nop
    sw    $zero, 0($t0)
    sw    $zero, 4($t0)
    sw    $zero, 8($t0)
    sw    $zero, 12($t0)
    b     1b
    addiu $t0, $t0, 16
2:
    lui   $gp, %hi(_gp)
    addiu $gp, $gp, %lo(_gp)
    move  $a0, $gp
    lui   $a1, %hi(eeload_stack)
    addiu $a1, $a1, %lo(eeload_stack)
    lui   $a2, %hi(EELOAD_STACK_SIZE)
    addiu $a2, $a2, %lo(EELOAD_STACK_SIZE)
    lui   $a3, %hi(eeload_args)
    addiu $a3, $a3, %lo(eeload_args)
    lui   $t0, %hi(eeloadHalt)           // root, in $t0 (SYS-1b)
    addiu $t0, $t0, %lo(eeloadHalt)
    addiu $v1, $zero, 0x3C
    syscall
    move  $sp, $v0                       // SYS-8a: the answer is our stack
    lui   $t1, %hi(eeload_args)
    addiu $t1, $t1, %lo(eeload_args)
    lw    $a0, 0($t1)                    // SYS-8b: argc, then argv
    addiu $a1, $t1, 4
    // Pad to +0x9C: the hook reads `jal main` from exactly there.
    .fill (0x9C - (. - _eeload_start)) / 4, 4, 0
    jal   eeloadMain                     // +0x9C
    nop
3:
    b     3b                             // main never returns
    nop
