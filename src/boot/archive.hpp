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

// As above, but the name is ten bytes a caller assembled at run time rather
// than one folded into instructions by `packName` -- BOOT-9's boot-list
// tokens are not known until the archive is read, so there is no literal to
// pack. Those bytes live in RAM the caller owns, never in the ROM window this
// scans, so ARC-4's self-reference hazard does not apply to them.
[[nodiscard]] static bool named(const Entry &entry, const char *want) {
    for (uint32_t i = 0; i < kNameLength; i++) {
        if (entry.name[i] != want[i]) {
            return false;
        }
    }
    return true;
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
//
// A template so one body serves both a compile-time `Name` and a run-time
// `const char *` -- `named`'s overload set picks the right comparison for
// whichever a caller passes.
template <typename Want>
[[nodiscard]] static Found find(uintptr_t start, uintptr_t end, Want name) {
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

// BOOT-9's `@` token: the rest is hexadecimal, parsed the way the reference
// does -- '0'-'9' first, then anything in [0x57, 0x67) (nominally 'a'-'f',
// though the upper bound is not checked against the letter itself), and
// anything else taken as 'A'-'F' with no validation at all. IOPBTCONF only
// ever holds well-formed hex, so the leniency is never exercised, but it is
// exactly what the reference computes if it ever were.
[[nodiscard]] static constexpr uint32_t hexDigit(uint32_t byte) {
    if (static_cast<uint32_t>(byte - '0') < 10) {
        return byte - '0';
    }
    if (static_cast<uint32_t>(byte - 0x57) < 16) {
        return byte - 0x57;
    }
    return byte - 0x37;
}

// BOOT-8d: one word per name, the address of the module's file image, with a
// zero word after the last. The size the archive scan also found is not part
// of the list: a loader reads the ELF headers at that address for everything
// it needs.
}  // namespace ps2::archive
