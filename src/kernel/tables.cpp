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
void sysBootBrowser() asm("_sys_boot_browser");
void sysLoadProgram() asm("_sys_load_program");
void sysLoadOsd() asm("_sys_load_osd");
void sysExecPs2() asm("_sys_exec_ps2");

// sif.cpp
void sysSifStopDChain() asm("_sys_sif_stop_dchain");
void sysSifDmaStat() asm("_sys_sif_dma_stat");
void sysSifSetDma() asm("_sys_sif_set_dma");
void sysSifSetDChain() asm("_sys_sif_set_dchain");
void sysSifSetReg() asm("_sys_sif_set_reg");
void sysSifGetReg() asm("_sys_sif_get_reg");

// interrupt.cpp, and the entry in syscall.S the interrupt table points at
void sysAddIntcHandler() asm("_sys_add_intc_handler");
void sysRemoveIntcHandler() asm("_sys_remove_intc_handler");
void sysAddDmacHandler() asm("_sys_add_dmac_handler");
void sysRemoveDmacHandler() asm("_sys_remove_dmac_handler");
void interruptEntry() asm("_interrupt_entry");

// thread.cpp
void sysSetupThread() asm("_sys_setup_thread");
void sysSetupHeap() asm("_sys_setup_heap");
void sysEndOfHeap() asm("_sys_end_of_heap");
void sysCreateThread() asm("_sys_create_thread");
void sysDeleteThread() asm("_sys_delete_thread");
void sysStartThread() asm("_sys_start_thread");
void sysExitThread() asm("_sys_exit_thread");
void sysExitDeleteThread() asm("_sys_exit_delete_thread");
void sysTerminateThread() asm("_sys_terminate_thread");
void sysTerminateThreadDirect() asm("_sys_terminate_thread_direct");
void sysChangeThreadPriority() asm("_sys_change_thread_priority");
void sysChangeThreadPriorityDirect() asm("_sys_change_thread_priority_direct");
void sysRotateThreadReadyQueue() asm("_sys_rotate_thread_ready_queue");
void sysRotateThreadReadyQueueDirect() asm("_sys_rotate_thread_ready_queue_direct");
void sysReleaseWaitThread() asm("_sys_release_wait_thread");
void sysReleaseWaitThreadDirect() asm("_sys_release_wait_thread_direct");
void sysGetThreadId() asm("_sys_get_thread_id");
void sysReferThreadStatus() asm("_sys_refer_thread_status");
void sysSleepThread() asm("_sys_sleep_thread");
void sysWakeupThread() asm("_sys_wakeup_thread");
void sysWakeupThreadDirect() asm("_sys_wakeup_thread_direct");
void sysCancelWakeupThread() asm("_sys_cancel_wakeup_thread");
void sysSuspendThread() asm("_sys_suspend_thread");
void sysResumeThread() asm("_sys_resume_thread");
void sysResumeThreadDirect() asm("_sys_resume_thread_direct");
void sysCreateSema() asm("_sys_create_sema");
void sysDeleteSema() asm("_sys_delete_sema");
void sysDeleteSemaDirect() asm("_sys_delete_sema_direct");
void sysSignalSema() asm("_sys_signal_sema");
void sysSignalSemaDirect() asm("_sys_signal_sema_direct");
void sysWaitSema() asm("_sys_wait_sema");
void sysPollSema() asm("_sys_poll_sema");
void sysReferSemaStatus() asm("_sys_refer_sema_status");

// gs.cpp and gs.S
void sysSetGsCrt() asm("_sys_set_gs_crt");
void sysSetVSyncFlag() asm("_sys_set_vsync_flag");
void sysGetGsImr() asm("_sys_get_gs_imr");
void sysSetGsImr() asm("_sys_set_gs_imr");

// osdconfig.cpp
void sysSetOsdConfigParam() asm("_sys_set_osd_config_param");
void sysGetOsdConfigParam() asm("_sys_get_osd_config_param");
void sysGetOsdConfigParam2() asm("_sys_get_osd_config_param2");

// cache.S
void sysFlushCache() asm("_sys_flush_cache");
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
// which for an unimplemented one means the reporter of EE-8d. SYS-11: the
// reference indexes and writes the table without a bound, and the SDK's
// runtime installs its own handlers at 0x7F and 0x82 that way, so the table
// runs to 0xFF -- the reporter from 0x7D on until something is installed.
//
// EE-8c: two blocks are published twice, at 0x14..0x19 -> 0x1A..0x1F and
// 0x63..0x66 -> 0x67..0x6A. Collapsing either would break callers that use the
// higher numbers, so the repetition is deliberate.
[[gnu::section(".syscalls"), gnu::used]]
Handler syscall_table[256] = {
    // 0x00
    sysUndefined,
    sysUndefined,
    sysSetGsCrt,                                           // 0x02  SYS-14a
    sysUndefined,
    sysBootBrowser,                                        // 0x04  SYS-6a
    sysUndefined,
    sysLoadProgram,                                        // 0x06  EE-9a
    sysExecPs2,                                            // 0x07  SYS-8d
    // 0x08
    sysUndefined, sysUndefined, sysUndefined, sysUndefined,
    sysUndefined,
    sysSetExceptionHandler,                                // 0x0D
    sysSetCommonHandler,                                   // 0x0E
    sysSetInterruptHandler,                                // 0x0F
    // 0x10  SYS-12a
    sysAddIntcHandler, sysRemoveIntcHandler,
    sysAddDmacHandler, sysRemoveDmacHandler,
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
    sysCreateThread, sysDeleteThread, sysStartThread, sysExitThread,   // 0x20  SYS-10
    sysExitDeleteThread, sysTerminateThread, sysTerminateThreadDirect,
    sysUndefined,                                          // 0x24..0x27
    sysUndefined, sysChangeThreadPriority, sysChangeThreadPriorityDirect,
    sysRotateThreadReadyQueue,                             // 0x28..0x2B
    sysRotateThreadReadyQueueDirect, sysReleaseWaitThread,
    sysReleaseWaitThreadDirect, sysGetThreadId,           // 0x2C..0x2F
    sysReferThreadStatus, sysReferThreadStatus, sysSleepThread,
    sysWakeupThread,                                       // 0x30..0x33
    sysWakeupThreadDirect, sysCancelWakeupThread, sysCancelWakeupThread,
    sysSuspendThread,                                      // 0x34..0x37
    sysSuspendThread, sysResumeThread, sysResumeThreadDirect,
    sysUndefined,                                          // 0x38..0x3B
    sysSetupThread, sysSetupHeap, sysEndOfHeap,           // 0x3C..0x3E  SYS-8
    sysUndefined,                                          // 0x3F  retired
    sysCreateSema, sysDeleteSema, sysSignalSema, sysSignalSemaDirect,   // 0x40  SYS-9
    sysWaitSema, sysPollSema, sysPollSema, sysReferSemaStatus,   // 0x44..0x47
    sysReferSemaStatus, sysDeleteSemaDirect,              // 0x48..0x49
    sysSetOsdConfigParam, sysGetOsdConfigParam,           // 0x4A..0x4B  SYS-15a
    sysUndefined, sysUndefined, sysUndefined, sysUndefined,   // 0x4c
    sysUndefined, sysUndefined, sysUndefined, sysUndefined,   // 0x50
    sysUndefined, sysUndefined, sysUndefined, sysUndefined,   // 0x54
    sysUndefined, sysUndefined, sysUndefined, sysUndefined,   // 0x58
    sysUndefined, sysUndefined, sysUndefined, sysUndefined,   // 0x5c
    // 0x60
    sysSetCacheModeUncached,                               // 0x60  EE-8e
    sysEnableCacheUncached,                                // 0x61  EE-8e
    sysDisableCacheUncached,                               // 0x62  EE-8e
    sysReadCop0,                                           // 0x63  cached
    sysFlushCache,                                         // 0x64  SYS-16
    sysUndefined, sysUndefined,
    sysReadCop0,                                           // 0x67  = 0x63
    // 0x68
    sysFlushCache,                                         // 0x68  = 0x64
    sysUndefined, sysUndefined,
    sysSifStopDChain,                                      // 0x6B  SYS-13b
    sysUndefined, sysUndefined, sysUndefined,
    sysGetOsdConfigParam2,                                 // 0x6F  SYS-15c
    // 0x70
    sysGetGsImr,                                           // 0x70  SYS-7c
    sysSetGsImr,                                           // 0x71  SYS-7c
    sysUndefined,
    sysSetVSyncFlag,                                       // 0x73  SYS-14b
    sysSetSyscall,                                         // 0x74
    sysNothing,                                            // 0x75
    sysSifDmaStat,                                         // 0x76  SYS-13d
    sysSifSetDma,                                          // 0x77  SYS-13c
    // 0x78  SYS-13b, SYS-13a
    sysSifSetDChain, sysSifSetReg, sysSifGetReg,
    sysLoadOsd,                                            // 0x7B  EE-9b
    sysUndefined,                                          // 0x7C
    // 0x7D .. 0xFF: reachable (SYS-11), nothing installed
    sysUndefined, sysUndefined, sysUndefined,              // 0x7D..0x7F
    sysUndefined, sysUndefined, sysUndefined, sysUndefined,   // 0x80
    sysUndefined, sysUndefined, sysUndefined, sysUndefined,   // 0x84
    sysUndefined, sysUndefined, sysUndefined, sysUndefined,   // 0x88
    sysUndefined, sysUndefined, sysUndefined, sysUndefined,   // 0x8c
    sysUndefined, sysUndefined, sysUndefined, sysUndefined,   // 0x90
    sysUndefined, sysUndefined, sysUndefined, sysUndefined,   // 0x94
    sysUndefined, sysUndefined, sysUndefined, sysUndefined,   // 0x98
    sysUndefined, sysUndefined, sysUndefined, sysUndefined,   // 0x9c
    sysUndefined, sysUndefined, sysUndefined, sysUndefined,   // 0xa0
    sysUndefined, sysUndefined, sysUndefined, sysUndefined,   // 0xa4
    sysUndefined, sysUndefined, sysUndefined, sysUndefined,   // 0xa8
    sysUndefined, sysUndefined, sysUndefined, sysUndefined,   // 0xac
    sysUndefined, sysUndefined, sysUndefined, sysUndefined,   // 0xb0
    sysUndefined, sysUndefined, sysUndefined, sysUndefined,   // 0xb4
    sysUndefined, sysUndefined, sysUndefined, sysUndefined,   // 0xb8
    sysUndefined, sysUndefined, sysUndefined, sysUndefined,   // 0xbc
    sysUndefined, sysUndefined, sysUndefined, sysUndefined,   // 0xc0
    sysUndefined, sysUndefined, sysUndefined, sysUndefined,   // 0xc4
    sysUndefined, sysUndefined, sysUndefined, sysUndefined,   // 0xc8
    sysUndefined, sysUndefined, sysUndefined, sysUndefined,   // 0xcc
    sysUndefined, sysUndefined, sysUndefined, sysUndefined,   // 0xd0
    sysUndefined, sysUndefined, sysUndefined, sysUndefined,   // 0xd4
    sysUndefined, sysUndefined, sysUndefined, sysUndefined,   // 0xd8
    sysUndefined, sysUndefined, sysUndefined, sysUndefined,   // 0xdc
    sysUndefined, sysUndefined, sysUndefined, sysUndefined,   // 0xe0
    sysUndefined, sysUndefined, sysUndefined, sysUndefined,   // 0xe4
    sysUndefined, sysUndefined, sysUndefined, sysUndefined,   // 0xe8
    sysUndefined, sysUndefined, sysUndefined, sysUndefined,   // 0xec
    sysUndefined, sysUndefined, sysUndefined, sysUndefined,   // 0xf0
    sysUndefined, sysUndefined, sysUndefined, sysUndefined,   // 0xf4
    sysUndefined, sysUndefined, sysUndefined, sysUndefined,   // 0xf8
    sysUndefined, sysUndefined, sysUndefined, sysUndefined,   // 0xfc
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
// number. IP2 (INTC), IP3 (DMAC) and IP7 (timers) go to the interrupt entry,
// which reads the controllers itself (SYS-12b); the vector returns when it
// finds a zero.
[[gnu::section(".inttable"), gnu::used]]
Handler interrupt_table[8] = {
    nullptr, nullptr, interruptEntry, interruptEntry,
    nullptr, nullptr, nullptr, interruptEntry,
};

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
