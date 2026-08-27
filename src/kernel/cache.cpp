// The cache and CP0 band: syscall slots 0x60 to 0x63, and the fourth table the
// last of them dispatches through.
//
// docs/spec/04-ee-kernel.md EE-8e and docs/spec/05-ee-syscall-abi.md SYS-4.
// The first three are published through KSEG1 rather than KSEG0, and that is a
// hardware requirement, not a convention: each one reconfigures or invalidates
// the very cache its own instruction fetches would come through. The link
// script forms the aliases; nothing here can enforce it, which is why the gate
// does.
//
// Each takes one argument. The three cache calls return nothing (SYS-4); the
// CP0 read returns the register (SYS-4c).

#include <stdint.h>

namespace {

// Config's cache-enable bits sit at 16 and 17, so an argument naming caches is
// shifted there; its low two bits are the two caches, in that order -- data
// first, instruction second, which is the order Config itself uses.
constexpr uint32_t kConfigEnableShift = 16;
constexpr uint32_t kCacheArgumentMask = 3;
constexpr uint32_t kDataCache = 1;
constexpr uint32_t kInstructionCache = 2;

// The tag walks: one line at a time, both ways of each line, over the size of
// the cache being swept. The instruction cache's tags cover twice what the
// data cache's do -- 16 KiB against 8 KiB, both across two ways.
constexpr uint32_t kCacheLine = 0x40;
constexpr uint32_t kDCacheSpan = 0x1000;
constexpr uint32_t kICacheSpan = 0x2000;

// The `cache` operations each walk uses. Each names its cache in the operation
// itself -- the instruction has no other way to say which -- so the data and
// instruction forms are different numbers, not the same number applied twice.
// Each is a *field of the instruction*, so it has to reach the assembler as a
// literal, hence the template parameter.
constexpr int kDCacheInvalidate = 0x16;
constexpr int kDCacheWriteback = 0x14;
constexpr int kICacheInvalidate = 0x07;

// `sync.p` has no mnemonic in this assembler, so it goes in as the word it is.
void syncP() {
    asm volatile(".word 0x0000040F" ::: "memory");
}

void syncL() {
    asm volatile("sync" ::: "memory");
}

[[nodiscard]] uint32_t readConfig() {
    uint32_t value;
    asm volatile("mfc0 %0, $16" : "=r"(value));
    return value;
}

void writeConfig(uint32_t value) {
    asm volatile("mtc0 %0, $16" : : "r"(value) : "memory");
}

template <int Op, int Way>
void cacheLine(uint32_t address) {
    asm volatile("cache %0, %1(%2)"
                 :
                 : "i"(Op), "i"(Way), "r"(address)
                 : "memory");
}

[[nodiscard]] bool cacheIsOn(uint32_t which) {
    return ((readConfig() >> kConfigEnableShift) & which) != 0;
}

// Both ways of every line over the cache's span, with the barrier the walk
// needs on either side of each line.
template <int Op, bool ParallelSync>
void walk(uint32_t span) {
    for (uint32_t address = 0; address < span; address += kCacheLine) {
        if constexpr (ParallelSync) {
            syncP();
            cacheLine<Op, 0>(address);
            cacheLine<Op, 1>(address);
            syncP();
        } else {
            syncL();
            cacheLine<Op, 0>(address);
            syncL();
            cacheLine<Op, 1>(address);
            syncL();
        }
    }
}

}  // namespace

extern "C" {

void sysSetCacheMode(uint32_t mode) asm("_sys_set_cache_mode");
void sysEnableCache(uint32_t which) asm("_sys_enable_cache");
void sysDisableCache(uint32_t which) asm("_sys_disable_cache");
uint32_t sysReadCop0(uint32_t index) asm("_sys_read_cop0");

uint32_t cop0Read0() asm("_cop0_read_0");
uint32_t cop0Read1() asm("_cop0_read_1");
uint32_t cop0Read2() asm("_cop0_read_2");
uint32_t cop0Read3() asm("_cop0_read_3");
uint32_t cop0Read4() asm("_cop0_read_4");
uint32_t cop0Read5() asm("_cop0_read_5");
uint32_t cop0Read6() asm("_cop0_read_6");
uint32_t cop0ReadNone() asm("_cop0_read_none");

// tables.cpp -- EE-6f.
extern void (*cop0_read_table[8])();

// Slot 0x60: set the cache mode, which is the low three bits of `Config`.
//
// The reference clears those three bits and then **ANDs** the argument in
// rather than ORing it, which makes the result unconditionally zero -- see
// `docs/analysis/18-ee-cache-syscalls.md`, where the raw instruction word is
// quoted, and `spec/05` SYS-4a. It is reproduced rather than corrected: every
// retail machine behaves this way, so software written for the platform met
// this behaviour and not the intended one.
void sysSetCacheMode(uint32_t mode) {
    const uint32_t cleared = readConfig() & ~uint32_t{7};
    writeConfig(cleared & (mode & kCacheArgumentMask));    // SYS-4a: and, not or
    syncP();
}

// Slot 0x61: turn on the caches named in the argument.
//
// A cache that is currently *off* is invalidated before it is enabled: its tags
// are whatever they were when it was turned off, and enabling it without
// clearing them would let stale lines answer. One that is already on is left
// alone, so the call is safe to repeat.
void sysEnableCache(uint32_t which) {
    which &= kCacheArgumentMask;
    if (!cacheIsOn(kDataCache)) {
        walk<kDCacheInvalidate, false>(kDCacheSpan);
    }
    if (!cacheIsOn(kInstructionCache)) {
        walk<kICacheInvalidate, true>(kICacheSpan);
    }
    syncL();
    writeConfig(readConfig() | (which << kConfigEnableShift));
    syncP();
}

// Slot 0x62: turn off the caches named in the argument.
//
// The mirror of 0x61, and the polarity of both tests is reversed with it: here
// a cache that is currently *on* is swept before it is disabled, because its
// dirty lines are the only copy of what they hold. One already off is skipped.
void sysDisableCache(uint32_t which) {
    which &= kCacheArgumentMask;
    if (cacheIsOn(kDataCache)) {
        walk<kDCacheWriteback, false>(kDCacheSpan);
    }
    if (cacheIsOn(kInstructionCache)) {
        walk<kICacheInvalidate, true>(kICacheSpan);
    }
    syncL();
    writeConfig(readConfig() & ~(which << kConfigEnableShift));
    syncP();
}

// Slot 0x63: read the CP0 register the argument names.
//
// SYS-4c. This is a dispatcher rather than a function, and it has to be: the
// register number is a *field of the `mfc0` instruction*, so it cannot be
// supplied at run time. A table of one stub per register is the only way to
// offer the call at all, which is why the kernel publishes a fourth table
// (EE-6f) beside its three dispatch tables.
//
// The index is not checked, as it is not in the reference: a caller passing
// more than seven reads a word past the table and jumps to it. The address is
// formed by hand rather than by subscripting the array, so that reproducing
// that behaviour does not also hand the optimiser an out-of-range subscript.
uint32_t sysReadCop0(uint32_t index) {
    using Reader = uint32_t (*)();
    const auto slot = reinterpret_cast<uintptr_t>(cop0_read_table)
                      + index * sizeof(void (*)());
    return reinterpret_cast<Reader>(*reinterpret_cast<void (**)()>(slot))();
}

// The stubs themselves. Index 7 has no register to read -- the R5900 has
// nothing there -- and returns without writing `$v0` at all, so the caller sees
// whatever it already held rather than a value invented here.
#define PS2_COP0_READER(name, number)                     \
    uint32_t name() {                                     \
        uint32_t value;                                   \
        asm volatile("mfc0 %0, $" #number : "=r"(value)); \
        return value;                                     \
    }

PS2_COP0_READER(cop0Read0, 0)
PS2_COP0_READER(cop0Read1, 1)
PS2_COP0_READER(cop0Read2, 2)
PS2_COP0_READER(cop0Read3, 3)
PS2_COP0_READER(cop0Read4, 4)
PS2_COP0_READER(cop0Read5, 5)
PS2_COP0_READER(cop0Read6, 6)

#undef PS2_COP0_READER

[[gnu::naked]] uint32_t cop0ReadNone() {
    asm volatile("jr $ra\n\tnop");
}

}  // extern "C"
