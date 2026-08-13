// The self-locating archive scan, for whichever processor is asking.
//
// docs/spec/01-rom-archive.md ARC-2 to ARC-4, and docs/spec/03-boot-chain.md
// BOOT-6a. The reference writes this scan out once per consumer rather than
// sharing one copy, and a rebuild must do the same: the two processors cannot
// call into each other's code, and the IOP's copy has to be compiled for an
// R3000A. A header gives each its own copy of the instructions from one written
// description, which is the part worth having in one place.
//
// The functions are `static` for that reason and not for tidiness. `inline`
// would make them weak, the linker would keep exactly one, and whichever
// processor lost would execute the other's instruction set -- which is BOOT-6a's
// warning arriving by a route it does not mention. A shared header is fine; a
// shared symbol is not.

#pragma once

#include <stdint.h>

namespace ps2::archive {

inline constexpr uint32_t kNameLength = 10;

// ARC-2: sixteen bytes, a ten-byte NUL-padded name among them.
struct Entry {
    char name[kNameLength];
    uint16_t extinfo_size;
    uint32_t size;
};
static_assert(sizeof(Entry) == 16);

// ARC-3: every file is placed on a sixteen-byte boundary.
[[nodiscard]] static constexpr uint32_t align16(uint32_t size) {
    return (size + 15) & ~uint32_t{15};
}

// A name to look for, as the two words and the halfword the field holds.
//
// It is packed at compile time on purpose. **The boot block lies inside the
// range it searches**, so a name kept as a string in memory is a sixteen-byte
// aligned `RESET` that the scan finds before the real table -- and every
// consumer of the archive runs its own copy of this scan over the same bytes.
// Folded into instructions, a name exists nowhere a scan can see it, which is
// also why the reference compares immediates rather than strings.
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
        const auto byte =
            static_cast<uint32_t>(static_cast<unsigned char>(text[i]));
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

[[nodiscard]] static bool named(const Entry &entry, Name want) {
    const auto *words = reinterpret_cast<const uint32_t *>(entry.name);
    const auto tail = *reinterpret_cast<const uint16_t *>(entry.name + 8);
    return words[0] == want.low && words[1] == want.high && tail == want.tail;
}

// ARC-2's terminator: the first eight bytes of the name and the size, all zero.
[[nodiscard]] static bool terminator(const Entry &entry) {
    const auto *words = reinterpret_cast<const uint32_t *>(entry.name);
    return words[0] == 0 && words[1] == 0 && entry.size == 0;
}

struct Found {
    uintptr_t address;
    uint32_t size;
};

// BOOT-6a: find a candidate table by its `RESET` first entry, then walk the
// entries accumulating `align16(size)` to recover each file's offset.
[[nodiscard]] static Found find(uintptr_t start, uintptr_t end, Name name) {
    for (uintptr_t at = start; at < end; at += sizeof(Entry)) {
        const auto &first = *reinterpret_cast<const Entry *>(at);
        if (!named(first, packName("RESET")) || (first.size & 15) != 0) {
            continue;                     // ARC-4: RESET's size is 16-aligned
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
    return {0, 0};                        // no table in range
}

}  // namespace ps2::archive
