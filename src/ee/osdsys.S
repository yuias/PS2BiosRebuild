// OSDSYS: the program the boot ends by running.
//
// docs/spec/04-ee-kernel.md EE-9c: the default boot runs `rom0:OSDSYS` with a
// single argument selecting the browser. This is that program's place in the
// image and its entry contract; what an on-screen menu needs -- a display, a
// font, artwork -- is material `docs/clean-room-policy.md` puts out of bounds
// and would be built from freely-licensed sources when there is a screen to
// put it on.
//
// Unlike an IOP module this is a plain executable: it is linked at the address
// it runs from and the kernel's loader places it there (spec/02 notes the
// archive's four ET_EXEC files).

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

    .set SIO_ISR, 0xB000F130
    .set SIO_TX,  0xB000F180
    .set SIO_TX_READY, 0x8000

    .section .text, "ax"
    .globl _osd_start

// entry(argc, argv): the kernel passes what EE-9c's caller gave it.
_osd_start:
    la    $sp, _osd_stack_top
    move  $s0, $a0
    move  $s1, $a1

    la    $a0, _running
    bal   _osd_print
    nop

    // Say which argument arrived, since that is what selects what an OSD
    // would show (EE-9c passes exactly one).
    beq   $s0, $zero, _osd_idle
    nop
    lw    $a0, 0($s1)
    bal   _osd_print
    nop
    la    $a0, _newline
    bal   _osd_print
    nop

_osd_idle:
    b     _osd_idle
    nop

_osd_print:
    move  $t0, $a0
_osd_print_byte:
    lbu   $t1, 0($t0)
    beq   $t1, $zero, _osd_print_done
    nop
_osd_print_wait:
    liw   $t2, SIO_ISR
    lw    $t3, 0($t2)
    andi  $t3, $t3, SIO_TX_READY
    bne   $t3, $zero, _osd_print_wait
    nop
    liw   $t2, SIO_TX
    sb    $t1, 0($t2)
    addiu $t0, $t0, 1
    b     _osd_print_byte
    nop
_osd_print_done:
    jr    $ra
    nop

    .section .rodata, "a"
_running:
    .asciz "# OSDSYS: loaded from the archive and running. Argument: "
_newline:
    .asciz "\n"

    .section .bss, "aw", @nobits
    .align 4
_osd_stack:
    .space 0x1000
_osd_stack_top:
