// Memory-controller bring-up, reached by the EE reset path (spec/04 EE-2).
//
// On hardware this negotiates with the RDRAM controller, counts devices and
// reports the size it found. An emulator presents its RAM already, so there is
// nothing to negotiate: the file exists because the reset path calls it, and
// the call is part of the specified sequence.
//
// Returns 0, which the reset path treats as success.

    .set noreorder
    .section .text, "ax"
    .globl _rdram
_rdram:
    jr    $ra
    move  $v0, $zero
