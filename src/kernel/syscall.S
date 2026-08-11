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

// EE-7e wants 128-bit stores, and LLVM has no R5900 target to assemble them
// with -- the same reason `sync.p` above is a `.word`. LQ and SQ take the two
// opcodes MIPS III leaves unused, and both ignore the low four address bits,
// so a context block only has to be quadword-aligned once.
.macro sq_ rt, offset, base
    .word (0x1F << 26) | ((\base) << 21) | ((\rt) << 16) | ((\offset) & 0xFFFF)
.endm

.macro lq_ rt, offset, base
    .word (0x1E << 26) | ((\base) << 21) | ((\rt) << 16) | ((\offset) & 0xFFFF)
.endm

    .set SYSCALL_TABLE, 0x4F40          // 0x80014F40, from a 0x8001 base
    .set SYSCALL_COUNT, 0x7D

    .set CONTEXT_BASE, 26               // $k0 addresses the context block

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
// EE-7e: context is saved 128 bits at a time, because the caller's registers
//        are that wide and a handler that returns a 64-bit value has already
//        destroyed the upper half of the one it wrote.
//
// SYS-2a: the prologue must leave $a0-$a3 alone. It touches only $k0 and $k1,
// which the vector page has already taken, so the handler is reached with the
// caller's arguments exactly as they were passed.
_syscall_entry:
    la    $k0, _syscall_context
    .irp reg, 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,28,29,30,31
    sq_   \reg, \reg * 16, CONTEXT_BASE
    .endr

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

// EE-7g: a scheduling handler resumes somewhere else by writing this block --
// its own `$sp` slot decides the stack `eret` comes back on, and `mtc0` to EPC
// decides the address. Restoring from memory rather than from registers we
// happened to keep is what makes that possible at all.
//
// $v0 carries the result and $v1 is left as the dispatcher's scaled index:
// SYS-3a's byte offset is what the reference's caller gets back too, measured
// rather than assumed, so putting the number back would be our invention.
_syscall_return:
    la    $k0, _syscall_context
    .irp reg, 1,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,28,29,30,31
    lq_   \reg, \reg * 16, CONTEXT_BASE
    .endr
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
_syscall_context:
    .space 32 * 16                      // one 128-bit slot per register
    .align 4
_syscall_stack:
    .space 0x1000
_syscall_stack_top:
