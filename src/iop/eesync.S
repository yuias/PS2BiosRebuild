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
    .set REQUEST_BIT,   0x00000002       // the EE has sent us something
    .set REPLY_BIT,     0x00000002       // we have sent it back
    .set CTRL_ANSWER,   0x00000040

// The IOP's SIF DMA channels, in the second controller's bank. Which channels
// these are was derived by watching the reference's driver rather than assumed
// (docs/analysis/24-sif-data-path.md).
    .set DMA_SIF0, 0xBF801520            // to the EE:   MADR +0, BCR +4, CHCR +8
    .set DMA_SIF1, 0xBF801530            // from the EE
    .set DMA_START, 0x01000000

// What the boot block left for us, at the address `src/boot/iopboot.S` fixes.
    .set BOOT_LIST, 0x00002000
    .set BL_TABLE,  0x008

    .set REQUEST_WORDS, 4                // a 16-byte request: one name
    .set PAYLOAD_WORDS, 64               // and up to 256 bytes back

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

// Serve the EE. A request is a ten-byte archive name; the answer is that
// file's first bytes. The EE cannot write our memory and we cannot write its
// -- everything crosses through the two SIF channels.
_serve:
    lui   $t0, (SIF_MSFLG >> 16) & 0xFFFF
    ori   $t0, $t0, SIF_MSFLG & 0xFFFF
    ori   $t2, $zero, REQUEST_BIT
_serve_wait:
    lw    $t1, 0($t0)
    and   $t1, $t1, $t2
    beq   $t1, $zero, _serve_wait
    nop
    sw    $t2, 0($t0)                    // acknowledge by clearing

    // Take what the EE sent into our own memory.
    la    $t3, _request
    lui   $t4, (DMA_SIF1 >> 16) & 0xFFFF
    ori   $t4, $t4, DMA_SIF1 & 0xFFFF
    sw    $t3, 0($t4)                    // MADR
    lui   $t5, 0x0001
    ori   $t5, $t5, REQUEST_WORDS        // one block of that many words
    sw    $t5, 4($t4)                    // BCR
    lui   $t5, (DMA_START >> 16) & 0xFFFF
    sw    $t5, 8($t4)                    // CHCR: start

    // Resolve the name in the archive, using the table the boot block left.
    la    $a0, _request
    bal   _lookup
    nop
    beq   $v0, $zero, _serve_empty
    nop

    // Copy what we found into the buffer the channel will send from.
    la    $t7, _payload
    move  $t8, $zero
    sltiu $at, $v1, PAYLOAD_WORDS * 4
    bne   $at, $zero, _serve_copy
    nop
    ori   $v1, $zero, PAYLOAD_WORDS * 4
_serve_copy:
    sltu  $at, $t8, $v1
    beq   $at, $zero, _serve_send
    nop
    addu  $t9, $v0, $t8
    lw    $at, 0($t9)
    addu  $t9, $t7, $t8
    sw    $at, 0($t9)
    b     _serve_copy
    addiu $t8, $t8, 4

_serve_empty:
    la    $t7, _payload
    sw    $zero, 0($t7)

_serve_send:
    la    $t3, _payload
    lui   $t4, (DMA_SIF0 >> 16) & 0xFFFF
    ori   $t4, $t4, DMA_SIF0 & 0xFFFF
    sw    $t3, 0($t4)                    // MADR
    lui   $t5, 0x0001
    ori   $t5, $t5, PAYLOAD_WORDS
    sw    $t5, 4($t4)                    // BCR
    lui   $t5, (DMA_START >> 16) & 0xFFFF
    sw    $t5, 8($t4)                    // CHCR: start

    lui   $t3, (SIF_SMFLG >> 16) & 0xFFFF
    ori   $t3, $t3, SIF_SMFLG & 0xFFFF
    ori   $t5, $zero, REPLY_BIT
    sw    $t5, 0($t3)                    // our write sets: the answer is ready
    b     _serve
    nop

// _lookup(name) -> address, size, or 0. Walks the archive's table, which the
// boot block left at BOOT_LIST + BL_TABLE, accumulating aligned sizes the way
// ARC-3 says offsets are implied rather than stored.
_lookup:
    lui   $t0, (BOOT_LIST >> 16) & 0xFFFF
    ori   $t0, $t0, BOOT_LIST & 0xFFFF
    lw    $t0, BL_TABLE($t0)
    beq   $t0, $zero, _lookup_miss
    nop
    lw    $t1, 12($t0)                   // entry 0 is RESET, and the table
    addiu $t1, $t1, 15                   // follows it, so the image base is
    srl   $t1, $t1, 4                    // the table less that file's size
    sll   $t1, $t1, 4
    subu  $t2, $t0, $t1                  // this entry's file address
    move  $t3, $t0
_lookup_entry:
    lw    $t4, 0($t3)
    lw    $t5, 4($t3)
    lw    $t6, 12($t3)
    or    $t7, $t4, $t5
    or    $t7, $t7, $t6
    beq   $t7, $zero, _lookup_miss
    nop
    move  $t8, $zero
_lookup_compare:
    addu  $t9, $t3, $t8
    lbu   $t4, 0($t9)
    addu  $t9, $a0, $t8
    lbu   $t5, 0($t9)
    bne   $t4, $t5, _lookup_step
    nop
    addiu $t8, $t8, 1
    slti  $t7, $t8, 10
    bne   $t7, $zero, _lookup_compare
    nop
    move  $v0, $t2
    jr    $ra
    move  $v1, $t6
_lookup_step:
    addiu $t6, $t6, 15
    srl   $t6, $t6, 4
    sll   $t6, $t6, 4
    addu  $t2, $t2, $t6
    b     _lookup_entry
    addiu $t3, $t3, 16
_lookup_miss:
    jr    $ra
    move  $v0, $zero

    .section .bss, "aw", @nobits
    .align 4
_sif_area:
    .space 64                            // what we published to the EE
_ee_area:
    .space 4                             // and what it published to us
    .align 4
_request:
    .space REQUEST_WORDS * 4             // the name the EE asked for
    .align 4
_payload:
    .space PAYLOAD_WORDS * 4             // and the bytes we send back
