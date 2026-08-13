// The EE's half of the boot block, from the point a stack exists.
//
// docs/spec/04-ee-kernel.md EE-1b establishes one in the scratchpad mapping the
// four instructions before this file made; everything after that is ordinary
// code and lives here. `reset.S` keeps only what has to run before there is a
// stack to call anything with.

#include "archive.hpp"

#include <stdint.h>

namespace {

using namespace ps2::archive;

// The ROM window as the EE sees it, uncached. The archive is somewhere inside.
constexpr uintptr_t kRomStart = 0x9FC00000;
constexpr uintptr_t kRomEnd = 0x9FC80000;

// EE-3b: physical zero, uncached, is where KERNEL goes; EE-3d enters it past
// the vector page that occupies its first 0x1000 bytes.
constexpr uintptr_t kKernelLoad = 0xA0000000;
constexpr uintptr_t kKernelEntry = 0x80001000;

}  // namespace

extern "C" {

[[noreturn]] void eeReset() {
    // EE-2: bring the memory controller up. The reference reaches `RDRAM` by a
    // hard-coded address; resolving it by name instead removes the constant that
    // EE-2a warns must never drift from the layout.
    const Found rdram = find(kRomStart, kRomEnd, packName("RDRAM"));
    if (rdram.address != 0) {
        reinterpret_cast<void (*)()>(rdram.address)();

        // EE-3: resolve KERNEL, copy it to physical zero, enter it.
        const Found kernel = find(kRomStart, kRomEnd, packName("KERNEL"));
        if (kernel.address != 0) {
            // The reference moves sixteen bytes an iteration with `lq`/`sq`;
            // four words land the same bytes and need no R5900-only encoding.
            const auto *from = reinterpret_cast<const uint32_t *>(kernel.address);
            auto *to = reinterpret_cast<uint32_t *>(kKernelLoad);
            for (uint32_t moved = 0; moved < align16(kernel.size); moved += 16) {
                *to++ = *from++;
                *to++ = *from++;
                *to++ = *from++;
                *to++ = *from++;
            }
            reinterpret_cast<void (*)()>(kKernelEntry)();
        }
    }

    // Nothing can be done without the kernel; stop where it can be seen rather
    // than run on (the EE has no POST register: BOOT-5b's IOP idiom).
    for (;;) {
    }
}

}  // extern "C"
