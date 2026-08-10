// SYSMEM: the IOP's memory manager -- the first module the boot list names.
//
// The container is docs/spec/02-module-abi.md IRX-1, the library table IRX-4,
// the ordinals IRX-6 and the entry convention IRX-12. Ordinals 4 and 5 are the
// allocator and its deallocator, which `docs/analysis/05-sysmem-and-loadcore.md`
// identified from how every other module calls them.
//
// The IOP is an R3000A: MIPS I only.

    .set noreorder
    .set noat

// Load a symbol's address. The pair is `lui`/`addiu` so the linker's %hi carry
// compensation is the right one, and the loader's HI16/LO16 fixups (IRX-3a)
// are what make it work once the module has been rebased.
.macro la reg, symbol
    lui   \reg, %hi(\symbol)
    addiu \reg, \reg, %lo(\symbol)
.endm

// Provisional extent. The reference takes the machine's memory size from the
// boot parameters the reset path latched (spec/03 BOOT-4 step 5); until that
// is plumbed through, a retail 2 MiB machine's spare middle is the heap, kept
// clear of the stack the boot block put at the top.
    .set HEAP_START, 0x00100000
    .set HEAP_END,   0x001F0000
    .set ALIGNMENT,  16

    .section .text, "ax"
    .globl _module_start

// IRX-12: entry(argc, argv, 0, module_record), with the module's own $gp
// installed. `return & 3` decides residency -- clear keeps the module -- and a
// memory manager stays.
_module_start:
    lui   $t0, (HEAP_START >> 16) & 0xFFFF
    ori   $t0, $t0, HEAP_START & 0xFFFF
    la    $t1, _heap_cursor
    sw    $t0, 0($t1)
    sw    $zero, 4($t1)
    jr    $ra
    move  $v0, $zero                     // resident

// IRX-7: slots 0 and 1 are reserved hooks that nothing imports, kept occupied
// rather than removed (IRX-6a), so they need somewhere to point.
_reserved_hook:
    jr    $ra
    move  $v0, $zero

// Ordinal 4: allocate(size) -> address, or 0.
//
// A bump allocator, which is all the boot needs: every allocation the module
// list makes lives as long as the machine does. Anything with a real lifetime
// arrives with the heap of `HEAPLIB`.
_allocate:
    beq   $a0, $zero, _allocate_fail
    nop
    addiu $a0, $a0, ALIGNMENT - 1
    srl   $a0, $a0, 4
    sll   $a0, $a0, 4                    // rounded up to 16
    la    $t3, _heap_cursor
    lw    $t0, 0($t3)
    addu  $t1, $t0, $a0
    lui   $t2, (HEAP_END >> 16) & 0xFFFF
    ori   $t2, $t2, HEAP_END & 0xFFFF
    sltu  $at, $t2, $t1
    bne   $at, $zero, _allocate_fail     // past the end of the heap
    nop
    sw    $t1, 0($t3)
    sw    $t0, 4($t3)
    jr    $ra
    move  $v0, $t0
_allocate_fail:
    jr    $ra
    move  $v0, $zero

// Ordinal 5: release(address) -> 0, or -1.
//
// A bump allocator can give back only what it handed out last. Saying so is
// better than accepting every address and leaking: a caller that frees out of
// order finds out at once rather than exhausting the heap later.
_deallocate:
    la    $t3, _heap_cursor
    lw    $t0, 4($t3)
    beq   $t0, $zero, _deallocate_fail
    nop
    bne   $a0, $t0, _deallocate_fail
    nop
    la    $t1, _heap_cursor
    sw    $t0, 0($t1)
    sw    $zero, 4($t1)
    jr    $ra
    move  $v0, $zero
_deallocate_fail:
    jr    $ra
    addiu $v0, $zero, -1

_unimplemented:
    jr    $ra
    addiu $v0, $zero, -1

// IRX-4: the export table. It lives in `.data` because registration writes the
// registry link over its first word (IRX-4c) -- a table in read-only memory
// could not be registered at all.
    .section .data, "aw"
    .align 2
    .globl _sysmem_exports
_sysmem_exports:
    .word 0x41C00000                     // magic, consumed at registration
    .word 0                              // head of the bound-client list
    .short 0x0101                        // version 1.01, BCD
    .short 0                             // flags
    .ascii "sysmem\0\0"                  // IRX-4a: eight bytes, NUL-padded
    // IRX-5: absolute function pointers, each carrying an R_MIPS_32 fixup,
    // terminated by a zero word (IRX-5b).
    .word _reserved_hook                 // 0  reserved
    .word _reserved_hook                 // 1  reserved
    .word _unimplemented                 // 2
    .word _unimplemented                 // 3
    .word _allocate                      // 4  allocate
    .word _deallocate                    // 5  release
    .word _unimplemented                 // 6
    .word _unimplemented                 // 7
    .word 0                              // IRX-5b: the terminator

    .section .bss, "aw", @nobits
    .align 2
_heap_cursor:
    .space 4                             // and _heap_last at +4
