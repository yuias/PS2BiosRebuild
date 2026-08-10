// The EE kernel's entry point and vector page.
//
// docs/spec/04-ee-kernel.md EE-4: this file is copied to physical zero and
// entered at 0x80001000, so its first 0x1000 bytes *are* the exception vector
// page and the entry point is the first thing after it. The layout is not a
// choice; it is what the reset path's copy makes true.
//
// What is here so far is the placement and the announcement. The vectors, the
// dispatch tables and the 125 syscall slots of EE-5 to EE-8 come next.

    .set noreorder
    .set noat

.macro sync_p
    .word 0x0000040F
.endm

// Load a 32-bit constant. `%hi`/`%lo` are not used here: `%hi` compensates for
// the sign extension of a following `addiu`, which is wrong when the low half
// is applied with `ori`.
.macro liw reg, value
    lui   \reg, ((\value) >> 16) & 0xFFFF
    ori   \reg, \reg, (\value) & 0xFFFF
.endm

// Load a symbol's address, where the `%hi`/`%lo` pairing with `addiu` is right.
.macro la reg, symbol
    lui   \reg, %hi(\symbol)
    addiu \reg, \reg, %lo(\symbol)
.endm

// The EE's serial transmit register and its readiness flag. The ROM prints a
// byte at a time through these, which is how the reference kernel's own boot
// log is observable at all (docs/analysis/22-ee-execution.md).
    .set SIO_TX_READY, 0x8000

// ---------------------------------------------------------------- vectors
// EE-5 populates 0x000, 0x080, 0x100, 0x180 and 0x200. Until the dispatcher
// exists, every vector stops where it can be seen rather than running on.
    .section .vectors, "ax"
    .globl _vectors
_vectors:
_vector_tlb_refill:                 // 0x000
    b     _vector_tlb_refill
    nop

    .org 0x080
_vector_counter:
    b     _vector_counter
    nop

    .org 0x100
_vector_debug:
    b     _vector_debug
    nop

    .org 0x180
_vector_common:                     // EE-5a: the same code as 0x000
    b     _vector_common
    nop

    .org 0x200
_vector_interrupt:
    b     _vector_interrupt
    nop

// ---------------------------------------------------------------- 0x1000
    .section .text, "ax"
    .globl _kernel_entry
_kernel_entry:
    // EE-4a: the boot's result is in the scratchpad word the reset path left
    // it in, not in a register. Nothing consumes it yet; read it where the
    // specification says it is so the dependency is recorded in code.
    liw   $k0, 0x70003FF0
    lw    $s0, 0($k0)

    liw   $sp, 0x70003FE0

    la    $a0, _banner
    jal   _print
    nop

_kernel_idle:
    b     _kernel_idle
    nop

// _print(text): write a NUL-terminated string to the serial port, waiting for
// the transmitter between bytes.
_print:
    move  $t0, $a0
_print_byte:
    lbu   $t1, 0($t0)
    beq   $t1, $zero, _print_done
    nop
_print_wait:
    liw   $t2, 0xB000F130
    lw    $t3, 0($t2)
    andi  $t3, $t3, SIO_TX_READY
    bne   $t3, $zero, _print_wait
    nop
    liw   $t2, 0xB000F180
    sb    $t1, 0($t2)
    addiu $t0, $t0, 1
    b     _print_byte
    nop
_print_done:
    jr    $ra
    nop

    .align 4
_banner:
    .asciz "# PS2BiosRebuild EE kernel: entered at 0x80001000.\n"
