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

// The EE's two SIF DMA channels: CHCR +0, MADR +0x10, QWC +0x20, TADR +0x30.
    .set DMA_SIF0, 0xB000C000            // from the IOP
    .set DMA_SIF1, 0xB000C400            // to the IOP
// BOOT-11b: both channels run in chain mode -- MOD=chain, TIE and STR, with
// TTE clear so the tags stay out of the data.
    .set DMA_CHAIN, 0x00000184
    .set DMA_START, 0x00000100           // CHCR.STR, which the hardware clears

// BOOT-11g: a channel's own STR is not enough. The controller has a master
// enable of its own and a hold register that starts out holding, and until
// both are dealt with a started channel simply does not run. A simulator that
// looks only at STR will not notice; an emulator will move nothing.
    .set DMA_CTRL,    0xB000E000         // bit 0: the controller is enabled
    .set DMA_STAT,    0xB000E010
    .set DMA_ENABLEW, 0xB000F590         // bit 16 holds every channel off
    .set DMA_ENABLED, 0x00002000         // what the reference leaves there
    .set DMA_STAT_CLEAR, 0x0000E31F

    .set TAG_REFE,      0x00000000       // a source tag pointing elsewhere, last
    .set TAG_END,       0x80000000       // the IOP-facing header's end bit

    .set REQUEST_WORDS, 8                // name, verb, and where to reply
    .set PACKET_QUADS,  3                // the header quadword and those two

    .globl _sif_handshake
    .globl _sif_exchange
    // The compiled half of the kernel calls this by the project's C++ naming.
    // The alias goes away when this file follows it.
    .globl sifExchange
    .set sifExchange, _sif_exchange
    .globl _sif_peer

    .section .text, "ax"

// _sif_handshake(): returns once the IOP has answered, with its published
// address in $v0.
_sif_handshake:
    // BOOT-11g: bring the DMA controller up before anything asks it to move.
    liw   $t0, DMA_STAT
    liw   $t1, DMA_STAT_CLEAR
    sw    $t1, 0($t0)                    // acknowledge whatever reset left
    liw   $t0, DMA_ENABLEW
    liw   $t1, DMA_ENABLED
    sw    $t1, 0($t0)                    // release the hold on every channel
    liw   $t0, DMA_CTRL
    ori   $t1, $zero, 1
    sw    $t1, 0($t0)

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

// _sif_exchange(name, verb, destination)
//
// One request out and one answer back. The EE cannot read the IOP's memory and
// the IOP cannot read the EE's, so everything crosses as a transfer.
//
// BOOT-11: neither channel is told where its bytes belong by its own driver.
// What goes out is a packet headed by a quadword naming an address in *IOP*
// memory, and what comes back is headed by a tag naming an address in ours --
// which is why the destination travels in the request rather than into MADR.
_sif_exchange:
    // BOOT-11i: arm the receiving channel first. A transfer runs only when
    // both ends are ready, so a side that arms its receiver after asking has
    // arranged for neither end to move.
    liw   $t4, DMA_SIF0
    liw   $t5, DMA_CHAIN
    sw    $t5, 0($t4)

    // Put the ten-byte name, the verb and the destination where the outgoing
    // channel will read them: one quadword past the header.
    la    $t0, _sif_request
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
    sw    $a2, 16($t0)                   // where the answer is to land here
    sw    $zero, 20($t0)
    sw    $zero, 24($t0)
    sw    $zero, 28($t0)

    // The header the IOP's channel reads: the address it published to us, and
    // how many words follow. Its end bit stops that channel after this packet.
    la    $t6, _sif_peer
    lw    $t6, 0($t6)
    liw   $t7, TAG_END
    or    $t6, $t6, $t7
    la    $t3, _sif_packet
    sw    $t6, 0($t3)
    ori   $t6, $zero, REQUEST_WORDS
    sw    $t6, 4($t3)
    sw    $zero, 8($t3)
    sw    $zero, 12($t3)

    // One source tag covers the whole packet, header included: TTE is clear,
    // so the tag itself does not travel and the header leads the data.
    la    $t6, _sif_tag
    liw   $t7, TAG_REFE | PACKET_QUADS
    sw    $t7, 0($t6)
    sw    $t3, 4($t6)
    sw    $zero, 8($t6)
    sw    $zero, 12($t6)

    liw   $t4, DMA_SIF1
    sw    $t6, 0x30($t4)                 // TADR
    liw   $t5, DMA_CHAIN
    sw    $t5, 0($t4)                    // CHCR: start

    // BOOT-11h: wait for the channel. A transfer is not instantaneous, and
    // raising the flag before it has drained tells the IOP to read a buffer
    // the bytes have not reached yet.
_sif_exchange_sent:
    lw    $t5, 0($t4)
    andi  $t5, $t5, DMA_START
    bne   $t5, $zero, _sif_exchange_sent
    nop

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

    // The channel was armed before the request went out; all that is left is
    // to let it finish. The tag the IOP put in front of the data is what says
    // where it lands and how much of it there is.
    liw   $t4, DMA_SIF0
_sif_exchange_received:
    lw    $t5, 0($t4)
    andi  $t5, $t5, DMA_START
    bne   $t5, $zero, _sif_exchange_received
    nop
    jr    $ra
    move  $v0, $a2

    .section .bss, "aw", @nobits
    .align 4
_sif_area:
    .space 256                           // what we publish to the IOP
_sif_peer:
    .space 4                             // and what it publishes to us
    .align 4
_sif_tag:
    .space 16                            // the source tag the outgoing channel reads
_sif_packet:
    .space 16                            // the header the IOP's channel reads
_sif_request:
    .space REQUEST_WORDS * 4             // and the request it heads
