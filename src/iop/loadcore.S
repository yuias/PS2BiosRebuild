// LOADCORE: the module registry, and the services modules call to join it.
//
// docs/spec/02-module-abi.md IRX-10: two entry points register an export
// table, and they are not interchangeable -- ordinal 6 compares tag and major
// version and accepts only a strictly greater minor, ordinal 10 pins a table
// at the head with no comparison at all.
//
// The registry itself is a single word of RAM, and the boot block's loader
// already keeps it: a module loaded before this one still has to be
// registered, so the head cannot live inside this module's own data. The
// address is shared with `src/boot/iopboot.S`.
//
// This module also imports `sysmem` 4, which is the first cross-module call in
// the image and therefore the first test of the binding of IRX-9.

    .set noreorder
    .set noat

.macro la reg, symbol
    lui   \reg, %hi(\symbol)
    addiu \reg, \reg, %lo(\symbol)
.endm

    .set REGISTRY_HEAD, 0x00001F80
    .set EXPORT_MAGIC,  0x41C00000
    .set RAN_MARKER,    0x00003010       // scaffolding, until there is more to see

    .section .text, "ax"
    .globl _module_start

// IRX-12: resident, like any library.
_module_start:
    addiu $sp, $sp, -16
    sw    $ra, 0($sp)
    // Prove the binding worked by using it: ask `sysmem` for memory through
    // the stub the loader rewrote.
    jal   _import_sysmem_allocate
    addiu $a0, $zero, 64
    lui   $t0, (RAN_MARKER >> 16) & 0xFFFF
    ori   $t0, $t0, RAN_MARKER & 0xFFFF
    sw    $v0, 0($t0)
    lw    $ra, 0($sp)
    addiu $sp, $sp, 16
    jr    $ra
    move  $v0, $zero                     // resident

_reserved_hook:
    jr    $ra
    move  $v0, $zero

// Ordinal 6: versioned registration (IRX-10a). A library's identity is its tag
// and major version; the minor is a generation counter and only a strictly
// greater one is allowed to take over.
_register_versioned:
    lw    $t0, 0($a0)
    lui   $t1, (EXPORT_MAGIC >> 16) & 0xFFFF
    bne   $t0, $t1, _register_reject     // not an export table
    nop
    la    $t2, _registry
    lw    $t2, 0($t2)
    lw    $t3, 0($t2)                    // walk from the head
    move  $t3, $t2
_register_walk:
    beq   $t3, $zero, _register_accept
    nop
    lw    $t4, 12($a0)
    lw    $t5, 12($t3)
    bne   $t4, $t5, _register_next
    nop
    lw    $t4, 16($a0)
    lw    $t5, 16($t3)
    bne   $t4, $t5, _register_next
    nop
    lhu   $t4, 8($a0)
    lhu   $t5, 8($t3)
    srl   $t6, $t4, 8
    srl   $t7, $t5, 8
    bne   $t6, $t7, _register_next       // a different library entirely
    nop
    andi  $t6, $t4, 0xFF
    andi  $t7, $t5, 0xFF
    sltu  $at, $t7, $t6
    beq   $at, $zero, _register_reject   // equal or lower: refused
    nop
    b     _register_accept
    nop
_register_next:
    lw    $t3, 0($t3)
    b     _register_walk
    nop

_register_accept:
    la    $t2, _registry
    lw    $t3, 0($t2)
    sw    $t3, 0($a0)                    // IRX-4c: the magic becomes the link
    sw    $a0, 0($t2)
    jr    $ra
    move  $v0, $zero
_register_reject:
    jr    $ra
    addiu $v0, $zero, -1

// Ordinal 10: pinned registration (IRX-10b). No comparison, and flags bit 0 is
// set at run time -- the stored table always has flags 0.
_register_pinned:
    lw    $t0, 0($a0)
    lui   $t1, (EXPORT_MAGIC >> 16) & 0xFFFF
    bne   $t0, $t1, _register_reject
    nop
    lhu   $t4, 10($a0)
    ori   $t4, $t4, 1
    sh    $t4, 10($a0)
    b     _register_accept
    nop

_unimplemented:
    jr    $ra
    addiu $v0, $zero, -1

    .section .data, "aw"
    .align 2
// The registry head, at the address the loader in the boot block uses. It is
// an absolute constant rather than a symbol because the two are separate
// images and only the address can be shared.
_registry:
    .word REGISTRY_HEAD

    .globl _loadcore_exports
_loadcore_exports:
    .word 0x41C00000
    .word 0
    .short 0x0101                        // version 1.01, BCD
    .short 0
    .ascii "loadcore"                    // IRX-4a: exactly eight, no terminator
    .word _reserved_hook                 // 0  reserved
    .word _reserved_hook                 // 1  reserved
    .word _unimplemented                 // 2
    .word _unimplemented                 // 3
    .word _unimplemented                 // 4
    .word _unimplemented                 // 5
    .word _register_versioned            // 6  register, versioned
    .word _unimplemented                 // 7
    .word _unimplemented                 // 8
    .word _unimplemented                 // 9
    .word _register_pinned               // 10 register, pinned
    .word _unimplemented                 // 11
    .word _unimplemented                 // 12
    .word 0                              // IRX-5b

// IRX-8: an import table is stubs, two instructions each, and the loader
// rewrites the first word of each into a jump (IRX-9a). It lives in `.data`
// because it is written to; IOP memory carries no execute permission to lose.
    .align 2
    .globl _sysmem_imports
_sysmem_imports:
    .word 0x41E00000
    .word 0
    .short 0x0101
    .short 0                             // IRX-9b: no low bit, so it is bound
    .ascii "sysmem\0\0"
_import_sysmem_allocate:
    jr    $ra
    addiu $v0, $zero, 4
_import_sysmem_release:
    jr    $ra
    addiu $v0, $zero, 5
    .word 0                              // the terminator
