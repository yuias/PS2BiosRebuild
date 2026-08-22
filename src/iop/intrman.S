// INTRMAN's two exception handlers: the interrupt dispatcher's entry and
// exit, and the reschedule syscall. spec/06 IOP-2e/IOP-2j and IOP-3h.
//
// Each is an EXCEPMAN handler (IOP-1a): a structure whose first word is the
// `next` EXCEPMAN rewrites, whose second is the owner's, and whose code
// begins at the third. The vector at 0x80 has saved $at, EPC, Status and
// Cause to 0x400..0x40C and jumps here with $k0 free (IOP-1c).
//
// Both build the frame of src/iop/context.hpp on the interrupted stack, run
// the C++ half on the interrupt stack, and restore from whatever frame it
// returns -- the same one, or another thread's (IOP-3h). The return is
// `jr` with `rfe` in its delay slot: `rfe` pops Status's interrupt-enable
// stack, which the exception pushed, so the frame's Status is written back
// as the exception left it, not as the interrupted code had it.

    .set noreorder
    .set noat

    .set FRAME_BYTES, 0x90
    .set SLOT_EPC,    0
    .set SLOT_STATUS, 4
    .set SLOT_HI,     8
    .set SLOT_LO,     12
    .set SLOT_REG,    16                  // + reg * 4
    .set SAVED_AT,    0x400
    .set SAVED_EPC,   0x404
    .set SAVED_STATUS, 0x408
    .set SAVED_CAUSE, 0x40C

// Push the frame: every register but $zero, $k0 and $k1, then hi/lo, then
// the Status and EPC the vector saved. $sp's own slot holds the value it had
// before the push, so a restore finds the interrupted stack again.
.macro push_frame
    addiu $sp, $sp, -FRAME_BYTES
    lw    $k0, SAVED_AT($zero)
    sw    $k0, SLOT_REG + 1 * 4($sp)
    .irp reg, 2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,28,30,31
    sw    $\reg, SLOT_REG + \reg * 4($sp)
    .endr
    addiu $k0, $sp, FRAME_BYTES
    sw    $k0, SLOT_REG + 29 * 4($sp)
    mfhi  $k0
    sw    $k0, SLOT_HI($sp)
    mflo  $k0
    sw    $k0, SLOT_LO($sp)
    lw    $k0, SAVED_STATUS($zero)
    sw    $k0, SLOT_STATUS($sp)
    lw    $k0, SAVED_EPC($zero)
    sw    $k0, SLOT_EPC($sp)
.endm

// Restore from the frame at $k0 and return to it. Status goes back first,
// while interrupts are still held off by IEc being clear in what the
// exception left; `rfe` then pops the pre-exception enable into place as the
// last thing that happens.
.macro pop_frame_and_return
    lw    $k1, SLOT_STATUS($k0)
    nop
    mtc0  $k1, $12
    lw    $k1, SLOT_HI($k0)
    nop
    mthi  $k1
    lw    $k1, SLOT_LO($k0)
    nop
    mtlo  $k1
    .irp reg, 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,28,29,30,31
    lw    $\reg, SLOT_REG + \reg * 4($k0)
    .endr
    lw    $k1, SLOT_EPC($k0)
    nop
    jr    $k1
    .word 0x42000010                      // rfe
.endm

    .section .data, "aw"
    .align 4

// IOP-2d: registered on cause 0, the interrupt, at priority 2.
    .globl _intrman_interrupt_handler
_intrman_interrupt_handler:
    .word 0                               // next: EXCEPMAN's
    .word 0                               // info
    push_frame
    move  $a0, $sp                        // the frame
    lw    $a1, SAVED_CAUSE($zero)
    // The C++ half decides where to stand: on the interrupt stack unless this
    // interrupt has nested inside a handler already standing there.
    la    $k0, _intrman_stack
    sltu  $k1, $sp, $k0
    bne   $k1, $zero, 1f                  // below the stack: not nested
    la    $k1, _intrman_stack_top
    sltu  $k1, $sp, $k1
    bne   $k1, $zero, 2f                  // inside it: nested, stay here
    nop
1:
    la    $sp, _intrman_stack_top
2:
    addiu $sp, $sp, -16
    jal   _intrman_dispatch               // (frame, cause) -> frame to resume
    nop
    move  $k0, $v0
    pop_frame_and_return

// IOP-2d: registered on cause 8, the syscall. EPC is the `syscall` itself;
// the return goes past it.
    .globl _intrman_syscall_handler
_intrman_syscall_handler:
    .word 0
    .word 0
    push_frame
    lw    $k0, SLOT_EPC($sp)
    nop
    lw    $a1, 0($k0)                     // the instruction, for its code field
    addiu $k0, $k0, 4
    sw    $k0, SLOT_EPC($sp)
    move  $a0, $sp
    la    $sp, _intrman_stack_top
    addiu $sp, $sp, -16
    jal   _intrman_syscall                // (frame, instruction) -> frame
    nop
    move  $k0, $v0
    pop_frame_and_return

    .section .bss, "aw", @nobits
    .align 4
    .globl _intrman_stack
_intrman_stack:
    .space 0x800                          // IOP-2e: the reference's size
    .globl _intrman_stack_top
_intrman_stack_top:
