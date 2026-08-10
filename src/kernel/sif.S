// The EE's half of the meeting with the IOP.
//
// docs/spec/03-boot-chain.md BOOT-10. The EE opens the bus, publishes an
// address in its own RAM and raises a bit; the IOP answers with an address in
// its RAM and a bit of its own. Neither side may proceed alone.
//
// BOOT-10c: from this side a write to MSFLG *sets* bits and a write to SMFLG
// *clears* them; the IOP's writes do the reverse. Getting that backwards does
// not fail, it livelocks -- each side's acknowledgement erasing the other's
// request -- which is why it is written out rather than assumed.

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

    .set SIF_MSCOM, 0xB000F200
    .set SIF_SMCOM, 0xB000F210
    .set SIF_MSFLG, 0xB000F220
    .set SIF_SMFLG, 0xB000F230
    .set SIF_CTRL,  0xB000F240

    .set HANDSHAKE_BIT, 0x00010000
    .set CTRL_OPEN,     0x00000100

    .globl _sif_handshake
    .globl _sif_peer

    .section .text, "ax"

// _sif_handshake(): returns once the IOP has answered, with its published
// address in $v0.
_sif_handshake:
    liw   $t0, SIF_CTRL
    ori   $t1, $zero, CTRL_OPEN
    sw    $t1, 0($t0)

    la    $t1, _sif_area                 // BOOT-10a: an address in our RAM
    liw   $t0, SIF_MSCOM
    sw    $t1, 0($t0)

    liw   $t0, SIF_MSFLG
    liw   $t2, HANDSHAKE_BIT
    sw    $t2, 0($t0)                    // our write sets

    liw   $t0, SIF_SMFLG
_sif_wait:
    lw    $t1, 0($t0)
    and   $t1, $t1, $t2
    beq   $t1, $zero, _sif_wait          // BOOT-10b: wait for the answer
    nop

    liw   $t3, SIF_SMCOM
    lw    $v0, 0($t3)
    la    $t4, _sif_peer
    sw    $v0, 0($t4)
    sw    $t2, 0($t0)                    // our write to SMFLG clears: an ack
    jr    $ra
    nop

    .section .bss, "aw", @nobits
    .align 4
_sif_area:
    .space 256                           // what we publish to the IOP
_sif_peer:
    .space 4                             // and what it publishes to us
