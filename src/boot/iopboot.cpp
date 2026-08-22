// IOPBOOT: the IOP's module loader.
//
// docs/spec/03-boot-chain.md BOOT-7. The boot block enters this by a call to
// its ROM address with the latched RAM-size byte in $a0 (BOOT-6c); this
// rebuild reaches it through an ordinary C++ function call, and the call
// never returns.
//
// It is linked at the address its archive offset gives it (CMakeLists.txt
// computes that offset with a pre-pass over the manifest, before this file's
// own build exists to be measured) and a post-build check confirms the final
// image agrees. That is what makes this an ordinary component rather than
// the position-independent code it used to be: every call is a plain `jal`,
// and every fixed address below is a link-time constant like any other
// component's.
//
// The loading itself -- placing, fixing up, binding, registering, entering --
// is `src/iop/loader.hpp`, which `LOADCORE` carries its own copy of for the
// modules loaded on request later. What is here is the boot list: reading
// it, and walking it.
//
// This is an R3000A, so `ps2AddComponent`'s per-source flags pin it to
// MIPS I, the same as the compiled IOP modules.

#include "archive.hpp"
#include "../iop/loader.hpp"

#include <stdint.h>

namespace {

using namespace ps2::archive;
using namespace ps2::loader;

// What the boot leaves for anything that follows it. These live *above* the
// modules, not below: a module's bss is zeroed by the loader as it goes, and
// the boot list sat inside that range until an early module grew enough to
// wipe it. The load base only tells you where the first module starts.
//
//   +0x000  how many names resolved
//   +0x004  the base load address the `@` token set
//   +0x008  the archive's table, so a module need not carry its own scan
//   +0x00C  where the next module would go
//   +0x010  (address, size) pairs in the archive, in load order
//   +0x400  where each was loaded, once it has been
//
// The address is ABI: `src/iop/eesync.cpp` reads +0x008 and `tools/imgcheck.py`
// reads +0x000, +0x004 and +0x400, so the layout is fixed even though nothing
// in `docs/spec/` names it.
constexpr uintptr_t kBootList = 0x001F8100;
constexpr uint32_t kBootListMax = 64;
constexpr uintptr_t kBlCount = 0x000;
constexpr uintptr_t kBlBase = 0x004;
constexpr uintptr_t kBlTable = 0x008;
constexpr uintptr_t kBlNext = 0x00C;
constexpr uintptr_t kBlEntries = 0x010;
constexpr uintptr_t kBlLoaded = 0x400;

constexpr uintptr_t kRomSearchStart = 0xBFC00000;
constexpr uintptr_t kRomSearchEnd = 0xBFC80000;  // BOOT-6b: the first 512 KiB

[[nodiscard]] volatile uint32_t &bootListWord(uintptr_t offset) {
    return *reinterpret_cast<volatile uint32_t *>(kBootList + offset);
}

// The shared idiom every failure path ends in: stop where it can be seen
// rather than run on into whatever memory happens to hold.
[[noreturn]] void stop() {
    for (;;) {
    }
}

// BOOT-9's `@` token: the rest is hexadecimal, parsed the way the reference
// does -- '0'-'9' first, then anything in [0x57, 0x67) (nominally 'a'-'f',
// though the upper bound is not checked against the letter itself), and
// anything else taken as 'A'-'F' with no validation at all. IOPBTCONF only
// ever holds well-formed hex, so the leniency is never exercised, but it is
// exactly what the reference computes if it ever were.
[[nodiscard]] uint32_t hexDigit(uint32_t byte) {
    if (static_cast<uint32_t>(byte - '0') < 10) {
        return byte - '0';
    }
    if (static_cast<uint32_t>(byte - 0x57) < 16) {
        return byte - 0x57;
    }
    return byte - 0x37;
}

void writeBootEntry(uint32_t index, uint32_t rom_address, uint32_t size) {
    bootListWord(kBlEntries + index * 8) = rom_address;
    bootListWord(kBlEntries + index * 8 + 4) = size;
}

[[nodiscard]] uint32_t bootEntryAddress(uint32_t index) {
    return bootListWord(kBlEntries + index * 8);
}

// Place the module at `base`, bind, register and enter it (IRX-1, IRX-3,
// IRX-9, IRX-10, IRX-12). Returns the next base, or 0 on failure.
[[nodiscard]] uint32_t loadModule(uint32_t rom_address, uint32_t base_address) {
    const auto *rom = reinterpret_cast<const uint8_t *>(rom_address);
    Segments segments;
    if (!readHeaders(rom, segments)) {
        return 0;                              // IRX-1: not a module
    }
    const Placed placed = place(rom, segments, base_address);

    // IRX-12c: the record's +0x10 and +0x14 are the entry and $gp. It is
    // built just past the module, which is also where the next one starts.
    const uint32_t record_address = align16(placed.end);
    auto *record = reinterpret_cast<uint8_t *>(record_address);
    poke32(record + 0x00, base_address);                   // where it was put
    poke32(record + 0x04, placed.end - base_address);      // and how much memory
    poke32(record + 0x10, placed.entry);
    poke32(record + 0x14, placed.gp);

    // IRX-9 then IRX-10: bind what this module imports, so its entry can call
    // it, and register what it exports, so the modules after it can bind to
    // this one. The reference has a module register itself by calling
    // `loadcore` ordinal 6 from its entry; doing it in the loader instead
    // means the first module needs no loader to already exist.
    auto *segment_start = reinterpret_cast<uint8_t *>(base_address);
    auto *segment_end = reinterpret_cast<uint8_t *>(placed.end);
    bind(segment_start, segment_end);
    registerExports(segment_start, segment_end);

    // `return & 3` decides residency; with no allocator there is nothing to
    // give back yet, so a module asking to go simply is not there.
    (void)callEntry(placed.entry, placed.gp, 0, nullptr, record_address);

    return record_address + 0x20;              // past the record: the next base
}

}  // namespace

extern "C" {

// BOOT-6c: the boot block enters this with the latched RAM-size byte in $a0.
// The reference sizes its own stack from it (BOOT-7); this rebuild is called
// from `reset_iop.cpp` with a stack already sized the same way, so there is
// nothing left for the argument to do here. It stays in the signature so the
// call site keeps placing it where BOOT-6c says it must.
[[noreturn]] void iopboot([[maybe_unused]] uint32_t ram_size_byte) {
    // Resolve IOPBTCONF with the archive scan (BOOT-6a).
    const Found list = find(kRomSearchStart, kRomSearchEnd, packName("IOPBTCONF"));
    if (list.address == 0) {
        stop();                                // BOOT-9a: no list, no boot
    }
    const auto *text = reinterpret_cast<const uint8_t *>(list.address);

    // BOOT-9: whitespace-separated tokens, where any byte below 0x20 ends one.
    uint32_t i = 0;
    uint32_t module_count = 0;
    uint32_t base_address = 0;                 // the base load address, from `@`
    while (i < list.size) {
        while (i < list.size && text[i] < 0x20) {
            i++;
        }
        if (i >= list.size) {
            break;
        }
        const uint8_t c = text[i];
        if (c == '@') {
            // `@` sets the base load address; the rest of the token is hex.
            i++;
            base_address = 0;
            while (i < list.size && text[i] >= 0x20) {
                base_address = (base_address << 4) + hexDigit(text[i]);
                i++;
            }
        } else if (c == '#') {
            // BOOT-9b: a directive is not a name.
            while (i < list.size && text[i] >= 0x20) {
                i++;
            }
        } else {
            // A module name. Copy the token into the ten-byte, NUL-padded
            // form an entry name has -- ARC-9 resolves it, and BOOT-9a says a
            // miss aborts the boot rather than skipping the entry.
            char name[kNameLength] = {};
            uint32_t n = 0;
            while (i < list.size && text[i] >= 0x20) {
                if (n < kNameLength) {
                    name[n++] = static_cast<char>(text[i]);
                }
                i++;
            }
            const Found module = find(kRomSearchStart, kRomSearchEnd, name);
            if (module.address == 0) {
                stop();                        // BOOT-9a
            }
            if (module_count >= kBootListMax) {
                stop();
            }
            writeBootEntry(module_count, static_cast<uint32_t>(module.address),
                            module.size);
            module_count++;
        }
    }

    bootListWord(kBlCount) = module_count;
    bootListWord(kBlBase) = base_address;

    // Leave the archive's table where a module can find it. `ROMDIR` is the
    // entry that describes the table, so resolving that name with the scan we
    // already have yields the table's own address -- and a module then needs
    // no further copy of the scan to read the archive.
    const Found romdir = find(kRomSearchStart, kRomSearchEnd, packName("ROMDIR"));
    bootListWord(kBlTable) = static_cast<uint32_t>(romdir.address);

    // Load each module in turn, in the order the list gave them (BOOT-9c). The
    // base advances past each one, so a module's address depends on every
    // module before it -- which is why the order is ABI and not a preference.
    uint32_t running_base = base_address;
    for (uint32_t index = 0; index < module_count; index++) {
        const uint32_t rom_address = bootEntryAddress(index);
        bootListWord(kBlLoaded + index * 4) = running_base;  // where this one goes
        bootListWord(kBlNext) = running_base;   // and, so far, where the next goes
        const uint32_t next_base = loadModule(rom_address, running_base);
        if (next_base == 0) {
            stop();                            // a module that will not load
        }
        running_base = next_base;
    }
    bootListWord(kBlNext) = running_base;       // where the next module would go
    stop();
}

}  // extern "C"
