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
    .set REQUEST_BIT,   0x00000002       // we have sent the IOP something
    .set REPLY_BIT,     0x00000002       // and it has answered
    .set CTRL_OPEN,     0x00000100

// The EE's two SIF DMA channels: CHCR +0, MADR +0x10, QWC +0x20.
    .set DMA_SIF0, 0xB000C000            // from the IOP
    .set DMA_SIF1, 0xB000C400            // to the IOP
    .set DMA_START, 0x00000100           // CHCR.STR, cleared by the hardware

    .set REQUEST_QUADS, 1                // a 16-byte request: one name
    .set PAYLOAD_QUADS, 16               // and up to 256 bytes back

    .globl _sif_handshake
    .globl _sif_request
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

// _sif_request(name) -> the bytes that came back
//
// The EE cannot read the IOP's memory and the IOP cannot read the EE's, so a
// request is a transfer out and its answer a transfer back. Both channels run
// in normal mode: the quadword count is what moves, with no tag chain.
_sif_request:
    // Put the ten-byte name where the outgoing channel will read it.
    la    $t0, _sif_area
    move  $t1, $zero
_sif_request_name:
    addu  $t2, $a0, $t1
    lbu   $t3, 0($t2)
    addu  $t2, $t0, $t1
    sb    $t3, 0($t2)
    addiu $t1, $t1, 1
    slti  $t2, $t1, 16
    bne   $t2, $zero, _sif_request_name
    nop

    liw   $t4, DMA_SIF1
    sw    $t0, 0x10($t4)                 // MADR
    ori   $t5, $zero, REQUEST_QUADS
    sw    $t5, 0x20($t4)                 // QWC
    liw   $t5, DMA_START
    sw    $t5, 0($t4)                    // CHCR: start

    liw   $t0, SIF_MSFLG
    ori   $t2, $zero, REQUEST_BIT
    sw    $t2, 0($t0)                    // our write sets: it is there for you

    liw   $t0, SIF_SMFLG
    ori   $t2, $zero, REPLY_BIT
_sif_request_wait:
    lw    $t1, 0($t0)
    and   $t1, $t1, $t2
    beq   $t1, $zero, _sif_request_wait
    nop
    sw    $t2, 0($t0)                    // our write to SMFLG clears

    la    $t6, _sif_reply
    liw   $t4, DMA_SIF0
    sw    $t6, 0x10($t4)                 // MADR
    ori   $t5, $zero, PAYLOAD_QUADS
    sw    $t5, 0x20($t4)                 // QWC
    liw   $t5, DMA_START
    sw    $t5, 0($t4)                    // CHCR: start
    jr    $ra
    move  $v0, $t6

    .section .bss, "aw", @nobits
    .align 4
_sif_reply:
    .space 256                           // what the IOP sends back
_sif_area:
    .space 256                           // what we publish to the IOP
_sif_peer:
    .space 4                             // and what it publishes to us
