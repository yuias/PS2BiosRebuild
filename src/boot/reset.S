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

    // Everything from here needs the stack that line above just made, so it
    // is C++: `reset_ee.cpp`. EE-2 onwards resolves two files out of the
    // archive, calls one and copies the other into place.
    j     eeReset
    nop

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

    // Everything from here needs a stack, so it is C++: `reset_iop.cpp`. The
    // register clear above left `$sp` zero, as BOOT-4 step 2 requires; low RAM
    // is about to be cleared, so the stack goes at the top of the 2 MiB a
    // retail machine has.
    liw   $sp, 0x001FFFF0
    j     iopReset
    nop

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
