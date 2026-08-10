// SYSMEM: the IOP's memory manager -- the first module the boot list names.
//
// This is the module's *shape* so far: the container of
// docs/spec/02-module-abi.md IRX-1, the library table of IRX-4, the ordinals of
// IRX-6 and the entry convention of IRX-12. The allocator behind ordinals 4
// and 5 is not written yet, and its stubs say so by failing rather than
// pretending to succeed.
//
// The IOP is an R3000A: MIPS I only.

    .set noreorder
    .set noat

// Scaffolding: somewhere the loader test can see that the entry really ran.
// It goes when the module has behaviour worth observing instead.
    .set RAN_MARKER, 0x00003000

    .section .text, "ax"
    .globl _module_start

// IRX-12: entry(argc, argv, 0, module_record), with the module's own $gp
// installed. `return & 3` decides residency -- clear keeps the module, set
// frees it -- and a memory manager stays.
_module_start:
    lui   $t0, (RAN_MARKER >> 16) & 0xFFFF
    ori   $t0, $t0, RAN_MARKER & 0xFFFF
    lui   $t1, 0x5359                    // "SY"
    ori   $t1, $t1, 0x534D               // "SM"
    sw    $t1, 0($t0)
    sw    $a3, 4($t0)                    // the module record we were given
    jr    $ra
    move  $v0, $zero                     // resident

// IRX-7: slots 0 and 1 are reserved hooks that nothing imports. They are kept
// occupied rather than removed (IRX-6a), so they need somewhere to point.
_reserved_hook:
    jr    $ra
    move  $v0, $zero

// Ordinals 4 and 5 are the allocator and its deallocator (docs/analysis/05).
// Until there is a heap to allocate from, a caller must be told it failed.
_allocate:
    jr    $ra
    move  $v0, $zero                     // no memory
_deallocate:
    jr    $ra
    addiu $v0, $zero, -1                 // nothing to release

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
    .align 4
_heap_anchor:
    .space 64
