// The EE's half of the boot block, from the point a stack exists.
//
// docs/spec/04-ee-kernel.md EE-1b establishes one in the scratchpad mapping the
// four instructions before this file made; everything after that is ordinary
// code and lives here. `reset.S` keeps only what has to run before there is a
// stack to call anything with.

#include <stdint.h>

namespace {

// The ROM window as the EE sees it, uncached. The archive is somewhere inside.
constexpr uintptr_t kRomStart = 0x9FC00000;
constexpr uintptr_t kRomEnd = 0x9FC80000;

// EE-3b: physical zero, uncached, is where KERNEL goes; EE-3d enters it past
// the vector page that occupies its first 0x1000 bytes.
constexpr uintptr_t kKernelLoad = 0xA0000000;
constexpr uintptr_t kKernelEntry = 0x80001000;

constexpr uint32_t kNameLength = 10;

// spec/01 ARC-2: sixteen bytes, a ten-byte NUL-padded name among them.
struct RomdirEntry {
    char name[kNameLength];
    uint16_t extinfo_size;
    uint32_t size;
};
static_assert(sizeof(RomdirEntry) == 16);

// ARC-3: every file is placed on a sixteen-byte boundary.
[[nodiscard]] constexpr uint32_t align16(uint32_t size) {
    return (size + 15) & ~uint32_t{15};
}

// A name to look for, as the two words and the halfword the field holds.
//
// It is packed at compile time on purpose. **This file lies inside the range it
// searches**, so a name kept as a string in memory is a sixteen-byte-aligned
// `RESET` that the scan finds before the real table -- and every other consumer
// of the archive runs its own copy of this scan over the same bytes. Folded
// into instructions, the name exists nowhere the scan can see it.
struct Name {
    uint32_t low;
    uint32_t high;
    uint16_t tail;
};

template <uint32_t N>
[[nodiscard]] consteval Name packName(const char (&text)[N]) {
    static_assert(N - 1 <= kNameLength);
    Name packed{0, 0, 0};
    for (uint32_t i = 0; i + 1 < N; i++) {
        const auto byte = static_cast<uint32_t>(static_cast<unsigned char>(text[i]));
        if (i < 4) {
            packed.low |= byte << (8 * i);
        } else if (i < 8) {
            packed.high |= byte << (8 * (i - 4));
        } else {
            packed.tail |= static_cast<uint16_t>(byte << (8 * (i - 8)));
        }
    }
    return packed;
}

[[nodiscard]] bool named(const RomdirEntry &entry, Name want) {
    const auto *words = reinterpret_cast<const uint32_t *>(entry.name);
    const auto tail = *reinterpret_cast<const uint16_t *>(entry.name + 8);
    return words[0] == want.low && words[1] == want.high && tail == want.tail;
}

// ARC-2's terminator: the first eight bytes of the name and the size, all zero.
[[nodiscard]] bool terminator(const RomdirEntry &entry) {
    for (uint32_t i = 0; i < 8; i++) {
        if (entry.name[i] != '\0') {
            return false;
        }
    }
    return entry.size == 0;
}

struct Found {
    uintptr_t address;
    uint32_t size;
};

// The self-locating scan of ARC-4, as BOOT-6a describes it: find a candidate
// table by its `RESET` first entry, then walk entries accumulating
// `align16(size)` to recover each file's offset.
[[nodiscard]] Found find(uintptr_t start, uintptr_t end, Name name) {
    for (uintptr_t at = start; at < end; at += sizeof(RomdirEntry)) {
        const auto &first = *reinterpret_cast<const RomdirEntry *>(at);
        if (!named(first, packName("RESET")) || (first.size & 15) != 0) {
            continue;                          // ARC-4: RESET is 16-aligned
        }
        // The table sits immediately after the RESET file, so the image base is
        // the table's address less that file's aligned size (ARC-3).
        uintptr_t file = at - align16(first.size);
        for (const auto *entry = &first;; entry++) {
            if (named(*entry, name)) {
                return {file, entry->size};
            }
            if (terminator(*entry)) {
                return {0, 0};
            }
            file += align16(entry->size);
        }
    }
    return {0, 0};                             // no table in range
}

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
