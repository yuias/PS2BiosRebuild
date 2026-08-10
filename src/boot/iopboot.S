// IOPBOOT: the IOP's module loader.
//
// docs/spec/03-boot-chain.md BOOT-7. The boot block enters this by a plain
// `jr` to its ROM address with the latched RAM-size byte in $a0 (BOOT-6c), so
// it is raw IOP code rather than an IRX module: no ELF header, no relocation,
// no library tables.
//
// It runs in place from the ROM window. The reference is linked at the address
// its archive offset gives it and is aligned there by a padding entry; ours is
// position-independent instead, so no padding entry has to be kept in step
// with the layout. Every address it needs is computed from where it finds
// itself running.
//
// This is an R3000A, so the code stays within MIPS I. It is also why every
// call here is `bal` and never `jal`: `jal` encodes an absolute target within
// the current 256 MiB region, which is a link-time address, while `bal` is
// PC-relative and survives running from wherever the archive put this file.

    .set noreorder
    .set noat

// The result of the parse, in low RAM:
//
//   +0  how many names resolved
//   +4  the base load address the `@` token set
//   +8  (address, size) pairs, in load order
//
// Nothing outside this file reads it yet -- the loader that will is the next
// piece of work -- so the layout is ours to choose, unlike the boot-parameter
// table of BOOT-8 whose address is ABI.
    .set BOOT_LIST,       0x00002000
    .set BOOT_LIST_MAX,   64
// Where a name is assembled before it is looked up. It has to be RAM: the
// only writable memory this early, since the code itself is running from ROM.
    .set NAME_SCRATCH,    0x00001F00

    .set RAM_2MB_STACK,   0x001FFFF0
    .set RAM_8MB_STACK,   0x007FFFF0

    .set ROM_SEARCH_START, 0xBFC00000
    .set ROM_SEARCH_END,   0xBFC80000   // BOOT-6b: the first 512 KiB

    .section .text, "ax"
    .globl _iopboot
_iopboot:
    // BOOT-7: size a stack from the RAM-size argument, and set $gp. A retail
    // machine reports 2 MiB; the other configuration has 8.
    addiu $t0, $zero, 2
    beq   $a0, $t0, _iopboot_small
    nop
    lui   $sp, (RAM_8MB_STACK >> 16) & 0xFFFF
    b     _iopboot_stack_set
    ori   $sp, $sp, RAM_8MB_STACK & 0xFFFF
_iopboot_small:
    lui   $sp, (RAM_2MB_STACK >> 16) & 0xFFFF
    ori   $sp, $sp, RAM_2MB_STACK & 0xFFFF
_iopboot_stack_set:
    move  $gp, $zero

    // Find out where we are. `bal` leaves the address of the next-but-one
    // instruction in $ra, which is this label, and every constant in this file
    // is then an offset from it.
    bal   _iopboot_anchor
    nop
_iopboot_anchor:
    move  $s0, $ra

    // Resolve IOPBTCONF with our own copy of the ARC-4 scan (BOOT-6a: each
    // consumer carries one; the two CPUs cannot call into each other's code).
    lui   $a0, (ROM_SEARCH_START >> 16) & 0xFFFF
    ori   $a0, $a0, ROM_SEARCH_START & 0xFFFF
    lui   $a1, (ROM_SEARCH_END >> 16) & 0xFFFF
    ori   $a1, $a1, ROM_SEARCH_END & 0xFFFF
    addiu $a2, $s0, _name_iopbtconf - _iopboot_anchor
    bal   _iopboot_find
    nop
    beq   $v0, $zero, _iopboot_stop      // BOOT-9a: no list, no boot
    nop

    move  $s1, $v0                       // the list text
    addu  $s2, $s1, $v1                  // one past its end
    move  $s3, $zero                     // modules recorded
    lui   $s4, (BOOT_LIST >> 16) & 0xFFFF
    ori   $s4, $s4, BOOT_LIST & 0xFFFF
    addiu $s5, $zero, 0                  // the base load address, from `@`

// BOOT-9: whitespace-separated tokens, where any byte below 0x20 ends one.
_iopboot_token:
    sltu  $t0, $s1, $s2
    beq   $t0, $zero, _iopboot_done
    nop
    lbu   $t1, 0($s1)
    sltiu $t0, $t1, 0x20                 // a separator?
    beq   $t0, $zero, _iopboot_token_start
    nop
    b     _iopboot_token
    addiu $s1, $s1, 1

_iopboot_token_start:
    addiu $t2, $zero, 0x40               // '@'
    beq   $t1, $t2, _iopboot_base
    nop
    addiu $t2, $zero, 0x23               // '#'
    beq   $t1, $t2, _iopboot_skip        // BOOT-9b: a directive is not a name
    nop
    b     _iopboot_module
    nop

// `@` sets the base load address; the rest of the token is hexadecimal.
_iopboot_base:
    addiu $s1, $s1, 1
    move  $s5, $zero
_iopboot_base_digit:
    sltu  $t0, $s1, $s2
    beq   $t0, $zero, _iopboot_done
    nop
    lbu   $t1, 0($s1)
    sltiu $t0, $t1, 0x20
    bne   $t0, $zero, _iopboot_token     // the token ended
    nop
    addiu $t2, $t1, -0x30                // '0'
    sltiu $t0, $t2, 10
    bne   $t0, $zero, _iopboot_base_add
    nop
    addiu $t2, $t1, -0x57                // 'a' - 10
    sltiu $t0, $t2, 16
    bne   $t0, $zero, _iopboot_base_add
    nop
    addiu $t2, $t1, -0x37                // 'A' - 10
_iopboot_base_add:
    sll   $s5, $s5, 4
    addu  $s5, $s5, $t2
    b     _iopboot_base_digit
    addiu $s1, $s1, 1

_iopboot_skip:
    sltu  $t0, $s1, $s2
    beq   $t0, $zero, _iopboot_done
    nop
    lbu   $t1, 0($s1)
    sltiu $t0, $t1, 0x20
    bne   $t0, $zero, _iopboot_token
    nop
    b     _iopboot_skip
    addiu $s1, $s1, 1

// Anything else is a module name. ARC-9 resolves it; BOOT-9a says a miss
// aborts the boot rather than skipping the entry.
_iopboot_module:
    // Copy the token into the ten-byte, NUL-padded form an entry name has.
    lui   $t3, (NAME_SCRATCH >> 16) & 0xFFFF
    ori   $t3, $t3, NAME_SCRATCH & 0xFFFF
    move  $t4, $zero
_iopboot_module_clear:
    addu  $t5, $t3, $t4
    sb    $zero, 0($t5)
    addiu $t4, $t4, 1
    slti  $t0, $t4, 10
    bne   $t0, $zero, _iopboot_module_clear
    nop
    move  $t4, $zero
_iopboot_module_copy:
    sltu  $t0, $s1, $s2
    beq   $t0, $zero, _iopboot_module_find
    nop
    lbu   $t1, 0($s1)
    sltiu $t0, $t1, 0x20
    bne   $t0, $zero, _iopboot_module_find
    nop
    slti  $t0, $t4, 10
    beq   $t0, $zero, _iopboot_module_next_char
    nop
    addu  $t5, $t3, $t4
    sb    $t1, 0($t5)
    addiu $t4, $t4, 1
_iopboot_module_next_char:
    b     _iopboot_module_copy
    addiu $s1, $s1, 1

_iopboot_module_find:
    lui   $a0, (ROM_SEARCH_START >> 16) & 0xFFFF
    ori   $a0, $a0, ROM_SEARCH_START & 0xFFFF
    lui   $a1, (ROM_SEARCH_END >> 16) & 0xFFFF
    ori   $a1, $a1, ROM_SEARCH_END & 0xFFFF
    bal   _iopboot_find
    move  $a2, $t3
    beq   $v0, $zero, _iopboot_stop      // BOOT-9a
    nop
    slti  $t0, $s3, BOOT_LIST_MAX
    beq   $t0, $zero, _iopboot_stop
    nop
    sll   $t1, $s3, 3
    addu  $t1, $s4, $t1
    sw    $v0, 8($t1)                    // the module's ROM address
    sw    $v1, 12($t1)                   // and its size
    addiu $s3, $s3, 1
    b     _iopboot_token
    nop

_iopboot_done:
    sw    $s3, 0($s4)                    // how many names resolved
    sw    $s5, 4($s4)                    // and the base they load at
    // The modules are located but not yet loaded: turning an IRX file into a
    // running module is docs/spec/02-module-abi.md's subject and the next
    // piece of work. Until then the boot ends here rather than pretending.
_iopboot_stop:
    b     _iopboot_stop
    nop

// _iopboot_find(start, end, name) -> address, size
//
// The self-locating scan again (ARC-4, BOOT-6a), this time taking the name as
// a pointer to ten bytes rather than as immediates, because the names come out
// of the boot list at run time.
_iopboot_find:
    move  $t0, $a0
_iopboot_find_table:
    lw    $t1, 0($t0)
    lui   $t2, 0x4553                    // "RESE"
    ori   $t2, $t2, 0x4552
    bne   $t1, $t2, _iopboot_find_next
    nop
    lbu   $t1, 4($t0)
    addiu $t2, $zero, 0x54               // "T"
    bne   $t1, $t2, _iopboot_find_next
    nop
    lw    $t1, 12($t0)
    andi  $t2, $t1, 15
    beq   $t2, $zero, _iopboot_find_walk
    nop
_iopboot_find_next:
    addiu $t0, $t0, 16
    sltu  $t1, $t0, $a1
    bne   $t1, $zero, _iopboot_find_table
    nop
    jr    $ra
    move  $v0, $zero

_iopboot_find_walk:
    lw    $t1, 12($t0)
    addiu $t1, $t1, 15
    srl   $t1, $t1, 4
    sll   $t1, $t1, 4
    subu  $t2, $t0, $t1                  // this entry's file address
    move  $t3, $t0
_iopboot_find_entry:
    lw    $t4, 0($t3)
    lw    $t5, 4($t3)
    lw    $t6, 12($t3)                   // size
    or    $t7, $t4, $t5
    or    $t7, $t7, $t6
    beq   $t7, $zero, _iopboot_find_end  // ARC-2's terminator
    nop
    // Compare the ten name bytes.
    move  $t8, $zero
_iopboot_find_compare:
    addu  $t9, $t3, $t8
    lbu   $t4, 0($t9)
    addu  $t9, $a2, $t8
    lbu   $t5, 0($t9)
    bne   $t4, $t5, _iopboot_find_step
    nop
    addiu $t8, $t8, 1
    slti  $t7, $t8, 10
    bne   $t7, $zero, _iopboot_find_compare
    nop
    move  $v0, $t2
    jr    $ra
    move  $v1, $t6
_iopboot_find_step:
    addiu $t6, $t6, 15
    srl   $t6, $t6, 4
    sll   $t6, $t6, 4
    addu  $t2, $t2, $t6
    b     _iopboot_find_entry
    addiu $t3, $t3, 16
_iopboot_find_end:
    jr    $ra
    move  $v0, $zero

    .align 2
_name_iopbtconf:
    .ascii "IOPBTCONF"
    .byte 0
