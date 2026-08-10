// The boot block: the first code either CPU runs.
//
// Implements docs/spec/03-boot-chain.md BOOT-1 to BOOT-6 and
// docs/spec/04-ee-kernel.md EE-1 to EE-3. Both processors enter at
// 0xBFC00000; the dispatch below is the only code they share.
//
// Fixed offsets in this file are ABI, not layout choices: the EE entry at
// 0x800 and the IOP entry at 0x2000 are where the dispatch sends each CPU
// (BOOT-1a), and the identification block sits at 0x100 because that is where
// tools look for it (BOOT-2).

    .set noreorder
    .set noat

// The R5900's ordering barrier. `sync.p` is not in the MIPS III the assembler
// accepts, and it is the form the EE needs after every CP0 write.
.macro sync_p
    .word 0x0000040F
.endm

// Load a 32-bit constant. `%hi`/`%lo` are not used: `%hi` compensates for the
// sign extension of a following `addiu`, which is wrong when the low half is
// applied with `ori` -- any constant whose low half is >= 0x8000 would land
// 0x10000 too high.
.macro liw reg, value
    lui   \reg, ((\value) >> 16) & 0xFFFF
    ori   \reg, \reg, (\value) & 0xFFFF
.endm

// Load the address of a symbol, where the assembler must do the relocation and
// the `%hi`/`%lo` pairing with `addiu` is the correct one.
.macro la reg, symbol
    lui   \reg, %hi(\symbol)
    addiu \reg, \reg, %lo(\symbol)
.endm

    .section .text, "ax"
    .globl _reset

// ---------------------------------------------------------------- 0x000
// BOOT-1: separate the two processors before anything machine-specific.
_reset:
    mfc0  $k0, $15                  // PRId
    slti  $k1, $k0, 0x59            // BOOT-1a: slti against 0x59, full word
    bne   $k1, $zero, _reset_iop
    nop
    liw   $k0, 0xBFC00800           // >= 0x59: the EE
    jr    $k0
    nop
_reset_iop:
    liw   $k0, 0xBFC02000
    jr    $k0
    nop

// ---------------------------------------------------------------- 0x100
// BOOT-2: the identification block. Placement is preserved; the text is ours
// (BOOT-2a -- the reference's vendor strings are out of bounds).
    .org 0x100
_identification:
    .asciz "2026-08-10"
    .asciz "PS2BiosRebuild"
    .asciz "https://github.com/ru4nm31/PS2BiosRebuild"

// ---------------------------------------------------------------- 0x800
// The EE path. docs/spec/04-ee-kernel.md EE-1 onwards.
    .org 0x800
_ee_reset:
    // EE-1: the four CP0 registers, each write followed by sync.p.
    liw   $k0, 0x00073003
    mtc0  $k0, $16                  // Config
    sync_p
    liw   $k0, 0x70400000
    mtc0  $k0, $12                  // Status
    sync_p
    mtc0  $zero, $9                 // Count
    sync_p
    ori   $k0, $zero, 1
    mtc0  $k0, $11                  // Compare
    sync_p

    liw   $k0, 0xB000F500
    addiu $k1, $zero, -1
    sw    $k1, 0($k0)

    // EE-1a: one TLB entry, at index 0, mapping the scratchpad. It must exist
    // before the next step, because the reset path has no other memory.
    mtc0  $zero, $0                 // Index = 0
    mtc0  $zero, $5                 // PageMask = 0
    liw   $k0, 0x70000000
    mtc0  $k0, $10                  // EntryHi
    liw   $k0, 0x80000007
    mtc0  $k0, $2                   // EntryLo0
    ori   $k0, $zero, 0x0007
    mtc0  $k0, $3                   // EntryLo1
    sync_p
    tlbwi
    sync_p

    // EE-1b: the stack lives in that mapping. Main RAM is not usable yet.
    liw   $sp, 0x70003FF0

    // EE-2: bring the memory controller up. The reference reaches `RDRAM` by
    // a hard-coded address; resolving it by name instead removes the constant
    // that EE-2a warns must never drift from the layout.
    liw   $a0, 0x9FC00000
    liw   $a1, 0x9FC80000
    liw   $a2, 0x41524452           // "RDRA"
    liw   $a3, 0x0000004D           // "M"
    jal   _ee_find
    move  $t9, $zero                // last two name bytes: none
    beq   $v0, $zero, _ee_stop
    nop
    jalr  $v0
    nop

    // EE-3: resolve KERNEL, copy it to physical 0, enter it.
    liw   $a0, 0x9FC00000
    liw   $a1, 0x9FC80000
    liw   $a2, 0x4E52454B           // "KERN"
    liw   $a3, 0x00004C45           // "EL"
    jal   _ee_find
    move  $t9, $zero
    beq   $v0, $zero, _ee_stop
    nop

    // EE-3b: to 0xA0000000 -- physical zero -- for the file's size rounded up
    // to 16. The reference moves 16 bytes an iteration with lq/sq; four words
    // land the same bytes and need no R5900-only encodings here.
    move  $t0, $v0                  // source, in the ROM
    move  $t1, $v1                  // length
    liw   $t2, 0xA0000000           // destination
    addiu $t1, $t1, 15
    srl   $t1, $t1, 4
    sll   $t1, $t1, 4
_ee_copy:
    lw    $t3, 0($t0)
    lw    $t4, 4($t0)
    lw    $t5, 8($t0)
    lw    $t6, 12($t0)
    sw    $t3, 0($t2)
    sw    $t4, 4($t2)
    sw    $t5, 8($t2)
    sw    $t6, 12($t2)
    addiu $t0, $t0, 16
    addiu $t2, $t2, 16
    addiu $t1, $t1, -16
    bgtz  $t1, _ee_copy
    nop

    // EE-3d: enter the kernel. Its first 0x1000 bytes are the vector page, so
    // the entry point is what follows them (EE-4).
    liw   $k0, 0x80001000
    jr    $k0
    nop

_ee_stop:
    // Nothing can be done without the kernel; stop where it can be seen
    // rather than run on (the EE has no POST register: BOOT-5b's IOP idiom).
    b     _ee_stop
    nop

// _ee_find(start, end, name0, name1, name2) -> address, size
//
// The self-locating scan of ARC-4, as BOOT-6a describes it: find a candidate
// table by its `RESET` first entry, then walk entries accumulating
// align16(size) to recover each file's offset. Returns 0 when the name is not
// present.
_ee_find:
    move  $t0, $a0
_ee_find_table:
    lw    $t1, 0($t0)
    liw   $t2, 0x45534552           // "RESE"
    bne   $t1, $t2, _ee_find_next
    nop
    lbu   $t1, 4($t0)
    ori   $t2, $zero, 0x54          // "T"
    bne   $t1, $t2, _ee_find_next
    nop
    lw    $t1, 12($t0)              // entry 0's size
    andi  $t2, $t1, 15              // ARC-4: must be 16-aligned
    beq   $t2, $zero, _ee_find_walk
    nop
_ee_find_next:
    addiu $t0, $t0, 16
    sltu  $t1, $t0, $a1
    bne   $t1, $zero, _ee_find_table
    nop
    jr    $ra
    move  $v0, $zero                // no table in range

_ee_find_walk:
    // The table sits immediately after the RESET file, so the image base is
    // the table's address less that file's aligned size (ARC-3).
    lw    $t1, 12($t0)
    addiu $t1, $t1, 15
    srl   $t1, $t1, 4
    sll   $t1, $t1, 4
    subu  $t2, $t0, $t1             // $t2: this entry's file address
    move  $t3, $t0                  // $t3: this entry
_ee_find_entry:
    lw    $t4, 0($t3)
    lw    $t5, 4($t3)
    lhu   $t6, 8($t3)
    lw    $t7, 12($t3)              // size
    bne   $t4, $a2, _ee_find_step
    nop
    bne   $t5, $a3, _ee_find_step
    nop
    bne   $t6, $t9, _ee_find_step
    nop
    move  $v0, $t2                  // found
    jr    $ra
    move  $v1, $t7
_ee_find_step:
    or    $t8, $t4, $t5
    or    $t8, $t8, $t7
    beq   $t8, $zero, _ee_find_end  // ARC-2's all-zero terminator
    nop
    addiu $t7, $t7, 15
    srl   $t7, $t7, 4
    sll   $t7, $t7, 4
    addu  $t2, $t2, $t7
    addiu $t3, $t3, 16
    b     _ee_find_entry
    nop
_ee_find_end:
    jr    $ra
    move  $v0, $zero

// ---------------------------------------------------------------- 0x2000
// The IOP path. BOOT-4's sequence, in its order, with BOOT-5's POST codes.
    .org 0x2000
_iop_reset:
    // BOOT-4 step 1: apply the bus-configuration table. BOOT-3 decides which
    // table; a retail machine takes the false branch, and its table is the one
    // carrying POST 0xFC as a side effect (BOOT-5).
    mfc0  $t0, $15
    slti  $t1, $t0, 0x10            // BOOT-3: PRId < 0x10
    bne   $t1, $zero, _iop_dev
    nop
    liw   $t0, 0xBF801450
    lw    $t0, 0($t0)
    andi  $t0, $t0, 8               // BOOT-3: or the alternate-machine bit
    bne   $t0, $zero, _iop_dev
    nop
    la    $t0, _iop_bus_retail
    b     _iop_bus_apply
    nop
_iop_dev:
    la    $t0, _iop_bus_dev
_iop_bus_apply:
    // BOOT-4 step 1: the first pair keeps bit 0x1000 of the register's current
    // value; every later pair is written verbatim.
    lw    $t1, 0($t0)               // register address
    lw    $t2, 4($t0)               // value
    lw    $t3, 0($t1)
    andi  $t3, $t3, 0x1000
    or    $t2, $t2, $t3
    sw    $t2, 0($t1)
    addiu $t0, $t0, 8
_iop_bus_pair:
    lw    $t1, 0($t0)
    beq   $t1, $zero, _iop_bus_done
    nop
    lw    $t2, 4($t0)
    sw    $t2, 0($t1)
    addiu $t0, $t0, 8
    b     _iop_bus_pair
    nop
_iop_bus_done:
    liw   $t0, 0xBF802070
    ori   $t1, $zero, 0x02          // POST 2: bus table applied
    sb    $t1, 0($t0)

    // BOOT-4 step 2: clear the general-purpose registers.
    move  $at, $zero
    move  $v0, $zero
    move  $v1, $zero
    move  $a0, $zero
    move  $a1, $zero
    move  $a2, $zero
    move  $a3, $zero
    move  $t0, $zero
    move  $t1, $zero
    move  $t2, $zero
    move  $t3, $zero
    move  $t4, $zero
    move  $t5, $zero
    move  $t6, $zero
    move  $t7, $zero
    move  $s0, $zero
    move  $s1, $zero
    move  $s2, $zero
    move  $s3, $zero
    move  $s4, $zero
    move  $s5, $zero
    move  $s6, $zero
    move  $s7, $zero
    move  $t8, $zero
    move  $t9, $zero
    move  $gp, $zero
    move  $sp, $zero
    move  $fp, $zero
    move  $ra, $zero

    // BOOT-4 step 3: clear low RAM, where the kernel builds its structures.
    move  $t0, $zero
_iop_clear:
    sw    $zero, 0($t0)
    addiu $t0, $t0, 4
    slti  $t1, $t0, 0xF80
    bne   $t1, $zero, _iop_clear
    nop
    liw   $t0, 0xBF802070
    ori   $t1, $zero, 0x03          // POST 3: low RAM cleared
    sb    $t1, 0($t0)

    // BOOT-4 step 4: prime the cache through the uncached alias.
    liw   $t0, 0xA0000000
    lw    $zero, 0x00($t0)
    lw    $zero, 0x04($t0)
    lw    $zero, 0x08($t0)
    lw    $zero, 0x0C($t0)
    lw    $zero, 0x10($t0)
    lw    $zero, 0x14($t0)
    lw    $zero, 0x18($t0)
    lw    $zero, 0x1C($t0)

    liw   $t0, 0xBF802070
    ori   $t1, $zero, 0x04          // POST 4: parameters selected
    sb    $t1, 0($t0)

    // BOOT-4 step 2, continued: the COP0 half.
    mtc0  $zero, $12                // Status
    mtc0  $zero, $13                // Cause
    liw   $t0, 0xBF802070
    ori   $t1, $zero, 0x05          // POST 5: COP0 cleared
    sb    $t1, 0($t0)

    // BOOT-4 step 5 and BOOT-6: latch the RAM size and hand off by name.
    liw   $t0, 0xBF801060
    ori   $t1, $zero, 0x0B          // 2 MiB, the retail record
    sw    $t1, 0($t0)
    liw   $t0, 0xBF802070
    ori   $t1, $zero, 0x08          // POST 8: entering the IOPBOOT handoff
    sb    $t1, 0($t0)

    liw   $a0, 0xBFC00000
    liw   $a1, 0xBFC80000           // BOOT-6b: the first 512 KiB only
    liw   $a2, 0x42504F49           // "IOPB"
    liw   $a3, 0x00544F4F           // "OOT"
    jal   _iop_find
    move  $t9, $zero
    beq   $v0, $zero, _iop_missing
    nop

    liw   $t0, 0xBF802070
    ori   $t1, $zero, 0x09          // POST 9: IOPBOOT RAM size latched
    sb    $t1, 0($t0)
    move  $t0, $v0
    ori   $a0, $zero, 0x02          // BOOT-6c: the RAM-size byte
    jr    $t0                       // BOOT-6c: entered with jr; never returns
    nop

_iop_missing:
    // BOOT-6d and BOOT-5b: a failed lookup is a terminal stop, not a fall
    // through into whatever follows.
    liw   $t0, 0xBF802070
    ori   $t1, $zero, 0xFA
    sb    $t1, 0($t0)
_iop_stop:
    b     _iop_stop
    nop

// The same scan again, for the IOP. BOOT-6a: the reference writes it out
// per-consumer rather than sharing one copy, and so must a rebuild -- the two
// processors cannot call into each other's code.
_iop_find:
    move  $t0, $a0
_iop_find_table:
    lw    $t1, 0($t0)
    liw   $t2, 0x45534552
    bne   $t1, $t2, _iop_find_next
    nop
    lbu   $t1, 4($t0)
    ori   $t2, $zero, 0x54
    bne   $t1, $t2, _iop_find_next
    nop
    lw    $t1, 12($t0)
    andi  $t2, $t1, 15
    beq   $t2, $zero, _iop_find_walk
    nop
_iop_find_next:
    addiu $t0, $t0, 16
    sltu  $t1, $t0, $a1
    bne   $t1, $zero, _iop_find_table
    nop
    jr    $ra
    move  $v0, $zero

_iop_find_walk:
    lw    $t1, 12($t0)
    addiu $t1, $t1, 15
    srl   $t1, $t1, 4
    sll   $t1, $t1, 4
    subu  $t2, $t0, $t1
    move  $t3, $t0
_iop_find_entry:
    lw    $t4, 0($t3)
    lw    $t5, 4($t3)
    lhu   $t6, 8($t3)
    lw    $t7, 12($t3)
    bne   $t4, $a2, _iop_find_step
    nop
    bne   $t5, $a3, _iop_find_step
    nop
    bne   $t6, $t9, _iop_find_step
    nop
    move  $v0, $t2
    jr    $ra
    move  $v1, $t7
_iop_find_step:
    or    $t8, $t4, $t5
    or    $t8, $t8, $t7
    beq   $t8, $zero, _iop_find_end
    nop
    addiu $t7, $t7, 15
    srl   $t7, $t7, 4
    sll   $t7, $t7, 4
    addu  $t2, $t2, $t7
    addiu $t3, $t3, 16
    b     _iop_find_entry
    nop
_iop_find_end:
    jr    $ra
    move  $v0, $zero

// The bus-configuration tables of BOOT-4 step 1: (register, value) pairs,
// terminated by a zero address. The POST write in each is what emits BOOT-5's
// first code before any code has run.
//
// Only the POST entry is populated so far. The remaining pairs configure the
// SSBUS and RAM controllers, whose values the hardware dictates; they belong
// with the hardware bring-up work and are not needed to boot an emulator.
    .align 4
_iop_bus_retail:
    .word 0xBF802070, 0x000000FC    // BOOT-5: POST 0xFC, the retail table
    .word 0x00000000, 0x00000000
_iop_bus_dev:
    .word 0xBF802070, 0x000000FE    // BOOT-5: POST 0xFE, the other machine
    .word 0x00000000, 0x00000000
