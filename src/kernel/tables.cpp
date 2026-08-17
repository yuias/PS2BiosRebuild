// The kernel's dispatch tables, at the addresses the interface fixes.
//
// docs/spec/04-ee-kernel.md EE-6f: the kernel publishes its tables at known
// addresses, and EE-6e and SYS-5b require them to be *writable* -- syscalls
// install into all three at run time. They are data in RAM, not constants, so
// none of the arrays below is `const`.
//
// The handlers still live in assembly, either because they are the vector page
// (EE-8f) or because they save 128 bits of context (EE-7e). Their symbols keep
// the names those files give them; the `asm` labels here are what let this file
// name them the way the rest of the C++ does.

#include <stdint.h>

extern "C" {

using Handler = void (*)();

// syscall.S
void syscallEntry() asm("_syscall_entry");
void sysUndefined() asm("_sys_undefined");

// vectors.S
void vectorCommonStop() asm("_vector_common_stop");
void sysSetExceptionHandler() asm("_sys_set_exception_handler");
void sysSetCommonHandler() asm("_sys_set_common_handler");
void sysSetInterruptHandler() asm("_sys_set_interrupt_handler");
void sysEnableIntc() asm("_sys_enable_intc");
void sysDisableIntc() asm("_sys_disable_intc");
void sysEnableDmac() asm("_sys_enable_dmac");
void sysDisableDmac() asm("_sys_disable_dmac");
void sysSetSyscall() asm("_sys_set_syscall");
void sysNothing() asm("_sys_nothing");

// program.cpp
void sysLoadProgram() asm("_sys_load_program");
void sysLoadOsd() asm("_sys_load_osd");

// cache.S
void sysReadCop0() asm("_sys_read_cop0");
void cop0Read0() asm("_cop0_read_0");
void cop0Read1() asm("_cop0_read_1");
void cop0Read2() asm("_cop0_read_2");
void cop0Read3() asm("_cop0_read_3");
void cop0Read4() asm("_cop0_read_4");
void cop0Read5() asm("_cop0_read_5");
void cop0Read6() asm("_cop0_read_6");
void cop0ReadNone() asm("_cop0_read_none");

// EE-8e: the cache trio is published through KSEG1, so that each runs uncached
// -- every one of them reconfigures or sweeps the cache its own fetches would
// otherwise come through. A rebuild that normalised these three to KSEG0 would
// fail intermittently rather than cleanly.
//
// The aliases are computed by the link script: adding `0x20000000` to a
// function's address is a link-time constant there, where in C++ it would need
// a cast the language will not perform before `main` -- and this image runs no
// static initialisers.
void sysSetCacheModeUncached() asm("_sys_set_cache_mode_kseg1");
void sysEnableCacheUncached() asm("_sys_enable_cache_kseg1");
void sysDisableCacheUncached() asm("_sys_disable_cache_kseg1");

// EE-8a: 125 slots, numbered 0x00 to 0x7C, indexed by the absolute syscall
// number. EE-8b: no slot is null -- every number in range resolves to code,
// which for an unimplemented one means the reporter of EE-8d.
//
// EE-8c: two blocks are published twice, at 0x14..0x19 -> 0x1A..0x1F and
// 0x63..0x66 -> 0x67..0x6A. Collapsing either would break callers that use the
// higher numbers, so the repetition is deliberate.
[[gnu::section(".syscalls"), gnu::used]]
Handler syscall_table[125] = {
    // 0x00
    sysUndefined, sysUndefined, sysUndefined, sysUndefined,
    sysUndefined, sysUndefined,
    sysLoadProgram,                                        // 0x06  EE-9a
    sysUndefined,
    // 0x08
    sysUndefined, sysUndefined, sysUndefined, sysUndefined,
    sysUndefined,
    sysSetExceptionHandler,                                // 0x0D
    sysSetCommonHandler,                                   // 0x0E
    sysSetInterruptHandler,                                // 0x0F
    // 0x10
    sysUndefined, sysUndefined, sysUndefined, sysUndefined,
    sysEnableIntc,                                         // 0x14
    sysDisableIntc,                                        // 0x15
    sysEnableDmac,                                         // 0x16
    sysDisableDmac,                                        // 0x17
    // 0x18
    sysUndefined, sysUndefined,
    sysEnableIntc,                                         // 0x1A  = 0x14
    sysDisableIntc,                                        // 0x1B  = 0x15
    sysEnableDmac,                                         // 0x1C  = 0x16
    sysDisableDmac,                                        // 0x1D  = 0x17
    sysUndefined, sysUndefined,
    // 0x20 .. 0x5F
    sysUndefined, sysUndefined, sysUndefined, sysUndefined,
    sysUndefined, sysUndefined, sysUndefined, sysUndefined,
    sysUndefined, sysUndefined, sysUndefined, sysUndefined,
    sysUndefined, sysUndefined, sysUndefined, sysUndefined,
    sysUndefined, sysUndefined, sysUndefined, sysUndefined,
    sysUndefined, sysUndefined, sysUndefined, sysUndefined,
    sysUndefined, sysUndefined, sysUndefined, sysUndefined,
    sysUndefined, sysUndefined, sysUndefined, sysUndefined,
    sysUndefined, sysUndefined, sysUndefined, sysUndefined,
    sysUndefined, sysUndefined, sysUndefined, sysUndefined,
    sysUndefined, sysUndefined, sysUndefined, sysUndefined,
    sysUndefined, sysUndefined, sysUndefined, sysUndefined,
    sysUndefined, sysUndefined, sysUndefined, sysUndefined,
    sysUndefined, sysUndefined, sysUndefined, sysUndefined,
    sysUndefined, sysUndefined, sysUndefined, sysUndefined,
    sysUndefined, sysUndefined, sysUndefined, sysUndefined,
    // 0x60
    sysSetCacheModeUncached,                               // 0x60  EE-8e
    sysEnableCacheUncached,                                // 0x61  EE-8e
    sysDisableCacheUncached,                               // 0x62  EE-8e
    sysReadCop0,                                           // 0x63  cached
    sysUndefined, sysUndefined, sysUndefined,
    sysReadCop0,                                           // 0x67  = 0x63
    // 0x68
    sysUndefined, sysUndefined, sysUndefined, sysUndefined,
    sysUndefined, sysUndefined, sysUndefined, sysUndefined,
    // 0x70
    sysUndefined, sysUndefined, sysUndefined, sysUndefined,
    sysSetSyscall,                                         // 0x74
    sysNothing,                                            // 0x75
    sysUndefined, sysUndefined,
    // 0x78
    sysUndefined, sysUndefined, sysUndefined,
    sysLoadOsd,                                            // 0x7B  EE-9b
    sysUndefined,                                          // 0x7C
};

// EE-6a: fourteen entries, for ExcCode 0 to 13, indexed by `Cause & 0x7C`
// without a shift. EE-6b: only ExcCode 8 has its own handler.
//
// The doubleword after them is where the dispatcher parks `$t9` while it reads
// `Cause`. It is a member rather than a separate object because the two have to
// stay adjacent, and only one object per section guarantees that.
struct ExceptionTable {
    Handler entries[14];
    uint64_t parked;
};

[[gnu::section(".exctable"), gnu::used]]
ExceptionTable exception_table = {
    {
        vectorCommonStop,          // 0  Int
        vectorCommonStop,          // 1  Mod
        vectorCommonStop,          // 2  TLBL
        vectorCommonStop,          // 3  TLBS
        vectorCommonStop,          // 4  AdEL
        vectorCommonStop,          // 5  AdES
        vectorCommonStop,          // 6  IBE
        vectorCommonStop,          // 7  DBE
        syscallEntry,              // 8  Sys
        vectorCommonStop,          // 9  Bp
        vectorCommonStop,          // 10 RI
        vectorCommonStop,          // 11 CpU
        vectorCommonStop,          // 12 Ov
        vectorCommonStop,          // 13 Tr
    },
    0,
};

// EE-6d: eight entries, read by the interrupt vector and indexed by interrupt
// number. None is installed yet; the vector returns when it finds a zero.
[[gnu::section(".inttable"), gnu::used]]
Handler interrupt_table[8] = {};

// EE-6f: the fourth table the kernel publishes, one stub per CP0 register.
// Slot 0x63 jumps through it because `mfc0` encodes its register number in the
// instruction, so "read register n" cannot be written any other way.
[[gnu::section(".cop0reads"), gnu::used]]
Handler cop0_read_table[8] = {
    cop0Read0, cop0Read1, cop0Read2, cop0Read3,
    cop0Read4, cop0Read5, cop0Read6,
    cop0ReadNone,                  // the R5900 has no register 7
};

}  // extern "C"
