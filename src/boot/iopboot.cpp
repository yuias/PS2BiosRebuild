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
// This is an R3000A, so `ps2AddComponent`'s per-source flags pin it to
// MIPS I, the same as the compiled IOP modules.

#include "archive.hpp"

#include <stdint.h>

namespace {

using namespace ps2::archive;

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

// The head of the library registry (IRX-10). It is a word of low RAM rather
// than something `LOADCORE` owns, because the first module is loaded before
// any `LOADCORE` exists; `src/iop/loadcore.S` shares this exact address.
constexpr uintptr_t kRegistryHead = 0x001F8010;

constexpr uint32_t kExportMagic = 0x41C00000;
constexpr uint32_t kImportMagic = 0x41E00000;
constexpr uint32_t kTableHeader = 0x14;

constexpr uintptr_t kRomSearchStart = 0xBFC00000;
constexpr uintptr_t kRomSearchEnd = 0xBFC80000;  // BOOT-6b: the first 512 KiB

// IRX-1 and IRX-3's shapes, read directly out of ROM: every field used here
// is naturally aligned, since archive files sit on 16-byte boundaries and the
// ELF format itself aligns them from there.
struct Elf32Header {
    uint8_t ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint32_t entry;
    uint32_t phoff;
    uint32_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
};
static_assert(sizeof(Elf32Header) == 52);

struct Elf32ProgramHeader {
    uint32_t type;
    uint32_t offset;
    uint32_t vaddr;
    uint32_t paddr;
    uint32_t filesz;
    uint32_t memsz;
    uint32_t flags;
    uint32_t align;
};
static_assert(sizeof(Elf32ProgramHeader) == 32);

struct Elf32SectionHeader {
    uint32_t name;
    uint32_t type;
    uint32_t flags;
    uint32_t addr;
    uint32_t offset;
    uint32_t size;
    uint32_t link;
    uint32_t info;
    uint32_t addralign;
    uint32_t entsize;
};
static_assert(sizeof(Elf32SectionHeader) == 40);

struct Elf32Rel {
    uint32_t offset;
    uint32_t info;
};
static_assert(sizeof(Elf32Rel) == 8);

constexpr uint32_t kPtLoad = 1;
constexpr uint32_t kPtIopMod = 0x70000080;
constexpr uint32_t kShtRel = 9;
constexpr uint32_t kRMips32 = 2;
constexpr uint32_t kRMips26 = 4;
constexpr uint32_t kRMipsHi16 = 5;
constexpr uint32_t kRMipsLo16 = 6;

[[nodiscard]] uint32_t peek32(const void *address) {
    return *reinterpret_cast<const uint32_t *>(address);
}

void poke32(void *address, uint32_t value) {
    *reinterpret_cast<uint32_t *>(address) = value;
}

[[nodiscard]] uint16_t peek16(const void *address) {
    return *reinterpret_cast<const uint16_t *>(address);
}

[[nodiscard]] volatile uint32_t &bootListWord(uintptr_t offset) {
    return *reinterpret_cast<volatile uint32_t *>(kBootList + offset);
}

[[nodiscard]] volatile uint32_t &registryHead() {
    return *reinterpret_cast<volatile uint32_t *>(kRegistryHead);
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

// IRX-9: bind every import table found between the two addresses. An
// unresolved import is left as it is: IRX-8a makes an unbound stub return
// harmlessly rather than fail the boot.
void bindTable(uint8_t *import_table, const uint8_t *export_table) {
    // Step 1: count the exporter's entries to the zero terminator.
    const uint8_t *export_entries = export_table + kTableHeader;
    uint32_t count = 0;
    while (peek32(export_entries + count * 4) != 0) {
        count++;
    }

    uint8_t *stub = import_table + kTableHeader;
    for (;;) {
        if (peek32(stub) == 0) {
            break;                            // the terminator
        }
        const uint32_t second_word = peek32(stub + 4);   // step 2
        if ((second_word >> 26) != 9) {        // not `addiu`: leave the stub
            stub += 8;
            continue;
        }
        const uint32_t ordinal = second_word & 0xFFFF;
        uint32_t instruction;
        if (ordinal < count) {
            const uint32_t target = peek32(export_entries + ordinal * 4);
            instruction = 0x08000000 | ((target >> 2) & 0x03FFFFFF);  // step 3
        } else {
            instruction = 0x03E00008;          // step 4: `jr $ra`
        }
        poke32(stub, instruction);             // IRX-9a: only the first word
        stub += 8;
    }
}

void bind(uint8_t *start, uint8_t *end) {
    for (uint8_t *at = start; at < end; at += 4) {
        if (peek32(at) != kImportMagic) {
            continue;
        }
        if ((peek16(at + 10) & 7) != 0) {
            continue;                          // IRX-9b: not ours to bind
        }
        const uint32_t tag0 = peek32(at + 12);
        const uint32_t tag1 = peek32(at + 16);

        // Find the exporter: the same tag, at the registry head-most entry.
        auto *exporter = reinterpret_cast<uint8_t *>(registryHead());
        while (exporter != nullptr
               && !(peek32(exporter + 12) == tag0
                    && peek32(exporter + 16) == tag1)) {
            exporter = reinterpret_cast<uint8_t *>(peek32(exporter));
        }
        if (exporter == nullptr) {
            continue;                          // nothing exports it yet
        }
        bindTable(at, exporter);
    }
}

// IRX-10a: link every export table in the segment into the registry. The
// comparison is on tag and major version -- a library's identity -- with a
// strictly greater minor superseding.
void registerExports(uint8_t *start, uint8_t *end) {
    for (uint8_t *at = start; at < end; at += 4) {
        if (peek32(at) != kExportMagic) {
            continue;
        }
        const uint32_t tag0 = peek32(at + 12);
        const uint32_t tag1 = peek32(at + 16);
        const uint16_t version = peek16(at + 8);
        const auto major = static_cast<uint8_t>(version >> 8);
        const auto minor = static_cast<uint8_t>(version);

        bool skip = false;
        auto *walk = reinterpret_cast<uint8_t *>(registryHead());
        while (walk != nullptr) {
            if (peek32(walk + 12) == tag0 && peek32(walk + 16) == tag1) {
                const uint16_t existing = peek16(walk + 8);
                if (static_cast<uint8_t>(existing >> 8) == major) {
                    // Same tag and major: the same library. Strictly greater
                    // supersedes; equal or lower is left unregistered.
                    skip = static_cast<uint8_t>(existing) >= minor;
                    break;
                }
                // Same tag, different major: a different library -- keep
                // walking rather than treating this as a match.
            }
            walk = reinterpret_cast<uint8_t *>(peek32(walk));
        }
        if (!skip) {
            // IRX-4c: the magic becomes the link.
            const uint32_t head = registryHead();
            poke32(at, head);
            registryHead() = reinterpret_cast<uint32_t>(at);
        }
    }
}

// IRX-3a: the HI16s waiting for a LO16. One per address in the reference's
// modules; a compiler that hoists several `lui` of one address apart from
// their uses emits a short run of them before the LO16 they share, and every
// one of the run is rebased by it. `tools/mkirx.py` refuses a longer run.
constexpr uint32_t kHeldHi16Max = 8;

struct HeldHi16 {
    uint32_t *address[kHeldHi16Max];
    uint32_t stored[kHeldHi16Max];         // what the file had in each `lui`
    uint32_t count;
    bool applied;                          // a LO16 has used this run
};

// One REL entry, with the held HI16s (IRX-3a) threaded through by the caller.
void applyRelocation(const Elf32Rel &entry, uint8_t *base, HeldHi16 &held) {
    auto *word = reinterpret_cast<uint32_t *>(base + entry.offset);
    switch (entry.info & 0xFF) {
    case kRMips32:
        *word = *word + reinterpret_cast<uint32_t>(base);
        break;
    case kRMips26: {
        const uint32_t instruction = *word;
        const uint32_t byte_target = (instruction & 0x03FFFFFF) << 2;
        const uint32_t rebased = byte_target + reinterpret_cast<uint32_t>(base);
        const uint32_t target = (rebased >> 2) & 0x03FFFFFF;
        *word = (instruction & 0xFC000000) | target;
        break;
    }
    case kRMipsHi16:
        // A HI16 after a run was used starts the next run; one right after
        // another HI16 joins the run.
        if (held.applied || held.count >= kHeldHi16Max) {
            held.count = 0;
            held.applied = false;
        }
        held.address[held.count] = word;      // IRX-3a: hold it for its LO16
        held.stored[held.count] = *word;
        held.count++;
        break;
    case kRMipsLo16: {
        // A LO16 with no HI16 held is malformed (IRX-3a) and is left alone:
        // its high half is nowhere to be found, and address zero is not it.
        if (held.count == 0) {
            break;
        }
        const uint32_t instruction = *word;
        const auto low = static_cast<int32_t>(
            static_cast<int16_t>(instruction & 0xFFFF));
        const uint32_t address = (held.stored[0] << 16)
                                  + static_cast<uint32_t>(low)
                                  + reinterpret_cast<uint32_t>(base);
        *word = (instruction & 0xFFFF0000) | (address & 0xFFFF);

        // The carry out of the low half belongs to the high one. The run is
        // kept, not dropped, so a compiler's `lui` shared by several LO16s of
        // the same address (IRX-3a) has its high half recomputed for each --
        // to the same value, since `stored` still holds what the file had.
        uint32_t high = address >> 16;
        if ((address & 0x8000) != 0) {
            high += 1;
        }
        for (uint32_t k = 0; k < held.count; k++) {
            const uint32_t hi_instruction = *held.address[k];
            *held.address[k] = (hi_instruction & 0xFFFF0000) | (high & 0xFFFF);
        }
        held.applied = true;
        break;
    }
    default:
        break;
    }
}

// Apply the REL entries of IRX-3 to the segment just copied. The held HI16s
// reset at the start of every SHT_REL section, but persist across every entry
// within one.
void relocate(const uint8_t *rom, uint32_t base_address,
              const Elf32Header &header) {
    auto *base = reinterpret_cast<uint8_t *>(base_address);
    const uint8_t *section = rom + header.shoff;
    for (uint16_t s = 0; s < header.shnum; s++, section += header.shentsize) {
        const auto &sh = *reinterpret_cast<const Elf32SectionHeader *>(section);
        if (sh.type != kShtRel) {
            continue;
        }
        const uint8_t *rel = rom + sh.offset;
        const uint8_t *rel_end = rel + sh.size;
        HeldHi16 held;
        held.count = 0;
        held.applied = false;
        for (; rel < rel_end; rel += sizeof(Elf32Rel)) {
            applyRelocation(*reinterpret_cast<const Elf32Rel *>(rel), base,
                             held);
        }
    }
}

// entry(argc, argv, 0, record), with the module's own $gp installed for the
// call and the loader's restored to zero afterwards (IRX-12). This is the one
// place a plain C++ call will not do: $gp has to hold a value chosen at run
// time, for a callee that may be hand-written assembly reading it directly,
// and nothing about a normal call site pins a register that way.
void callEntry(uint32_t entry, uint32_t gp, uint32_t record) {
    asm volatile(
        "move  $a0, $zero\n\t"
        "move  $a1, $zero\n\t"
        "move  $a2, $zero\n\t"
        "move  $a3, %2\n\t"
        "move  $gp, %1\n\t"
        "jalr  %0\n\t"
        "nop\n\t"
        "move  $gp, $zero\n\t"
        :
        : "r"(entry), "r"(gp), "r"(record)
        // Numeric register names: clang's MIPS inline-asm clobber parser does
        // not accept the symbolic ABI names ($at, $v0, $a0, ...) that the
        // instruction text above uses freely.
        : "$1", "$2", "$3", "$4", "$5", "$6", "$7", "$8", "$9", "$10", "$11",
          "$12", "$13", "$14", "$15", "$24", "$25", "$28", "$31", "memory");
}

// Copy the single load segment to `base`, zero the bss behind it, apply the
// fixups, and call the entry (IRX-1, IRX-3, IRX-12). Returns the next base,
// or 0 on failure.
[[nodiscard]] uint32_t loadModule(uint32_t rom_address, uint32_t base_address) {
    const auto *rom = reinterpret_cast<const uint8_t *>(rom_address);
    const auto &header = *reinterpret_cast<const Elf32Header *>(rom);

    // IRX-1: an ELF32 for MIPS with e_type 0xFF80.
    if (header.type != 0xFF80) {
        return 0;
    }

    // Walk the program headers for the two segments of IRX-1.
    const uint8_t *phdr = rom + header.phoff;
    uint32_t load_offset = 0, load_filesz = 0, load_memsz = 0, iopmod_offset = 0;
    for (uint16_t i = 0; i < header.phnum; i++, phdr += header.phentsize) {
        const auto &ph = *reinterpret_cast<const Elf32ProgramHeader *>(phdr);
        if (ph.type == kPtLoad) {
            load_offset = ph.offset;
            load_filesz = ph.filesz;
            load_memsz = ph.memsz;
        } else if (ph.type == kPtIopMod) {
            iopmod_offset = ph.offset;
        }
    }
    if (load_filesz == 0 || iopmod_offset == 0) {
        return 0;                              // no load segment / no .iopmod
    }

    // IRX-2: the entry point and $gp the module wants installed.
    const uint8_t *iopmod = rom + iopmod_offset;
    const uint32_t entry = peek32(iopmod + 4) + base_address;
    uint32_t gp = peek32(iopmod + 8);
    if (gp != 0) {
        gp += base_address;
    }

    // Copy the load segment, then zero the bss behind it (IRX-1).
    const auto *src = reinterpret_cast<const uint32_t *>(rom + load_offset);
    auto *dst = reinterpret_cast<uint32_t *>(base_address);
    uint32_t moved = 0;
    for (; moved < load_filesz; moved += 4) {
        *dst++ = *src++;
    }
    for (; moved < load_memsz; moved += 4) {
        *dst++ = 0;
    }
    const uint32_t end_address = base_address + load_memsz;

    relocate(rom, base_address, header);

    // IRX-12c: the record's +0x10 and +0x14 are the entry and $gp. It is
    // built just past the module, which is also where the next one starts.
    const uint32_t record_address = align16(end_address);
    auto *record = reinterpret_cast<uint8_t *>(record_address);
    poke32(record + 0x00, base_address);                   // where it was put
    poke32(record + 0x04, end_address - base_address);     // and how much memory
    poke32(record + 0x10, entry);
    poke32(record + 0x14, gp);

    // IRX-9 then IRX-10: bind what this module imports, so its entry can call
    // it, and register what it exports, so the modules after it can bind to
    // this one. The reference has a module register itself by calling
    // `loadcore` ordinal 6 from its entry; doing it in the loader instead
    // means the first module needs no loader to already exist.
    auto *segment_start = reinterpret_cast<uint8_t *>(base_address);
    auto *segment_end = reinterpret_cast<uint8_t *>(end_address);
    bind(segment_start, segment_end);
    registerExports(segment_start, segment_end);

    // `return & 3` decides residency; with no allocator there is nothing to
    // give back yet, so a module asking to go simply is not there.
    callEntry(entry, gp, record_address);

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
