// The kernel's dispatch tables, at the addresses the interface fixes.
//
// docs/spec/04-ee-kernel.md EE-6f: the kernel publishes its tables at known
// addresses, and EE-6e and SYS-5b require them to be *writable* -- syscalls
// install into all three at run time. They are data in RAM, not constants.

    .set noreorder

    .globl _syscall_table
    .globl _exception_table
    .globl _interrupt_table

// EE-8a: 125 slots, numbered 0x00 to 0x7C, indexed by the absolute syscall
// number. EE-8b: no slot is null -- every number in range resolves to code,
// which for an unimplemented one means the reporter of EE-8d.
    .section .syscalls, "aw"
_syscall_table:
    .rept 0x0D                          // 0x00 .. 0x0C
    .word _sys_undefined
    .endr
    .word _sys_set_exception_handler    // 0x0D
    .word _sys_set_common_handler       // 0x0E
    .word _sys_set_interrupt_handler    // 0x0F
    .rept 4                             // 0x10 .. 0x13
    .word _sys_undefined
    .endr
    .word _sys_enable_intc              // 0x14
    .word _sys_disable_intc             // 0x15
    .word _sys_enable_dmac              // 0x16
    .word _sys_disable_dmac             // 0x17
    .word _sys_undefined                // 0x18
    .word _sys_undefined                // 0x19
    // EE-8c: the block 0x14..0x19 is published again at 0x1A..0x1F. Collapsing
    // it would break callers that use the higher numbers.
    .word _sys_enable_intc              // 0x1A
    .word _sys_disable_intc             // 0x1B
    .word _sys_enable_dmac              // 0x1C
    .word _sys_disable_dmac             // 0x1D
    .word _sys_undefined                // 0x1E
    .word _sys_undefined                // 0x1F
    .rept 0x54                          // 0x20 .. 0x73
    .word _sys_undefined
    .endr
    .word _sys_set_syscall              // 0x74
    .word _sys_nothing                  // 0x75
    .rept 7                             // 0x76 .. 0x7C
    .word _sys_undefined
    .endr

// EE-6a: fourteen entries, for ExcCode 0 to 13, indexed by `Cause & 0x7C`
// without a shift. EE-6b: only ExcCode 8 has its own handler.
    .section .exctable, "aw"
_exception_table:
    .word _vector_common_stop           // 0  Int
    .word _vector_common_stop           // 1  Mod
    .word _vector_common_stop           // 2  TLBL
    .word _vector_common_stop           // 3  TLBS
    .word _vector_common_stop           // 4  AdEL
    .word _vector_common_stop           // 5  AdES
    .word _vector_common_stop           // 6  IBE
    .word _vector_common_stop           // 7  DBE
    .word _syscall_entry                // 8  Sys
    .word _vector_common_stop           // 9  Bp
    .word _vector_common_stop           // 10 RI
    .word _vector_common_stop           // 11 CpU
    .word _vector_common_stop           // 12 Ov
    .word _vector_common_stop           // 13 Tr
// The doubleword the dispatcher parks $t9 in while it reads `Cause`. It sits
// here because the two tables are adjacent and this is the gap between them.
    .space 8

// EE-6d: eight entries, read by the interrupt vector and indexed by interrupt
// number. None is installed yet; the vector returns when it finds a zero.
    .section .inttable, "aw"
_interrupt_table:
    .space 32
