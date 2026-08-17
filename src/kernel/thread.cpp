// Thread records, and the main thread's setup: spec/05 SYS-8.
//
// docs/spec/05-ee-syscall-abi.md SYS-8, from docs/analysis/30. A program's
// runtime calls 0x3C before anything else, to be given a stack pointer and its
// argument list; 0x3D and 0x3E place the heap. All three work on the current
// thread's record, which is also where a launcher (program.cpp) leaves the
// arguments for 0x3C to hand over.
//
// There is one thread so far and no scheduler; the table is sized for what
// the reference has so the records exist for the scheduler to use.

#include <stdint.h>

namespace {

constexpr uint32_t kThreads = 256;
// SYS-8a: the frame above the returned stack pointer is a saved context of
// EE-7e's shape -- sixteen bytes a register -- and these are the registers
// 0x3C primes in it.
constexpr uint32_t kContextBytes = 0x2A0;
constexpr uint32_t kSlotGp = 28 * 16;
constexpr uint32_t kSlotSp = 29 * 16;
constexpr uint32_t kSlotFp = 30 * 16;
constexpr uint32_t kSlotRa = 31 * 16;
// SYS-8b: the block a program hands to 0x3C comes back as argc, sixteen argv
// words, then the strings.
constexpr uint32_t kArgvSlots = 16;
constexpr uint32_t kStringsAt = 4 + kArgvSlots * 4;

struct ThreadRecord {
    uint32_t entry;
    uint32_t context;       // what 0x3C answered: the frame's address
    uint32_t gp;
    uint32_t argc;
    const char *args;       // the strings, packed, NUL after each
    uint32_t stack;         // base
    uint32_t stack_size;
    uint32_t root;
    uint32_t heap_end;
};

ThreadRecord thread_table[kThreads];
uint32_t current_thread = 0;

[[nodiscard]] ThreadRecord &current() {
    return thread_table[current_thread];
}

void writeWord(uintptr_t address, uint32_t value) {
    *reinterpret_cast<volatile uint32_t *>(address) = value;
}

}  // namespace

extern "C" {

// EE-2b / EE-4a: RDRAM's return, kept by the kernel entry (entry.cpp).
uint32_t memory_size;

// The launcher's half of SYS-8d: record what 0x3C will hand over.
void setProgramArguments(uint32_t argc, const char *packed) {
    ThreadRecord &thread = current();
    thread.argc = argc;
    thread.args = packed;
}

// Slot 0x3C, SYS-8a and SYS-8b. `root` arrives in $t0, which syscall.S puts
// where this function's ABI expects a fifth argument.
uint32_t sysSetupThread(uint32_t gp, uint32_t stack, uint32_t stack_size,
                        uint32_t *args, uint32_t root) asm("_sys_setup_thread");
uint32_t sysSetupThread(uint32_t gp, uint32_t stack, uint32_t stack_size,
                        uint32_t *args, uint32_t root) {
    // SYS-8a: -1 asks for the top of memory. The reference compares all
    // sixty-four bits, so a zero-extended 0xFFFFFFFF wraps there; here it is
    // taken as -1 too (docs/implementation.md).
    if (stack == 0xFFFFFFFF) {
        stack = memory_size - 0x1000 - stack_size;
    }
    const uint32_t top = stack + stack_size;
    const uint32_t frame = top - kContextBytes;
    writeWord(frame + kSlotGp, gp);
    writeWord(frame + kSlotSp, top - 0x20);
    writeWord(frame + kSlotFp, top - 0x20);
    writeWord(frame + kSlotRa, root);

    ThreadRecord &thread = current();
    thread.context = frame;
    thread.gp = gp;
    thread.stack = stack;
    thread.stack_size = stack_size;
    thread.root = root;

    // SYS-8b: fill the caller's block from the list the launcher left.
    if (args != nullptr) {
        args[0] = thread.argc;
        auto *strings = reinterpret_cast<char *>(args) + kStringsAt;
        const char *from = thread.args;
        for (uint32_t k = 0; k < thread.argc && k < kArgvSlots; k++) {
            args[1 + k] = reinterpret_cast<uintptr_t>(strings);
            if (from != nullptr) {
                do {
                    *strings++ = *from;
                } while (*from++ != '\0');
            } else {
                *strings++ = '\0';
            }
        }
    }
    thread.args = reinterpret_cast<const char *>(args);
    return frame;
}

// Slot 0x3D, SYS-8c.
uint32_t sysSetupHeap(uint32_t start, int32_t size) asm("_sys_setup_heap");
uint32_t sysSetupHeap(uint32_t start, int32_t size) {
    ThreadRecord &thread = current();
    thread.heap_end = size < 0 ? thread.stack
                               : start + static_cast<uint32_t>(size);
    return thread.heap_end;
}

// Slot 0x3E, SYS-8c.
uint32_t sysEndOfHeap() asm("_sys_end_of_heap");
uint32_t sysEndOfHeap() {
    return current().heap_end;
}

}  // extern "C"
