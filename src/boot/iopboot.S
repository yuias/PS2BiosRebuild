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

// What the boot leaves for anything that follows it. These live *above* the
// modules, not below: a module's bss is zeroed by the loader as it goes, and
// the boot list sat inside that range until an early module grew enough to
// wipe it. The load base only tells you where the first module starts.
//
//   +0x000  how many names resolved
//   +0x004  the base load address the `@` token set
//   +0x008  the archive's table, so a module need not carry its own scan
//   +0x00C  where the next module would go
//   +0x010  (address, size) pairs in the archive, in load order
//   +0x400  where each was loaded, once it has been
//
// Nothing outside this file reads it yet -- the loader that will is the next
// piece of work -- so the layout is ours to choose, unlike the boot-parameter
// table of BOOT-8 whose address is ABI.
    .set BOOT_LIST,       0x001F8100
    .set BOOT_LIST_MAX,   64
    .set BL_COUNT,        0x000
    .set BL_BASE,         0x004
    .set BL_TABLE,        0x008
    .set BL_NEXT,         0x00C
    .set BL_ENTRIES,      0x010
    .set BL_LOADED,       0x400
// Where a name is assembled before it is looked up. It has to be RAM: the
// only writable memory this early, since the code itself is running from ROM.
    .set NAME_SCRATCH,    0x001F8000
// The head of the library registry (IRX-10). Every registered export table's
// first word holds the previous head, which is what IRX-4c means by the magic
// being consumed at registration.
    .set REGISTRY_HEAD,   0x001F8010

    .set EXPORT_MAGIC,    0x41C00000
    .set IMPORT_MAGIC,    0x41E00000
    .set TABLE_HEADER,    0x14

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
    sw    $v0, BL_ENTRIES($t1)           // the module's ROM address
    sw    $v1, BL_ENTRIES+4($t1)         // and its size
    addiu $s3, $s3, 1
    b     _iopboot_token
    nop

_iopboot_done:
    sw    $s3, BL_COUNT($s4)             // how many names resolved
    sw    $s5, BL_BASE($s4)              // and the base they load at

    // Leave the archive's table where a module can find it. `ROMDIR` is the
    // entry that describes the table, so resolving that name with the scan we
    // already have yields the table's own address -- and a module then needs
    // no further copy of the scan to read the archive.
    lui   $a0, (ROM_SEARCH_START >> 16) & 0xFFFF
    ori   $a0, $a0, ROM_SEARCH_START & 0xFFFF
    lui   $a1, (ROM_SEARCH_END >> 16) & 0xFFFF
    ori   $a1, $a1, ROM_SEARCH_END & 0xFFFF
    addiu $a2, $s0, _name_romdir - _iopboot_anchor
    bal   _iopboot_find
    nop
    sw    $v0, BL_TABLE($s4)

// Load each module in turn, in the order the list gave them (BOOT-9c). The
// base advances past each one, so a module's address depends on every module
// before it -- which is why the order is ABI and not a preference.
    move  $s6, $zero                     // the index
    move  $s7, $s5                       // the running load base
_iopboot_load_next:
    slt   $t0, $s6, $s3
    beq   $t0, $zero, _iopboot_loaded
    nop
    sll   $t1, $s6, 3
    addu  $t1, $s4, $t1
    lw    $a0, BL_ENTRIES($t1)           // this module's ROM address
    sll   $t2, $s6, 2
    addu  $t2, $s4, $t2
    sw    $s7, BL_LOADED($t2)            // where this one is going
    sw    $s7, BL_NEXT($s4)              // and, so far, where the next goes
    bal   _iopboot_load
    move  $a1, $s7                       // and where to put it
    beq   $v0, $zero, _iopboot_stop      // a module that will not load is fatal
    nop
    move  $s7, $v0                       // the next module goes after it
    addiu $s6, $s6, 1
    b     _iopboot_load_next
    nop

_iopboot_loaded:
    sw    $s7, BL_NEXT($s4)              // where the next module would go
_iopboot_stop:
    b     _iopboot_stop
    nop

// _iopboot_load(rom_address, base) -> next base, or 0
//
// docs/spec/02-module-abi.md IRX-1, IRX-3 and IRX-12: copy the single load
// segment to `base`, zero the bss behind it, apply the fixups, and call the
// entry. Registers $s0-$s7 belong to the caller, so this works in $t* and
// keeps its own state on the stack.
    .set L_ROM,    0x00                  // the module in ROM
    .set L_BASE,   0x04                  // where it is being put
    .set L_ENTRY,  0x08
    .set L_GP,     0x0C
    .set L_END,    0x10                  // one past its bss
    .set L_RECORD, 0x14                  // the module record of IRX-12c
    .set L_FRAME,  0x40

_iopboot_load:
    addiu $sp, $sp, -L_FRAME
    sw    $ra, L_FRAME-4($sp)
    sw    $a0, L_ROM($sp)
    sw    $a1, L_BASE($sp)

    // IRX-1: an ELF32 for MIPS with e_type 0xFF80.
    lw    $t0, 16($a0)
    andi  $t1, $t0, 0xFFFF
    lui   $t2, 0
    ori   $t2, $t2, 0xFF80
    bne   $t1, $t2, _iopboot_load_fail
    nop

    // Walk the program headers for the two segments of IRX-1.
    lw    $t3, 28($a0)                   // e_phoff
    addu  $t3, $a0, $t3
    lhu   $t4, 42($a0)                   // e_phentsize
    lhu   $t5, 44($a0)                   // e_phnum
    move  $t6, $zero                     // p_offset of PT_LOAD
    move  $t7, $zero                     // p_filesz
    move  $t8, $zero                     // p_memsz
    move  $t9, $zero                     // the .iopmod offset
_iopboot_load_phdr:
    beq   $t5, $zero, _iopboot_load_have_phdrs
    nop
    lw    $t0, 0($t3)                    // p_type
    addiu $t1, $zero, 1                  // PT_LOAD
    bne   $t0, $t1, _iopboot_load_phdr_iopmod
    nop
    lw    $t6, 4($t3)
    lw    $t7, 16($t3)
    lw    $t8, 20($t3)
    b     _iopboot_load_phdr_step
    nop
_iopboot_load_phdr_iopmod:
    lui   $t1, 0x7000
    ori   $t1, $t1, 0x0080               // PT_IOPMOD
    bne   $t0, $t1, _iopboot_load_phdr_step
    nop
    lw    $t9, 4($t3)
_iopboot_load_phdr_step:
    addu  $t3, $t3, $t4
    addiu $t5, $t5, -1
    b     _iopboot_load_phdr
    nop

_iopboot_load_have_phdrs:
    beq   $t7, $zero, _iopboot_load_fail // no load segment
    nop
    beq   $t9, $zero, _iopboot_load_fail // no .iopmod
    nop

    // IRX-2: the entry point and $gp the module wants installed.
    lw    $t0, L_ROM($sp)
    addu  $t9, $t0, $t9
    lw    $t1, 4($t9)                    // entry
    lw    $t2, 8($t9)                    // gp
    lw    $t3, L_BASE($sp)
    addu  $t1, $t1, $t3
    beq   $t2, $zero, _iopboot_load_gp_set
    nop
    addu  $t2, $t2, $t3
_iopboot_load_gp_set:
    sw    $t1, L_ENTRY($sp)
    sw    $t2, L_GP($sp)

    // Copy the load segment, then zero the bss behind it (IRX-1).
    lw    $t0, L_ROM($sp)
    addu  $t0, $t0, $t6                  // source
    move  $t1, $t3                       // destination
    move  $t2, $zero
_iopboot_load_copy:
    beq   $t2, $t7, _iopboot_load_zero
    nop
    lw    $t4, 0($t0)
    sw    $t4, 0($t1)
    addiu $t0, $t0, 4
    addiu $t1, $t1, 4
    b     _iopboot_load_copy
    addiu $t2, $t2, 4
_iopboot_load_zero:
    beq   $t2, $t8, _iopboot_load_relocate
    nop
    sw    $zero, 0($t1)
    addiu $t1, $t1, 4
    b     _iopboot_load_zero
    addiu $t2, $t2, 4

_iopboot_load_relocate:
    addu  $t0, $t3, $t8
    sw    $t0, L_END($sp)
    bal   _iopboot_relocate
    nop

    // IRX-12c: the record's +0x10 and +0x14 are the entry and $gp. It is built
    // just past the module, which is also where the next one starts.
    lw    $t0, L_END($sp)
    addiu $t0, $t0, 15
    srl   $t0, $t0, 4
    sll   $t0, $t0, 4
    sw    $t0, L_RECORD($sp)
    lw    $t1, L_ENTRY($sp)
    sw    $t1, 0x10($t0)
    lw    $t2, L_GP($sp)
    sw    $t2, 0x14($t0)
    lw    $t3, L_BASE($sp)
    sw    $t3, 0x00($t0)                 // where it was put
    lw    $t4, L_END($sp)
    subu  $t4, $t4, $t3
    sw    $t4, 0x04($t0)                 // and how much of memory it holds

    // IRX-9 then IRX-10: bind what this module imports, so its entry can call
    // it, and register what it exports, so the modules after it can bind to
    // this one. The reference has a module register itself by calling
    // `loadcore` ordinal 6 from its entry; doing it in the loader instead
    // means the first module needs no loader to already exist.
    lw    $a0, L_BASE($sp)
    lw    $a1, L_END($sp)
    bal   _iopboot_bind
    nop
    lw    $a0, L_BASE($sp)
    lw    $a1, L_END($sp)
    bal   _iopboot_register
    nop

    // IRX-12: entry(argc, argv, 0, record), with the module's own $gp
    // installed. `return & 3` decides residency; with no allocator there is
    // nothing to give back yet, so a module asking to go simply is not there.
    move  $a0, $zero
    move  $a1, $zero
    move  $a2, $zero
    move  $a3, $t0
    lw    $gp, L_GP($sp)
    lw    $t1, L_ENTRY($sp)
    jalr  $t1
    nop
    move  $gp, $zero

    lw    $t0, L_RECORD($sp)
    addiu $v0, $t0, 0x20                 // past the record: the next base
    lw    $ra, L_FRAME-4($sp)
    b     _iopboot_load_done
    addiu $sp, $sp, L_FRAME

_iopboot_load_fail:
    lw    $ra, L_FRAME-4($sp)
    move  $v0, $zero
    addiu $sp, $sp, L_FRAME
_iopboot_load_done:
    jr    $ra
    nop

// _iopboot_relocate: apply the REL entries of IRX-3 to the segment just
// copied. The frame belongs to _iopboot_load, whose L_ROM and L_BASE this
// reads; $ra is saved because the caller has already stored its own.
_iopboot_relocate:
    addiu $sp, $sp, -16
    sw    $ra, 0($sp)
    lw    $t0, L_ROM+16($sp)
    lw    $t1, 32($t0)                   // e_shoff
    addu  $t1, $t0, $t1
    lhu   $t2, 46($t0)                   // e_shentsize
    lhu   $t3, 48($t0)                   // e_shnum
_iopboot_relocate_section:
    beq   $t3, $zero, _iopboot_relocate_done
    nop
    lw    $t4, 4($t1)                    // sh_type
    addiu $t5, $zero, 9                  // SHT_REL
    bne   $t4, $t5, _iopboot_relocate_next_section
    nop
    lw    $t6, 16($t1)                   // sh_offset
    lw    $t7, 20($t1)                   // sh_size
    addu  $t6, $t0, $t6
    addu  $t7, $t6, $t7                  // one past the last entry
    move  $t8, $zero                     // the held HI16, or 0
    move  $t9, $zero                     // its address
_iopboot_relocate_entry:
    sltu  $at, $t6, $t7
    beq   $at, $zero, _iopboot_relocate_next_section
    nop
    bal   _iopboot_apply
    nop
    b     _iopboot_relocate_entry
    addiu $t6, $t6, 8
_iopboot_relocate_next_section:
    addu  $t1, $t1, $t2
    addiu $t3, $t3, -1
    b     _iopboot_relocate_section
    nop
_iopboot_relocate_done:
    lw    $ra, 0($sp)
    addiu $sp, $sp, 16
    jr    $ra
    nop

// _iopboot_apply: one REL entry at $t6, with $t8/$t9 carrying a held HI16.
_iopboot_apply:
    lw    $at, 0($t6)                    // r_offset, a segment-relative vaddr
    lw    $v0, 4($t6)                    // r_info
    andi  $v0, $v0, 0xFF
    lw    $v1, L_BASE+16($sp)
    addu  $at, $at, $v1                  // where the word now lives

    addiu $a0, $zero, 2                  // R_MIPS_32
    bne   $v0, $a0, _iopboot_apply_26
    nop
    lw    $a1, 0($at)
    addu  $a1, $a1, $v1
    jr    $ra
    sw    $a1, 0($at)

_iopboot_apply_26:
    addiu $a0, $zero, 4                  // R_MIPS_26
    bne   $v0, $a0, _iopboot_apply_hi
    nop
    lw    $a1, 0($at)
    sll   $a2, $a1, 6                    // the target field, shifted out
    srl   $a2, $a2, 4                    // .. and back as a byte address
    addu  $a2, $a2, $v1
    srl   $a2, $a2, 2
    sll   $a2, $a2, 6
    srl   $a2, $a2, 6                    // keep 26 bits
    lui   $a3, 0xFC00
    and   $a1, $a1, $a3                  // keep the opcode
    or    $a1, $a1, $a2
    jr    $ra
    sw    $a1, 0($at)

_iopboot_apply_hi:
    addiu $a0, $zero, 5                  // R_MIPS_HI16
    bne   $v0, $a0, _iopboot_apply_lo
    nop
    lw    $t8, 0($at)                    // IRX-3a: hold it for its LO16
    jr    $ra
    move  $t9, $at

_iopboot_apply_lo:
    addiu $a0, $zero, 6                  // R_MIPS_LO16
    bne   $v0, $a0, _iopboot_apply_done
    nop
    // A LO16 with no HI16 held is malformed (IRX-3a) and is left alone: its
    // high half is nowhere to be found, and address zero is not it.
    beq   $t9, $zero, _iopboot_apply_done
    nop
    lw    $a1, 0($at)
    sll   $a2, $a1, 16
    sra   $a2, $a2, 16                   // the low half, sign-extended
    sll   $a3, $t8, 16                   // the held high half
    addu  $a3, $a3, $a2
    addu  $a3, $a3, $v1                  // the address, rebased
    andi  $a2, $a3, 0xFFFF
    lui   $v0, 0xFFFF
    and   $a1, $a1, $v0
    or    $a1, $a1, $a2
    sw    $a1, 0($at)
    // The carry out of the low half belongs to the high one. The held HI16 is
    // kept, not dropped, so a compiler's `lui` shared by several LO16s of the
    // same address (IRX-3a) has its high half recomputed for each -- to the
    // same value, since $t8 still holds what the file stored.
    srl   $a2, $a3, 16
    andi  $v0, $a3, 0x8000
    beq   $v0, $zero, _iopboot_apply_lo_store
    nop
    addiu $a2, $a2, 1
_iopboot_apply_lo_store:
    lw    $a1, 0($t9)
    lui   $v0, 0xFFFF
    and   $a1, $a1, $v0
    andi  $a2, $a2, 0xFFFF
    or    $a1, $a1, $a2
    sw    $a1, 0($t9)
_iopboot_apply_done:
    jr    $ra
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
_name_romdir:
    .ascii "ROMDIR"
    .byte 0, 0, 0, 0


// The name above is ten bytes, so what follows it needs realigning before it
// can be code again.
    .align 2

// _iopboot_register(start, end): link every export table in the segment into
// the registry, per IRX-10a. The comparison is on tag and major version --
// a library's identity -- with a strictly greater minor superseding.
_iopboot_register:
    addiu $sp, $sp, -32
    sw    $ra, 0($sp)
    move  $t0, $a0
_iopboot_register_scan:
    sltu  $at, $t0, $a1
    beq   $at, $zero, _iopboot_register_done
    nop
    lw    $t1, 0($t0)
    lui   $t2, (EXPORT_MAGIC >> 16) & 0xFFFF
    bne   $t1, $t2, _iopboot_register_step
    nop

    // IRX-10a: walk the registry for the same tag and major version.
    lui   $t3, (REGISTRY_HEAD >> 16) & 0xFFFF
    ori   $t3, $t3, REGISTRY_HEAD & 0xFFFF
    lw    $t4, 0($t3)                     // the current head
_iopboot_register_walk:
    beq   $t4, $zero, _iopboot_register_link
    nop
    lw    $t5, 12($t0)                    // our tag, first word
    lw    $t6, 12($t4)
    bne   $t5, $t6, _iopboot_register_walk_next
    nop
    lw    $t5, 16($t0)                    // and its second
    lw    $t6, 16($t4)
    bne   $t5, $t6, _iopboot_register_walk_next
    nop
    lhu   $t5, 8($t0)                     // versions
    lhu   $t6, 8($t4)
    srl   $t7, $t5, 8
    srl   $t8, $t6, 8
    bne   $t7, $t8, _iopboot_register_walk_next   // a different library
    nop
    andi  $t7, $t5, 0xFF
    andi  $t8, $t6, 0xFF
    sltu  $at, $t8, $t7                   // strictly greater supersedes
    beq   $at, $zero, _iopboot_register_step
    nop
    b     _iopboot_register_link
    nop
_iopboot_register_walk_next:
    lw    $t4, 0($t4)
    b     _iopboot_register_walk
    nop

_iopboot_register_link:
    lui   $t3, (REGISTRY_HEAD >> 16) & 0xFFFF
    ori   $t3, $t3, REGISTRY_HEAD & 0xFFFF
    lw    $t4, 0($t3)
    sw    $t4, 0($t0)                     // IRX-4c: the magic becomes the link
    sw    $t0, 0($t3)
_iopboot_register_step:
    addiu $t0, $t0, 4
    b     _iopboot_register_scan
    nop
_iopboot_register_done:
    lw    $ra, 0($sp)
    addiu $sp, $sp, 32
    jr    $ra
    nop

// _iopboot_bind(start, end): bind every import table in the segment, per
// IRX-9. An unresolved import is left as it is: IRX-8a makes an unbound stub
// return harmlessly rather than fail the boot.
_iopboot_bind:
    addiu $sp, $sp, -32
    sw    $ra, 0($sp)
    move  $t0, $a0
_iopboot_bind_scan:
    sltu  $at, $t0, $a1
    beq   $at, $zero, _iopboot_bind_done
    nop
    lw    $t1, 0($t0)
    lui   $t2, (IMPORT_MAGIC >> 16) & 0xFFFF
    bne   $t1, $t2, _iopboot_bind_step
    nop
    lhu   $t1, 10($t0)                    // flags
    andi  $t1, $t1, 7
    bne   $t1, $zero, _iopboot_bind_step  // IRX-9b: these are not ours to bind
    nop

    // Find the exporter: the same tag, at the registry head-most entry.
    lui   $t3, (REGISTRY_HEAD >> 16) & 0xFFFF
    ori   $t3, $t3, REGISTRY_HEAD & 0xFFFF
    lw    $t4, 0($t3)
_iopboot_bind_find:
    beq   $t4, $zero, _iopboot_bind_step  // nothing exports it yet
    nop
    lw    $t5, 12($t0)
    lw    $t6, 12($t4)
    bne   $t5, $t6, _iopboot_bind_find_next
    nop
    lw    $t5, 16($t0)
    lw    $t6, 16($t4)
    beq   $t5, $t6, _iopboot_bind_table
    nop
_iopboot_bind_find_next:
    lw    $t4, 0($t4)
    b     _iopboot_bind_find
    nop

_iopboot_bind_table:
    // IRX-9 step 1: count the exporter's entries to the zero terminator.
    addiu $t5, $t4, TABLE_HEADER
    move  $t6, $zero
_iopboot_bind_count:
    sll   $at, $t6, 2
    addu  $at, $t5, $at
    lw    $at, 0($at)
    beq   $at, $zero, _iopboot_bind_stubs
    nop
    addiu $t6, $t6, 1
    b     _iopboot_bind_count
    nop

_iopboot_bind_stubs:
    addiu $t7, $t0, TABLE_HEADER
_iopboot_bind_stub:
    lw    $at, 0($t7)
    beq   $at, $zero, _iopboot_bind_step  // the terminator
    nop
    lw    $t8, 4($t7)                     // IRX-9 step 2: the second word
    srl   $t9, $t8, 26
    addiu $at, $zero, 9                   // addiu
    bne   $t9, $at, _iopboot_bind_stub_next
    nop
    andi  $t9, $t8, 0xFFFF                // the ordinal
    sltu  $at, $t9, $t6
    beq   $at, $zero, _iopboot_bind_over  // IRX-9 step 4: out of range
    nop
    sll   $at, $t9, 2
    addu  $at, $t5, $at
    lw    $at, 0($at)                     // export[ordinal]
    srl   $at, $at, 2
    lui   $t9, 0x03FF
    ori   $t9, $t9, 0xFFFF
    and   $at, $at, $t9
    lui   $t9, 0x0800                     // IRX-9 step 3: a `j` to it
    or    $at, $at, $t9
    b     _iopboot_bind_stub_write
    nop
_iopboot_bind_over:
    lui   $at, 0x03E0
    ori   $at, $at, 0x0008                // `jr $ra`: degrade to a return
_iopboot_bind_stub_write:
    sw    $at, 0($t7)                     // IRX-9a: only the first word
_iopboot_bind_stub_next:
    addiu $t7, $t7, 8
    b     _iopboot_bind_stub
    nop

_iopboot_bind_step:
    addiu $t0, $t0, 4
    b     _iopboot_bind_scan
    nop
_iopboot_bind_done:
    lw    $ra, 0($sp)
    addiu $sp, $sp, 32
    jr    $ra
    nop
