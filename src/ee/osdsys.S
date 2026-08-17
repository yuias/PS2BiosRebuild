// The entry contract of an EE program, and nothing else.
//
// The kernel enters at `e_entry` with the launcher's registers and no stack
// this program owns (spec/04 EE-9e). What a program does first is what any
// runtime does: call slot 0x3C (spec/05 SYS-8a) with a stack, a block for its
// arguments and a root, take the stack pointer it answers with, and read
// `argc` and `argv` out of the block (SYS-8b). It is here rather than in
// `osdsys.cpp` because a compiled function needs the stack to already exist,
// and 0x3C primes the top of the stack it is given, so it must be called
// before anything runs on that stack.

    .set noreorder
    .set OSD_STACK_SIZE, 0x1000          // matches osd_stack in osdsys.cpp

    .globl _osd_start
    .section .text, "ax"

_osd_start:
    move  $a0, $zero                     // gp: this program keeps none
    lui   $a1, %hi(osd_stack)
    addiu $a1, $a1, %lo(osd_stack)
    lui   $a2, %hi(OSD_STACK_SIZE)
    addiu $a2, $a2, %lo(OSD_STACK_SIZE)
    lui   $a3, %hi(osd_args)
    addiu $a3, $a3, %lo(osd_args)
    lui   $t0, %hi(osdHalt)              // root, in $t0 (SYS-1b)
    addiu $t0, $t0, %lo(osdHalt)
    addiu $v1, $zero, 0x3C
    syscall
    move  $sp, $v0                       // SYS-8a: the answer is our stack

    lui   $t1, %hi(osd_args)
    addiu $t1, $t1, %lo(osd_args)
    lw    $a0, 0($t1)                    // SYS-8b: argc, then argv
    j     osdMain
    addiu $a1, $t1, 4
