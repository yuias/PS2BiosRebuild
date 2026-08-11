// The cache trio: syscall slots 0x60, 0x61 and 0x62.
//
// docs/spec/04-ee-kernel.md EE-8e and docs/spec/05-ee-syscall-abi.md SYS-4.
// These three are published through KSEG1 rather than KSEG0, and that is a
// hardware requirement, not a convention: each one reconfigures or invalidates
// the very cache its own instruction fetches would come through. `tables.S`
// aliases them; nothing here can enforce it, which is why the gate does.
//
// All three take one argument and return nothing (SYS-4).

    .set noreorder
    .set noat

.macro sync_p
    .word 0x0000040F
.endm

// Config's cache-enable bits sit at 16 and 17, so an argument naming caches is
// shifted there; its low two bits are the two caches, in that order.
    .set CONFIG_ENABLE_SHIFT, 16
    .set CACHE_ARGUMENT_MASK, 3

// The tag walks: one line at a time, both ways of each line, over the size of
// the cache being swept. The instruction cache's tags cover half what the data
// cache's do.
    .set CACHE_LINE, 0x40
    .set ICACHE_SPAN, 0x1000
    .set DCACHE_SPAN, 0x2000

// The `cache` operations each walk uses. They differ between enabling and
// disabling because the two directions want different things done with a line
// that is already valid.
    .set ICACHE_INVALIDATE, 0x16
    .set ICACHE_WRITEBACK,  0x14
    .set DCACHE_OP,         0x07

    .globl _sys_set_cache_mode
    .globl _sys_enable_cache
    .globl _sys_disable_cache

    .section .text, "ax"

// Slot 0x60: set the cache mode, which is the low three bits of `Config`.
//
// The reference clears those three bits and then **ANDs** the argument in
// rather than ORing it, which makes the result unconditionally zero -- see
// `docs/analysis/18-ee-cache-syscalls.md`, where the raw instruction word is
// quoted, and `spec/05` SYS-4a. It is reproduced rather than corrected: every
// retail machine behaves this way, so software written for the platform met
// this behaviour and not the intended one.
_sys_set_cache_mode:
    mfc0  $t0, $16                      // Config
    andi  $a0, $a0, CACHE_ARGUMENT_MASK
    srl   $t0, $t0, 3
    sll   $t0, $t0, 3                   // the mode bits, cleared
    and   $t0, $t0, $a0                 // SYS-4a: and, not or
    mtc0  $t0, $16
    sync_p
    jr    $ra
    nop

// Slot 0x61: turn on the caches named in the argument.
//
// A cache that is currently *off* is invalidated before it is enabled: its
// tags are whatever they were when it was turned off, and enabling it without
// clearing them would let stale lines answer. One that is already on is left
// alone, so the call is safe to repeat.
_sys_enable_cache:
    andi  $a0, $a0, CACHE_ARGUMENT_MASK

    mfc0  $t0, $16
    srl   $t0, $t0, CONFIG_ENABLE_SHIFT
    andi  $t0, $t0, 1
    bne   $t0, $zero, _enable_data      // already on: nothing to invalidate
    nop
    move  $t0, $zero
_enable_instruction_line:
    sync
    cache ICACHE_INVALIDATE, 0($t0)
    cache ICACHE_INVALIDATE, 1($t0)
    sync
    addiu $t0, $t0, CACHE_LINE
    slti  $t1, $t0, ICACHE_SPAN
    bne   $t1, $zero, _enable_instruction_line
    nop

_enable_data:
    mfc0  $t0, $16
    srl   $t0, $t0, CONFIG_ENABLE_SHIFT
    andi  $t0, $t0, 2
    bne   $t0, $zero, _enable_write
    nop
    move  $t0, $zero
_enable_data_line:
    sync_p
    cache DCACHE_OP, 0($t0)
    cache DCACHE_OP, 1($t0)
    sync_p
    addiu $t0, $t0, CACHE_LINE
    slti  $t1, $t0, DCACHE_SPAN
    bne   $t1, $zero, _enable_data_line
    nop

_enable_write:
    sll   $a0, $a0, CONFIG_ENABLE_SHIFT
    mfc0  $t0, $16
    or    $t0, $t0, $a0                 // set the bits the caller named
    sync
    mtc0  $t0, $16
    sync_p
    jr    $ra
    nop

// Slot 0x62: turn off the caches named in the argument.
//
// The mirror of 0x61, and the polarity of both tests is reversed with it: here
// a cache that is currently *on* is swept before it is disabled, because its
// dirty lines are the only copy of what they hold. One already off is skipped.
_sys_disable_cache:
    andi  $a0, $a0, CACHE_ARGUMENT_MASK

    mfc0  $t0, $16
    srl   $t0, $t0, CONFIG_ENABLE_SHIFT
    andi  $t0, $t0, 1
    beq   $t0, $zero, _disable_data     // already off: nothing to write back
    nop
    move  $t0, $zero
_disable_instruction_line:
    sync
    cache ICACHE_WRITEBACK, 0($t0)
    sync
    cache ICACHE_WRITEBACK, 1($t0)
    sync
    addiu $t0, $t0, CACHE_LINE
    slti  $t1, $t0, ICACHE_SPAN
    bne   $t1, $zero, _disable_instruction_line
    nop

_disable_data:
    mfc0  $t0, $16
    srl   $t0, $t0, CONFIG_ENABLE_SHIFT
    andi  $t0, $t0, 2
    beq   $t0, $zero, _disable_write
    nop
    move  $t0, $zero
_disable_data_line:
    sync_p
    cache DCACHE_OP, 0($t0)
    cache DCACHE_OP, 1($t0)
    sync_p
    addiu $t0, $t0, CACHE_LINE
    slti  $t1, $t0, DCACHE_SPAN
    bne   $t1, $zero, _disable_data_line
    nop

_disable_write:
    sll   $a0, $a0, CONFIG_ENABLE_SHIFT
    not   $a0, $a0
    mfc0  $t0, $16
    and   $t0, $t0, $a0                 // clear the bits the caller named
    sync
    mtc0  $t0, $16
    sync_p
    jr    $ra
    nop
