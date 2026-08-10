// The EE kernel's entry point.
//
// docs/spec/04-ee-kernel.md EE-4: KERNEL is copied to physical zero and
// entered at 0x80001000, so the vector page of `vectors.S` sits below this and
// the entry point is the first thing after it. The layout is not a choice; it
// is what the reset path's copy makes true.

    .set noreorder
    .set noat

.macro liw reg, value
    lui   \reg, ((\value) >> 16) & 0xFFFF
    ori   \reg, \reg, (\value) & 0xFFFF
.endm

.macro la reg, symbol
    lui   \reg, %hi(\symbol)
    addiu \reg, \reg, %lo(\symbol)
.endm

// The EE's serial transmit register and its readiness flag. The ROM prints a
// byte at a time through these, which is how a boot log is observable at all
// (docs/analysis/22-ee-execution.md).
    .set SIO_ISR, 0xB000F130
    .set SIO_TX,  0xB000F180
    .set SIO_TX_READY, 0x8000

    .globl _kernel_entry
    .globl _print
    .globl _print_hex8

    .section .text, "ax"
_kernel_entry:
    // EE-4a: the boot's result is in the scratchpad word the reset path left
    // it in, not in a register. Nothing consumes it yet; reading it where the
    // specification says it is keeps the dependency recorded in code.
    liw   $k0, 0x70003FF0
    lw    $s0, 0($k0)

    la    $sp, _kernel_stack_top

    la    $a0, _banner
    jal   _print
    nop

    // The vector page and the dispatch tables came with us -- they are part of
    // this file's image -- so there is nothing to install. What does have to
    // change is `Status`: EE-1 leaves BEV *set*, which sends exceptions to the
    // ROM's vectors at 0xBFC00200. Clearing it is what points them at the page
    // this kernel just brought with it.
    mfc0  $k0, $12
    liw   $k1, 0xFFBFFFFF               // ~BEV
    and   $k0, $k0, $k1
    mtc0  $k0, $12
    .word 0x0000040F                    // sync.p

    // BOOT-10: meet the IOP. Until this returns the machine has one working
    // processor; after it, both know where the other's memory is.
    jal   _sif_handshake
    nop
    la    $a0, _met_iop
    jal   _print
    nop

    // Ask the IOP for a file out of the archive. Only the IOP can read the
    // ROM's file table on its side of the bus, so this is the shape every
    // later `rom0:` read has: a name out, bytes back.
    la    $a0, _wanted
    jal   _sif_request
    nop
    move  $s1, $v0
    la    $a0, _fetched
    jal   _print
    nop
    jal   _print
    move  $a0, $s1

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
    liw   $t2, SIO_ISR
    lw    $t3, 0($t2)
    andi  $t3, $t3, SIO_TX_READY
    bne   $t3, $zero, _print_wait
    nop
    liw   $t2, SIO_TX
    sb    $t1, 0($t2)
    addiu $t0, $t0, 1
    b     _print_byte
    nop
_print_done:
    jr    $ra
    nop

// _print_hex8(value): two hexadecimal digits, most significant first.
_print_hex8:
    addiu $sp, $sp, -16
    sw    $ra, 0($sp)
    sw    $s1, 4($sp)
    move  $s1, $a0
    srl   $a0, $s1, 4
    jal   _print_nibble
    nop
    andi  $a0, $s1, 0xF
    jal   _print_nibble
    nop
    lw    $ra, 0($sp)
    lw    $s1, 4($sp)
    jr    $ra
    addiu $sp, $sp, 16

_print_nibble:
    andi  $a0, $a0, 0xF
    sltiu $t0, $a0, 10
    beq   $t0, $zero, _print_nibble_letter
    nop
    addiu $t1, $a0, 0x30                // '0'
    b     _print_nibble_emit
    nop
_print_nibble_letter:
    addiu $t1, $a0, 0x57                // 'a' - 10
_print_nibble_emit:
    liw   $t2, SIO_ISR
    lw    $t3, 0($t2)
    andi  $t3, $t3, SIO_TX_READY
    bne   $t3, $zero, _print_nibble_emit
    nop
    liw   $t2, SIO_TX
    sb    $t1, 0($t2)
    jr    $ra
    nop

    .section .rodata, "a"
_banner:
    .asciz "# PS2BiosRebuild EE kernel: entered at 0x80001000.\n"
_met_iop:
    .asciz "# The IOP answered; the SIF handshake is complete.\n"
_fetched:
    .asciz "# ROMVER, fetched from the archive across the SIF: "
    .align 2
_wanted:
    .ascii "ROMVER"
    .byte 0, 0, 0, 0, 0, 0, 0, 0, 0, 0

    .section .bss, "aw", @nobits
    .align 4
_kernel_stack:
    .space 0x1000
_kernel_stack_top:
