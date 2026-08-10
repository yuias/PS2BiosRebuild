// The syscall entry and the handlers that do not have to live in the vector
// page. docs/spec/04-ee-kernel.md EE-7, and docs/spec/05-ee-syscall-abi.md.

    .set noreorder
    .set noat

.macro sync_p
    .word 0x0000040F
.endm

.macro liw reg, value
    lui   \reg, ((\value) >> 16) & 0xFFFF
    ori   \reg, \reg, (\value) & 0xFFFF
.endm

.macro la reg, symbol
    lui   \reg, %hi(\symbol)
    addiu \reg, \reg, %lo(\symbol)
.endm

    .set SYSCALL_TABLE, 0x4F40          // 0x80014F40, from a 0x8001 base
    .set SYSCALL_COUNT, 0x7D

    .globl _syscall_entry
    .globl _sys_undefined

    .section .text, "ax"

// The `Sys` handler, reached through the exception table's ExcCode 8 entry.
//
// EE-7a: the number arrives in $v1, not $v0.
// EE-7b: a negative number is negated rather than rejected.
// EE-7c: EPC is advanced past the `syscall` before dispatching, so the eventual
//        `eret` resumes after it. EE-7g's scheduling handlers may overwrite
//        that; nothing here does yet.
// EE-7d: a kernel stack is installed before anything that could nest.
//
// What is *not* done yet is EE-7e's full 128-bit context save. Handlers here
// preserve $ra, $sp and $at and clobber the temporaries; the `sq` context of
// the scheduler group arrives with the scheduler.
_syscall_entry:
    la    $k0, _syscall_saved
    sw    $ra, 0($k0)
    sw    $sp, 4($k0)
    sw    $at, 8($k0)

    bgez  $v1, _syscall_positive        // EE-7b
    nop
    subu  $v1, $zero, $v1
_syscall_positive:

    mfc0  $k0, $14                      // EE-7c: EPC += 4
    addiu $k0, $k0, 4
    mtc0  $k0, $14
    sync_p

    la    $sp, _syscall_stack_top       // EE-7d

    sltiu $k0, $v1, SYSCALL_COUNT
    beq   $k0, $zero, _syscall_out_of_range
    nop

    sll   $k0, $v1, 2
    lui   $k1, 0x8001
    addu  $k1, $k1, $k0
    lw    $k1, SYSCALL_TABLE($k1)
    // SYS-3a: a handler sees the number in $v1 still multiplied by four, which
    // is the byte index the dispatcher used. The undefined reporter divides it
    // back down to print it.
    jalr  $k1
    move  $v1, $k0

_syscall_return:
    la    $k0, _syscall_saved
    lw    $ra, 0($k0)
    lw    $sp, 4($k0)
    lw    $at, 8($k0)
    sync_p
    eret

_syscall_out_of_range:
    // A number outside 0..0x7C never reaches the table. EE-8b makes every
    // number *in* range resolve to code, so this is the only unreachable case.
    sll   $v1, $v1, 2
    jal   _sys_undefined
    nop
    b     _syscall_return
    move  $v0, $zero

// The handler every undefined and retired slot is bound to (EE-8d). It reports
// the number rather than returning quietly, so an out-of-date caller is
// diagnosed instead of jumping into nothing.
_sys_undefined:
    addiu $sp, $sp, -16
    sw    $ra, 0($sp)
    sw    $s0, 4($sp)
    srl   $s0, $v1, 2                   // SYS-3a: undo the dispatcher's scaling
    la    $a0, _undefined_message
    jal   _print
    nop
    move  $a0, $s0
    jal   _print_hex8
    nop
    la    $a0, _newline
    jal   _print
    nop
    lw    $ra, 0($sp)
    lw    $s0, 4($sp)
    addiu $sp, $sp, 16
    jr    $ra
    move  $v0, $zero

    .section .rodata, "a"
_undefined_message:
    .asciz "# syscall not implemented: 0x"
_newline:
    .asciz "\n"

    .section .bss, "aw", @nobits
    .align 4
_syscall_saved:
    .space 16                           // $ra, $sp, $at
    .align 4
_syscall_stack:
    .space 0x1000
_syscall_stack_top:
