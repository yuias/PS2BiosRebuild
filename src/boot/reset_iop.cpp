// The IOP's half of the boot block, from the point a stack exists.
//
// docs/spec/03-boot-chain.md BOOT-4 steps 3 to 5, BOOT-5's POST codes and
// BOOT-6's handoff. `reset.S` keeps step 1 and step 2 -- the bus table with its
// data, and the clearing of every general-purpose register, which cannot be
// written in a language that needs some of them to survive.
//
// Compiled `-march=mips1`: this is an R3000A, and the toolchain's default of
// MIPS III would let the compiler reach for instructions it does not have.

#include "archive.hpp"

#include <stdint.h>

namespace {

using namespace ps2::archive;

// BOOT-5b: the IOP writes its progress to a register a machine can be watched
// through, which is the only diagnostic this path has.
constexpr uintptr_t kPost = 0xBF802070;

// BOOT-4 step 3: low RAM is where the kernel builds its structures.
constexpr uintptr_t kLowRamEnd = 0xF80;

// BOOT-4 step 4: the uncached alias, read through to prime the cache.
constexpr uintptr_t kUncachedRam = 0xA0000000;
constexpr uint32_t kPrimeWords = 8;

// BOOT-4 step 5: where the RAM size is latched, and the retail record.
constexpr uintptr_t kRamSize = 0xBF801060;
constexpr uint32_t kRamSizeRetail = 0x0B;

// BOOT-6b: the archive's table is in the first 512 KiB of the ROM window.
constexpr uintptr_t kRomStart = 0xBFC00000;
constexpr uintptr_t kRomEnd = 0xBFC80000;

// BOOT-6c: what IOPBOOT is entered with.
constexpr uint32_t kRamSizeByte = 0x02;

void write32(uintptr_t address, uint32_t value) {
    *reinterpret_cast<volatile uint32_t *>(address) = value;
}

void post(uint8_t code) {
    *reinterpret_cast<volatile uint8_t *>(kPost) = code;
}

void clearCop0() {
    asm volatile("mtc0 $zero, $12\n\tmtc0 $zero, $13" ::: "memory");
}

}  // namespace

extern "C" {

[[noreturn]] void iopReset() {
    // BOOT-4 step 3: clear low RAM.
    for (uintptr_t at = 0; at < kLowRamEnd; at += 4) {
        write32(at, 0);
    }
    post(0x03);

    // BOOT-4 step 4: prime the cache through the uncached alias. The values are
    // not wanted; the fetches are.
    for (uint32_t word = 0; word < kPrimeWords; word++) {
        [[maybe_unused]] volatile const uint32_t primed =
            *reinterpret_cast<volatile const uint32_t *>(kUncachedRam
                                                         + word * 4);
    }
    post(0x04);

    // BOOT-4 step 2, continued: the COP0 half, which the register clear in
    // `reset.S` could not reach.
    clearCop0();
    post(0x05);

    // BOOT-4 step 5: latch the RAM size.
    write32(kRamSize, kRamSizeRetail);
    post(0x08);

    // BOOT-6: hand off by name.
    const Found iopboot = find(kRomStart, kRomEnd, packName("IOPBOOT"));
    if (iopboot.address == 0) {
        // BOOT-6d and BOOT-5b: a failed lookup is a terminal stop, not a fall
        // through into whatever follows.
        post(0xFA);
        for (;;) {
        }
    }

    post(0x09);
    // BOOT-6c: entered with the RAM-size byte, and it never returns. The
    // reference reaches it with `jr`, having nothing to return to; a call leaves
    // a return address IOPBOOT will not use.
    // BOOT-8b: mode 0 is the cold boot, and there is no command line.
    reinterpret_cast<void (*)(uint32_t, uint32_t, uint32_t, uint32_t)>(
        iopboot.address)(kRamSizeByte, 0, 0, 0);
    for (;;) {
    }
}

}  // extern "C"
