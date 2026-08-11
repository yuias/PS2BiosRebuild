// Loading and running a program from the archive.
//
// docs/spec/04-ee-kernel.md EE-9: slot 0x06 is the program loader, slot 0x7B
// is the same call with the path pinned to `rom0:OSDSYS`, and the default boot
// invokes it with a single argument. `rom0:` is served by the IOP, so the file
// crosses the SIF before any of it is a program (EE-9d).

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

    .set PT_LOAD, 1

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
    jal   _skip_device
    nop
    move  $a0, $v0
    la    $a1, _staging
    jal   _fetch
    nop
    beq   $v0, $zero, _load_failed
    nop

    la    $a0, _staging
    jal   _place_segments
    nop
    beq   $v0, $zero, _load_failed
    nop

    move  $t9, $v0                       // the entry point
    lw    $a0, 8($sp)
    lw    $a1, 12($sp)
    jr    $t9                            // EE-9: the program replaces us
    nop

_load_failed:
    la    $a0, _no_program
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
    la    $a0, _rom0_osdsys
    b     _sys_load_program
    nop

// EE-9c: the default boot runs that program with one argument.
_boot_default:
    addiu $sp, $sp, -32
    la    $t0, _boot_browser
    sw    $t0, 16($sp)
    la    $a0, _rom0_osdsys
    addiu $a1, $zero, 1
    b     _sys_load_program
    addiu $a2, $sp, 16

// _skip_device(path) -> the name after `:`, or the path itself.
_skip_device:
    move  $v0, $a0
    move  $t0, $a0
_skip_device_scan:
    lbu   $t1, 0($t0)
    beq   $t1, $zero, _skip_device_done
    nop
    addiu $t2, $zero, 0x3A               // ':'
    bne   $t1, $t2, _skip_device_next
    nop
    jr    $ra
    addiu $v0, $t0, 1
_skip_device_next:
    b     _skip_device_scan
    addiu $t0, $t0, 1
_skip_device_done:
    jr    $ra
    nop

// _fetch(name, destination) -> size, or 0.
//
// Two exchanges: how big is it, then send it. The transfer itself no longer
// needs the size -- BOOT-11 has the sender frame it -- but the caller does,
// since it has to know how much of the buffer the answer filled.
_fetch:
    addiu $sp, $sp, -48
    sw    $ra, 0($sp)
    sw    $s0, 4($sp)
    sw    $s1, 8($sp)
    move  $s0, $a0                       // the name, across both exchanges
    move  $s1, $a1                       // and where the bytes go

    move  $a1, $zero                     // ask the size first
    la    $a2, _reply
    jal   _sif_exchange
    nop
    la    $t0, _reply
    lw    $v1, 0($t0)
    beq   $v1, $zero, _fetch_done
    move  $v0, $zero

    move  $a0, $s0
    addiu $a1, $zero, 1                  // then ask for the bytes
    move  $a2, $s1
    sw    $v1, 12($sp)
    jal   _sif_exchange
    nop
    lw    $v0, 12($sp)
_fetch_done:
    lw    $ra, 0($sp)
    lw    $s0, 4($sp)
    lw    $s1, 8($sp)
    jr    $ra
    addiu $sp, $sp, 48

// _place_segments(elf) -> entry point, or 0.
//
// An EE program is stored as the ELF it is, so the loader reads the program
// headers rather than being told a layout: each PT_LOAD segment is copied to
// the address it asks for, and the memory beyond what the file holds is
// zeroed, which is where a program's bss comes from.
_place_segments:
    lhu   $t0, 16($a0)                   // e_type
    addiu $t1, $zero, 2                  // ET_EXEC
    bne   $t0, $t1, _place_reject
    nop
    lw    $t2, 28($a0)                   // e_phoff
    addu  $t2, $a0, $t2
    lhu   $t3, 42($a0)                   // e_phentsize
    lhu   $t4, 44($a0)                   // e_phnum
_place_segment:
    beq   $t4, $zero, _place_done
    nop
    lw    $t5, 0($t2)                    // p_type
    addiu $t6, $zero, PT_LOAD
    bne   $t5, $t6, _place_next
    nop
    lw    $t5, 4($t2)                    // p_offset
    addu  $t5, $a0, $t5
    lw    $t6, 8($t2)                    // p_vaddr
    lw    $t7, 16($t2)                   // p_filesz
    lw    $t8, 20($t2)                   // p_memsz
    move  $t9, $zero
_place_copy:
    sltu  $at, $t9, $t7
    beq   $at, $zero, _place_zero
    nop
    addu  $v0, $t5, $t9
    lbu   $v1, 0($v0)
    addu  $v0, $t6, $t9
    sb    $v1, 0($v0)
    b     _place_copy
    addiu $t9, $t9, 1
_place_zero:
    sltu  $at, $t9, $t8
    beq   $at, $zero, _place_next
    nop
    addu  $v0, $t6, $t9
    sb    $zero, 0($v0)
    b     _place_zero
    addiu $t9, $t9, 1
_place_next:
    addu  $t2, $t2, $t3
    addiu $t4, $t4, -1
    b     _place_segment
    nop
_place_done:
    jr    $ra
    lw    $v0, 24($a0)                   // e_entry
_place_reject:
    jr    $ra
    move  $v0, $zero

    .section .rodata, "a"
_rom0_osdsys:
    .asciz "rom0:OSDSYS"
_boot_browser:
    .asciz "BootBrowser"
_no_program:
    .asciz "# no program: the archive has nothing by that name.\n"

    .section .bss, "aw", @nobits
    .align 4
_reply:
    .space 16
_staging:
    .space 0x4000                        // where a program lands before it is one
