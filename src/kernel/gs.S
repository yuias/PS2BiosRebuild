// The two GS slots whose value is a whole 64-bit register: 0x70 and 0x71.
//
// docs/spec/05-ee-syscall-abi.md SYS-7c. They are here rather than in `gs.cpp`
// because of the ABI, not because of the hardware: this target passes a 64-bit
// argument in one register but returns one through a hidden pointer, so a
// compiled handler has no way to put a doubleword in $v0. The pair's whole
// point is that doubleword, so the two of them are written by hand.

    .set noreorder
    .set noat

    .set GS_IMR, 0x12001010

    .globl _sys_get_gs_imr
    .globl _sys_set_gs_imr

    .section .text, "ax"

// Slot 0x71: set the mask, and answer with the value just set -- not the one
// it replaced. The register cannot be read back, so the shadow below is the
// only record of it and 0x70 has no other answer.
_sys_set_gs_imr:
    lui   $v0, (GS_IMR >> 16) & 0xFFFF
    ori   $v0, $v0, GS_IMR & 0xFFFF
    sd    $a0, 0($v0)
    lui   $v0, %hi(_gs_imr_shadow)
    addiu $v0, $v0, %lo(_gs_imr_shadow)
    sd    $a0, 0($v0)
    jr    $ra
    daddu $v0, $zero, $a0               // all 64 bits; `move` would sign-extend 32

// Slot 0x70: the shadow, whole.
_sys_get_gs_imr:
    lui   $v0, %hi(_gs_imr_shadow)
    addiu $v0, $v0, %lo(_gs_imr_shadow)
    jr    $ra
    ld    $v0, 0($v0)

    .section .bss, "aw", @nobits
    .align 3                            // `sd` and `ld` want a doubleword
_gs_imr_shadow:
    .space 8
