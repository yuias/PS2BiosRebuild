// The IOP's module loader, for whichever module is doing the loading.
//
// docs/spec/02-module-abi.md IRX-1, IRX-3, IRX-9, IRX-10 and IRX-12: read an
// IRX's two segments, place the load segment and zero its bss, apply the
// fixups, bind what it imports against the library registry, register what
// it exports, and call its entry with the module's own $gp installed. The
// boot block does this for the boot list (`src/boot/iopboot.cpp`) and
// `LOADCORE` does it for modules loaded on request (docs/analysis/39: the
// reference keeps independent copies in IOPBOOT, LOADCORE and MODLOAD). As
// with `src/boot/archive.hpp`, every function is `static` so that each
// consumer carries its own copy of the instructions: the boot block is linked
// at its ROM address and a module is relocatable, and a symbol shared between
// them would resolve to the wrong one.

#pragma once

#include <stdint.h>

namespace ps2::loader {

// The head of the library registry (IRX-10). It is a word of low RAM rather
// than something `LOADCORE` owns, because the first module is loaded before
// any `LOADCORE` exists.
inline constexpr uintptr_t kRegistryHead = 0x001F8010;

inline constexpr uint32_t kExportMagic = 0x41C00000;
inline constexpr uint32_t kImportMagic = 0x41E00000;
inline constexpr uint32_t kTableHeader = 0x14;

// IRX-1 and IRX-3's shapes, read directly out of the file: every field used
// here is naturally aligned, since archive files sit on 16-byte boundaries
// and the ELF format itself aligns them from there.
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

inline constexpr uint32_t kPtLoad = 1;
inline constexpr uint32_t kPtIopMod = 0x70000080;
inline constexpr uint32_t kShtRel = 9;
inline constexpr uint32_t kRMips32 = 2;
inline constexpr uint32_t kRMips26 = 4;
inline constexpr uint32_t kRMipsHi16 = 5;
inline constexpr uint32_t kRMipsLo16 = 6;

[[nodiscard]] static uint32_t peek32(const void *address) {
    return *reinterpret_cast<const uint32_t *>(address);
}

static void poke32(void *address, uint32_t value) {
    *reinterpret_cast<uint32_t *>(address) = value;
}

[[nodiscard]] static uint16_t peek16(const void *address) {
    return *reinterpret_cast<const uint16_t *>(address);
}

[[nodiscard]] static volatile uint32_t &registryHead() {
    return *reinterpret_cast<volatile uint32_t *>(kRegistryHead);
}

// What the two program headers of IRX-1 say about a file.
struct Segments {
    uint32_t load_offset;
    uint32_t load_filesz;
    uint32_t load_memsz;
    uint32_t iopmod_offset;
};

// IRX-1: an ELF32 for MIPS with e_type 0xFF80, a load segment and an
// `.iopmod` segment. Returns false for anything else.
[[nodiscard]] static bool readHeaders(const uint8_t *file, Segments &segments) {
    const auto &header = *reinterpret_cast<const Elf32Header *>(file);
    if (header.type != 0xFF80) {
        return false;
    }
    segments = {0, 0, 0, 0};
    const uint8_t *phdr = file + header.phoff;
    for (uint16_t i = 0; i < header.phnum; i++, phdr += header.phentsize) {
        const auto &ph = *reinterpret_cast<const Elf32ProgramHeader *>(phdr);
        if (ph.type == kPtLoad) {
            segments.load_offset = ph.offset;
            segments.load_filesz = ph.filesz;
            segments.load_memsz = ph.memsz;
        } else if (ph.type == kPtIopMod) {
            segments.iopmod_offset = ph.offset;
        }
    }
    return segments.load_filesz != 0 && segments.iopmod_offset != 0;
}

// IRX-3a: the HI16s waiting for a LO16. One per address in the reference's
// modules; a compiler that hoists several `lui` of one address apart from
// their uses emits a short run of them before the LO16 they share, and every
// one of the run is rebased by it. `tools/mkirx.py` refuses a longer run.
inline constexpr uint32_t kHeldHi16Max = 8;

struct HeldHi16 {
    uint32_t *address[kHeldHi16Max];
    uint32_t stored[kHeldHi16Max];         // what the file had in each `lui`
    uint32_t count;
    bool applied;                          // a LO16 has used this run
};

// One REL entry, with the held HI16s (IRX-3a) threaded through by the caller.
static void applyRelocation(const Elf32Rel &entry, uint8_t *base, HeldHi16 &held) {
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
static void relocate(const uint8_t *file, uint32_t base_address) {
    const auto &header = *reinterpret_cast<const Elf32Header *>(file);
    auto *base = reinterpret_cast<uint8_t *>(base_address);
    const uint8_t *section = file + header.shoff;
    for (uint16_t s = 0; s < header.shnum; s++, section += header.shentsize) {
        const auto &sh = *reinterpret_cast<const Elf32SectionHeader *>(section);
        if (sh.type != kShtRel) {
            continue;
        }
        const uint8_t *rel = file + sh.offset;
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

// Where a placed module's code begins and ends, and what its `.iopmod` asks
// to have installed (IRX-2).
struct Placed {
    uint32_t entry;
    uint32_t gp;
    uint32_t end;
};

// Copy the load segment to `base_address`, zero the bss behind it and apply
// the fixups (IRX-1, IRX-3).
[[nodiscard]] static Placed place(const uint8_t *file, const Segments &segments,
                                  uint32_t base_address) {
    const uint8_t *iopmod = file + segments.iopmod_offset;
    Placed placed;
    placed.entry = peek32(iopmod + 4) + base_address;
    placed.gp = peek32(iopmod + 8);
    if (placed.gp != 0) {
        placed.gp += base_address;
    }

    const auto *src = reinterpret_cast<const uint32_t *>(file + segments.load_offset);
    auto *dst = reinterpret_cast<uint32_t *>(base_address);
    uint32_t moved = 0;
    for (; moved < segments.load_filesz; moved += 4) {
        *dst++ = *src++;
    }
    for (; moved < segments.load_memsz; moved += 4) {
        *dst++ = 0;
    }
    placed.end = base_address + segments.load_memsz;

    relocate(file, base_address);
    return placed;
}

// IRX-9: bind every import table found between the two addresses. An
// unresolved import is left as it is: IRX-8a makes an unbound stub return
// harmlessly rather than fail the boot.
static void bindTable(uint8_t *import_table, const uint8_t *export_table) {
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

static void bind(uint8_t *start, uint8_t *end) {
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
static void registerExports(uint8_t *start, uint8_t *end) {
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

// entry(argc, argv, 0, record), with the module's own $gp installed for the
// call and the loader's restored to zero afterwards (IRX-12). This is the one
// place a plain C++ call will not do: $gp has to hold a value chosen at run
// time, for a callee that may be hand-written assembly reading it directly,
// and nothing about a normal call site pins a register that way. Returns
// what the entry returned, whose low two bits decide residency (IRX-12a).
[[nodiscard]] static uint32_t callEntry(uint32_t entry, uint32_t gp, uint32_t argc,
                                        char **argv, uint32_t record) {
    uint32_t result;
    asm volatile(
        "move  $a0, %3\n\t"
        "move  $a1, %4\n\t"
        "move  $a2, $zero\n\t"
        "move  $a3, %5\n\t"
        "move  $gp, %2\n\t"
        "jalr  %1\n\t"
        "nop\n\t"
        "move  $gp, $zero\n\t"
        "move  %0, $v0\n\t"
        : "=r"(result)
        : "r"(entry), "r"(gp), "r"(argc), "r"(argv), "r"(record)
        // Numeric register names: clang's MIPS inline-asm clobber parser does
        // not accept the symbolic ABI names ($at, $v0, $a0, ...) that the
        // instruction text above uses freely.
        : "$1", "$2", "$3", "$4", "$5", "$6", "$7", "$8", "$9", "$10", "$11",
          "$12", "$13", "$14", "$15", "$24", "$25", "$28", "$31", "memory");
    return result;
}

}  // namespace ps2::loader
