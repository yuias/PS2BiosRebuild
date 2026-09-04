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
// is `src/iop/loader.hpp`, which `LOADCORE` carries its own copy of. What is
// here is the boot list: reading it, placing the two modules a loader cannot
// load itself, and handing the rest to `LOADCORE` (BOOT-8c). `LOADCORE` *is*
// the loader; this is only what has to run before one exists.
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
// BOOT-8c: the eight-word block `LOADCORE`'s entry is passed. On this image
// it is also `SYSMEM`'s first heap byte, which the reference's is not --
// its heap starts above its own placed image. `LOADCORE` copies the block
// before it allocates anything, and nothing else may run in between.
constexpr uintptr_t kBootInfo = 0x00020000;
constexpr uintptr_t kBiRamMiB = 0x00;
constexpr uintptr_t kBiMode = 0x04;
constexpr uintptr_t kBiCommandLine = 0x08;
constexpr uintptr_t kBiSysmemBase = 0x0C;
constexpr uintptr_t kBiReservedBase = 0x10;
constexpr uintptr_t kBiReservedSize = 0x14;
constexpr uintptr_t kBiListCount = 0x18;
constexpr uintptr_t kBiList = 0x1C;

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

void writeBootEntry(uint32_t index, uint32_t rom_address) {
    bootListWord(kBlEntries + index * 4) = rom_address;
    bootListWord(kBlEntries + (index + 1) * 4) = 0;
}

[[nodiscard]] uint32_t bootEntryAddress(uint32_t index) {
    return bootListWord(kBlEntries + index * 4);
}

void bootInfoWord(uintptr_t offset, uint32_t value) {
    *reinterpret_cast<volatile uint32_t *>(kBootInfo + offset) = value;
}

}  // namespace

extern "C" {

// BOOT-6c: the boot block enters this with the latched RAM-size byte in $a0.
// The reference sizes its own stack from it (BOOT-7); this rebuild is called
// from `reset_iop.cpp` with a stack already sized the same way, so the
// argument's only remaining use is BOOT-8c's first word.
//
// The other three are a reboot's (docs/analysis/45 §2): `MODLOAD`'s teardown
// core re-enters this function rather than the reset vector, with a mode and,
// for an update reboot, the command line at a fixed low address. The cold
// boot's call site names them too rather than leaving them to whatever the
// registers held -- the reference reaches this with `jr` and takes its
// chances there.
[[noreturn]] void iopboot(uint32_t ram_size_byte, uint32_t mode,
                          uint32_t command_line, uint32_t) {
    // BOOT-9e: the list's name is built, not stored -- "IOPBTCONF" with its
    // ninth byte overwritten by the mode's digit, so a soft reboot asks for
    // `IOPBTCON1` and an update reboot's intermediate stage for `IOPBTCON2`.
    // The overwrite is unconditional, as the reference's is, so a cold boot
    // asks for `IOPBTCON0` and reaches the ordinary list by the same
    // fallback any mode without a list of its own does.
    char list_name[kNameLength] = {'I', 'O', 'P', 'B', 'T', 'C', 'O', 'N', 'F', 0};
    list_name[8] = static_cast<char>('0' + (mode & 0xF));
    Found list = find(kRomSearchStart, kRomSearchEnd, list_name);
    if (list.address == 0) {
        list = find(kRomSearchStart, kRomSearchEnd, packName("IOPBTCONF"));
    }
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
                base_address = (base_address << 4) + ps2::archive::hexDigit(text[i]);
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
            writeBootEntry(module_count, static_cast<uint32_t>(module.address));
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

    // BOOT-8c: only `SYSMEM` and `LOADCORE` are loaded here, in the order the
    // list gave them (BOOT-9c) -- the list's first two names, because a
    // loader that allocates needs a memory manager and a registry before it
    // can exist. Everything after them is `LOADCORE`'s to load.
    if (module_count < 2) {
        stop();                                 // no memory manager, no loader
    }
    uint32_t running_record = base_address;
    uint32_t sysmem_base = 0;
    for (uint32_t index = 0; index < 2; index++) {
        const uint32_t module_base = running_record + kRecordSize;
        bootListWord(kBlLoaded + index * 4) = module_base;
        bootListWord(kBlNext) = module_base;
        const Loaded loaded = placeModule(bootEntryAddress(index), running_record);
        if (loaded.next_base == 0) {
            stop();                             // a module that will not load
        }
        // IRX-9 over the text segment alone (IRX-8b): these two join the
        // registry the boot is building, so they bind against it.
        (void)bind(reinterpret_cast<uint8_t *>(module_base),
                   reinterpret_cast<uint8_t *>(module_base + loaded.text_size));
        if (index == 0) {
            // BOOT-8c: `SYSMEM`'s entry takes the RAM size **in bytes**, not
            // the byte code the boot block was given: the reference sizes its
            // heap's top from it, and a zero there leaves that module marking
            // itself uninitialised and refusing every allocation afterwards.
            // BOOT-8c: and what it answers is where the next module goes --
            // the reference's `SYSMEM` puts its heap above its own image and
            // names that heap's first free block. Ours has a fixed heap and
            // answers 0, which leaves the arithmetic here; the `SYSMEM` a
            // title's image supplies answers for itself, and this is the
            // number that has to be used then.
            const uint32_t placement = callEntry(
                loaded.entry, loaded.gp, ram_size_byte << 20, nullptr,
                loaded.record);
            sysmem_base = module_base;
            if (placement != 0) {
                running_record = placement;
                bootListWord(kBlNext) = running_record;
                continue;
            }
        }
        running_record = loaded.next_base;
        bootListWord(kBlNext) = running_record;
        if (index == 1) {
            // BOOT-8c: `LOADCORE`'s entry takes the block, not `argc`. It
            // loads the rest of the list and comes back, and the boot's own
            // thread goes to sleep below.
            bootInfoWord(kBiRamMiB, ram_size_byte);
            bootInfoWord(kBiMode, mode);
            bootInfoWord(kBiCommandLine, command_line);
            bootInfoWord(kBiSysmemBase, sysmem_base);
            bootInfoWord(kBiReservedBase, 0);
            bootInfoWord(kBiReservedSize, 0);
            bootInfoWord(kBiListCount, module_count);
            // BOOT-8d: the whole list, from index 0 -- the loader skips the
            // two the boot block has already placed itself. Ours stays where
            // it is rather than being copied above the command line the way
            // the reference's is: there is no command line on a cold boot,
            // and this table is outside the heap.
            bootInfoWord(kBiList, kBootList + kBlEntries);
            (void)callEntry(loaded.entry, loaded.gp, kBootInfo, nullptr,
                            loaded.record);
        }
    }

    // spec/06 IOP-3i: the boot has been running on a thread since THREADMAN
    // made it one, and with the list done that thread has nothing left to
    // do. It sleeps -- `thbase` ordinal 24, reached through the registry the
    // same way a module's import would be -- and the idle thread and the
    // interrupt handlers carry the machine from here. Without a thread
    // manager there is nothing to sleep on, and the boot stops instead.
    auto *exporter = reinterpret_cast<const uint8_t *>(registryHead());
    const Name thbase = packName("thbase");
    while (exporter != nullptr) {
        if (peek32(exporter + 12) == thbase.low && peek32(exporter + 16) == thbase.high) {
            const auto sleep = reinterpret_cast<void (*)()>(
                peek32(exporter + kTableHeader + 24 * 4));
            sleep();
            break;
        }
        exporter = reinterpret_cast<const uint8_t *>(peek32(exporter));
    }
    stop();
}

}  // extern "C"
