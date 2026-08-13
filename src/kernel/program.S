// The syscall entries of the program loader.
//
// docs/spec/04-ee-kernel.md EE-9: slot 0x06 is the program loader, slot 0x7B
// is the same call with the path pinned to `rom0:OSDSYS`, and the default boot
// invokes it with a single argument.
//
// Only the entries are here. They jump into the program they loaded and never
// return, which no compiler expresses; the reading, fetching and placing are
// `program.cpp`.

    .set noreorder
    .set noat

.macro la reg, symbol
    lui   \reg, %hi(\symbol)
    addiu \reg, \reg, %lo(\symbol)
.endm

    .globl _sys_load_program
    .globl _sys_load_osd
    .globl _boot_default

    .section .text, "ax"

// Slot 0x06: load(path, argc, argv). It does not return -- the program it
// loads replaces the caller.
_sys_load_program:
    addiu $sp, $sp, -32
    sw    $ra, 0($sp)
    sw    $a1, 8($sp)                    // argc
    sw    $a2, 12($sp)                   // argv

    // The path is `rom0:NAME`. Only the IOP can read that device (EE-9d), and
    // it wants the bare archive name, so take what follows the colon.
    jal   skipDevice
    nop
    move  $a0, $v0
    la    $a1, staging
    jal   fetchFile
    nop
    beq   $v0, $zero, _load_failed
    nop

    la    $a0, staging
    jal   placeSegments
    nop
    beq   $v0, $zero, _load_failed
    nop

    move  $t9, $v0                       // the entry point
    lw    $a0, 8($sp)
    lw    $a1, 12($sp)
    jr    $t9                            // EE-9: the program replaces us
    nop

_load_failed:
    la    $a0, kNoProgram
    jal   _print
    nop
    lw    $ra, 0($sp)
    addiu $sp, $sp, 32
    jr    $ra
    addiu $v0, $zero, -1

// Slot 0x7B: EE-9b, the same call with the path pinned. The reference is a
// four-instruction wrapper that shifts the arguments along and tail-calls the
// loader; so is this.
_sys_load_osd:
    move  $a2, $a1
    move  $a1, $a0
    la    $a0, kRom0Osdsys
    b     _sys_load_program
    nop

// EE-9c: the default boot runs that program with one argument.
_boot_default:
    addiu $sp, $sp, -32
    la    $t0, kBootBrowser
    sw    $t0, 16($sp)
    la    $a0, kRom0Osdsys
    addiu $a1, $zero, 1
    b     _sys_load_program
    addiu $a2, $sp, 16
