// EESYNC: the IOP's half of the meeting with the EE.
//
// docs/spec/03-boot-chain.md BOOT-10. The IOP boot does not end at the last
// module of the list; it ends waiting for the EE, which is why this module is
// last in `IOPBTCONF`. The EE publishes an address and raises a bit in MSFLG;
// this answers with an address of its own and a bit in SMFLG, then clears what
// the EE raised.
//
// BOOT-10c is the part worth being careful about: a write from this side
// *clears* bits in MSFLG and *sets* them in SMFLG, and the EE's writes do the
// reverse. Registers that merely stored would livelock rather than fail.

    .set noreorder
    .set noat

.macro la reg, symbol
    lui   \reg, %hi(\symbol)
    addiu \reg, \reg, %lo(\symbol)
.endm

    .set SIF_MSCOM, 0xBD000000
    .set SIF_SMCOM, 0xBD000010
    .set SIF_MSFLG, 0xBD000020
    .set SIF_SMFLG, 0xBD000030
    .set SIF_CTRL,  0xBD000040

    .set HANDSHAKE_BIT, 0x00010000
    .set CTRL_ANSWER,   0x00000040

    .section .text, "ax"
    .globl _module_start

_module_start:
    // BOOT-10a: wait for the EE to raise its bit. Nothing else can proceed
    // until it does -- neither side may go on alone.
    lui   $t0, (SIF_MSFLG >> 16) & 0xFFFF
    ori   $t0, $t0, SIF_MSFLG & 0xFFFF
    lui   $t2, (HANDSHAKE_BIT >> 16) & 0xFFFF
_wait_for_ee:
    lw    $t1, 0($t0)
    and   $t1, $t1, $t2
    beq   $t1, $zero, _wait_for_ee
    nop

    // BOOT-10b: answer with an address in our own RAM and a bit of our own.
    lui   $t3, (SIF_CTRL >> 16) & 0xFFFF
    ori   $t3, $t3, SIF_CTRL & 0xFFFF
    ori   $t4, $zero, CTRL_ANSWER
    sw    $t4, 0($t3)

    la    $t4, _sif_area
    lui   $t3, (SIF_SMCOM >> 16) & 0xFFFF
    ori   $t3, $t3, SIF_SMCOM & 0xFFFF
    sw    $t4, 0($t3)

    lui   $t3, (SIF_SMFLG >> 16) & 0xFFFF
    ori   $t3, $t3, SIF_SMFLG & 0xFFFF
    sw    $t2, 0($t3)                    // our write sets

    // Record what the EE published, then clear its flag: our write to MSFLG
    // is an acknowledgement, not a request.
    lui   $t3, (SIF_MSCOM >> 16) & 0xFFFF
    ori   $t3, $t3, SIF_MSCOM & 0xFFFF
    lw    $t5, 0($t3)
    la    $t6, _ee_area
    sw    $t5, 0($t6)
    sw    $t2, 0($t0)                    // our write to MSFLG clears

    jr    $ra
    move  $v0, $zero                     // resident

    .section .bss, "aw", @nobits
    .align 4
_sif_area:
    .space 64                            // what we published to the EE
_ee_area:
    .space 4                             // and what it published to us
