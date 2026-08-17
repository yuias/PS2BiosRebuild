// The EE exception vector page, and the handlers that must live inside it.
//
// docs/spec/04-ee-kernel.md EE-4: this section is the first 0x1000 bytes of
// KERNEL, which the reset path copies to physical zero, so the offsets below
// are the architectural vector addresses themselves.
//
// EE-5b and EE-8f: some syscall handlers are here rather than in .text
// because they must stay reachable whatever else is mapped. Their placement is
// part of the interface, not a layout convenience.

    .set noreorder
    .set noat

.macro sync_p
    .word 0x0000040F
.endm

.macro liw reg, value
    lui   \reg, ((\value) >> 16) & 0xFFFF
    ori   \reg, \reg, (\value) & 0xFFFF
.endm

    .set EXCEPTION_TABLE, 0x5340        // 0x80015340, from a 0x8001 base
    .set SAVED_T9,        0x5378        // scratch for the dispatcher
    .set INTERRUPT_TABLE, 0x5380        // 0x80015380

    .set INTC_MASK,  0xB000F010
    .set DMAC_STAT,  0xB000E010

// EE-6: the common entry does not branch on the cause. It masks `Cause` with
// 0x7C -- which leaves the code already scaled by four -- and uses the result
// directly as a byte index into the table of EE-6a.
//
// The reference restores $t9 through a base register the table lookup has
// already overwritten; ours keeps the base in $k1 so the restore reads the
// word it saved. EE-6c's "no shift between the mask and the index" is what
// matters and is reproduced exactly.
.macro dispatch_exception
    lui   $k1, 0x8001
    sd    $t9, SAVED_T9($k1)
    mfc0  $t9, $13                      // Cause
    andi  $t9, $t9, 0x7C                // ExcCode, already scaled by 4
    addu  $k0, $k1, $t9
    lw    $k0, EXCEPTION_TABLE($k0)
    jr    $k0
    ld    $t9, SAVED_T9($k1)
.endm

    .section .vectors, "ax"
    .globl _vectors
_vectors:
// 0x000: TLB refill.
    dispatch_exception

// 0x080: counter.
    .org 0x080
_vector_counter:
    j     _vector_common_stop
    nop

// 0x100: debug. EE-5: the same handler as 0x080.
    .org 0x100
_vector_debug:
    j     _vector_common_stop
    nop

// 0x180: the common exception vector. EE-5a: byte-identical to 0x000. A
// rebuild may share one implementation but must populate both offsets.
    .org 0x180
    dispatch_exception

// 0x200: interrupt. EE-6d: indexed by interrupt number into its own table.
// The number is the highest pending and enabled IP bit -- Cause and Status
// agree on where they keep them -- which is what the reference's leading-zero
// count amounts to (docs/analysis/35). IP2 is the INTC, IP3 the DMAC and IP7
// the timers; the table's entries for them are the interrupt entry of
// syscall.S, which reads the controllers itself.
    .org 0x200
_vector_interrupt:
    lui   $k1, 0x8001
    sd    $t9, SAVED_T9($k1)
    mfc0  $t9, $13                      // Cause
    mfc0  $k0, $12                      // Status
    and   $t9, $t9, $k0
    andi  $t9, $t9, 0xFF00              // IP7..IP0
    beq   $t9, $zero, _vector_interrupt_none
    addiu $k0, $zero, 7 * 4             // scan from IP7 down
_vector_interrupt_scan:
    andi  $k1, $t9, 0x8000
    bne   $k1, $zero, _vector_interrupt_found
    sll   $t9, $t9, 1                   // next lower bit into 0x8000
    bne   $k0, $zero, _vector_interrupt_scan
    addiu $k0, $k0, -4
_vector_interrupt_found:
    lui   $k1, 0x8001
    addu  $k0, $k1, $k0
    lw    $k0, INTERRUPT_TABLE($k0)
    beq   $k0, $zero, _vector_interrupt_none
    nop
    jr    $k0
    ld    $t9, SAVED_T9($k1)
_vector_interrupt_none:
    lui   $k1, 0x8001
    ld    $t9, SAVED_T9($k1)
    sync_p
    eret

// Every cause but `Sys` reaches here (EE-6b: one common routine, not a handler
// per cause). There is nothing yet that can recover, so it stops where it can
// be seen.
    .org 0x280
    .globl _vector_common_stop
_vector_common_stop:
    b     _vector_common_stop
    nop

// ---------------------------------------------------------------------------
// EE-8f: the handlers for the exception- and interrupt-table installers and
// for the INTC/DMAC enable pairs live in this page.

// Slot 0x0D: install an exception handler for ExcCode 1..3.
    .org 0x300
    .globl _sys_set_exception_handler
_sys_set_exception_handler:
    addiu $t0, $a0, -1
    sltiu $t0, $t0, 3                   // 1 <= code <= 3
    beq   $t0, $zero, _sys_install_reject
    nop
    sll   $t0, $a0, 2
    lui   $at, 0x8001
    addu  $at, $at, $t0
    sw    $a1, EXCEPTION_TABLE($at)
    jr    $ra
    move  $v0, $a1                      // the handler installed, per analysis 16

// Slot 0x0E: the same, for ExcCode 4..13.
    .org 0x340
    .globl _sys_set_common_handler
_sys_set_common_handler:
    addiu $t0, $a0, -4
    sltiu $t0, $t0, 10                  // 4 <= code <= 13
    beq   $t0, $zero, _sys_install_reject
    nop
    sll   $t0, $a0, 2
    lui   $at, 0x8001
    addu  $at, $at, $t0
    sw    $a1, EXCEPTION_TABLE($at)
    jr    $ra
    move  $v0, $a1

// Slot 0x0F: install an interrupt handler, indexes 0..7.
    .org 0x380
    .globl _sys_set_interrupt_handler
_sys_set_interrupt_handler:
    sltiu $t0, $a0, 8
    beq   $t0, $zero, _sys_install_reject
    nop
    sll   $t0, $a0, 2
    lui   $at, 0x8001
    addu  $at, $at, $t0
    sw    $a1, INTERRUPT_TABLE($at)
    jr    $ra
    move  $v0, $a1

_sys_install_reject:
    // An index outside the accepted range returns 0, and none of the three
    // returns the previous handler: a caller that wants to chain must have
    // kept it (docs/analysis/16-ee-interrupt-syscalls.md).
    jr    $ra
    move  $v0, $zero

// EE-8g: the enable/disable quartet. Each toggles one bit of a hardware mask,
// reads it first, and writes only when the write would change it -- the
// registers are write-to-toggle, so an unconditional write is wrong on the
// second call. The return value is whether it acted.
//
//   $a0  the source number
//   $t0  the register, $t1 the bit
.macro toggle_bit register, shift, want_set
    andi  $a0, $a0, 0x1F
    addiu $t2, $zero, 1
    sllv  $t1, $t2, $a0
    .if \shift
    sll   $t1, $t1, 16                  // the DMAC bit starts at 16, not 0
    .endif
    liw   $t0, \register
    lw    $t3, 0($t0)
    and   $t3, $t3, $t1
    .if \want_set
    bne   $t3, $zero, _sys_toggle_noop  // already set: nothing to do
    .else
    beq   $t3, $zero, _sys_toggle_noop  // already clear
    .endif
    nop
    sw    $t1, 0($t0)                   // write-to-toggle
    jr    $ra
    addiu $v0, $zero, 1                 // acted
.endm

    .org 0x400
    .globl _sys_enable_intc
_sys_enable_intc:
    toggle_bit INTC_MASK, 0, 1

    .org 0x440
    .globl _sys_disable_intc
_sys_disable_intc:
    toggle_bit INTC_MASK, 0, 0

    .org 0x480
    .globl _sys_enable_dmac
_sys_enable_dmac:
    toggle_bit DMAC_STAT, 1, 1

    .org 0x4C0
    .globl _sys_disable_dmac
_sys_disable_dmac:
    toggle_bit DMAC_STAT, 1, 0

_sys_toggle_noop:
    jr    $ra
    move  $v0, $zero                    // the bit was already as asked

// Slot 0x75: a call that does nothing, quietly. SYS-3b: it is not one of the
// undefined slots, and the difference must survive a rebuild.
    .org 0x500
    .globl _sys_nothing
_sys_nothing:
    jr    $ra
    nop

// Slot 0x74: install a syscall handler. SYS-5a and SYS-5b -- the syscall table
// is writable at run time, like the two dispatch tables above it.
    .org 0x520
    .globl _sys_set_syscall
_sys_set_syscall:
    sltiu $t0, $a0, 0x100               // SYS-11: the table reaches 0xFF
    beq   $t0, $zero, _sys_set_syscall_done
    nop
    sll   $t0, $a0, 2
    lui   $at, 0x8001
    addu  $at, $at, $t0
    sw    $a1, 0x4F40($at)              // the syscall table of EE-8a
_sys_set_syscall_done:
    jr    $ra
    nop                                 // SYS-1: no value comes back
