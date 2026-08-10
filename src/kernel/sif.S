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

    .set REQUEST_QUADS, 1                // a 16-byte request: a name and a verb

    .globl _sif_handshake
    .globl _sif_exchange
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

// _sif_exchange(name, verb, destination, quadwords)
//
// One request out and one answer back. The EE cannot read the IOP's memory and
// the IOP cannot read the EE's, so everything crosses as a transfer. Both
// channels run in normal mode: the quadword count is what moves, with no tag
// chain.
//
// The count has to be right. A channel armed for less than was sent leaves the
// rest in the FIFO for whoever asks next, which is why the caller asks the size
// before it asks for the bytes.
_sif_exchange:
    // Put the ten-byte name and the verb where the outgoing channel reads.
    la    $t0, _sif_area
    move  $t1, $zero
    // An entry name is ten bytes, NUL-padded (spec/01 ARC-2). A caller's
    // string ends at its NUL and whatever follows it in memory is not part of
    // the name, so the copy stops there and the rest is filled in.
_sif_exchange_name:
    addu  $t2, $a0, $t1
    lbu   $t3, 0($t2)
    beq   $t3, $zero, _sif_exchange_pad
    nop
    addu  $t2, $t0, $t1
    sb    $t3, 0($t2)
    addiu $t1, $t1, 1
    slti  $t2, $t1, 10
    bne   $t2, $zero, _sif_exchange_name
    nop
    b     _sif_exchange_named
    nop
_sif_exchange_pad:
    slti  $t2, $t1, 10
    beq   $t2, $zero, _sif_exchange_named
    nop
    addu  $t2, $t0, $t1
    sb    $zero, 0($t2)
    b     _sif_exchange_pad
    addiu $t1, $t1, 1
_sif_exchange_named:
    sh    $zero, 10($t0)
    sw    $a1, 12($t0)

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
_sif_exchange_wait:
    lw    $t1, 0($t0)
    and   $t1, $t1, $t2
    beq   $t1, $zero, _sif_exchange_wait
    nop
    sw    $t2, 0($t0)                    // our write to SMFLG clears

    liw   $t4, DMA_SIF0
    sw    $a2, 0x10($t4)                 // MADR: where the answer goes
    sw    $a3, 0x20($t4)                 // QWC: how much of it there is
    liw   $t5, DMA_START
    sw    $t5, 0($t4)                    // CHCR: start
    jr    $ra
    move  $v0, $a2

    .section .bss, "aw", @nobits
    .align 4
_sif_area:
    .space 256                           // what we publish to the IOP
_sif_peer:
    .space 4                             // and what it publishes to us
