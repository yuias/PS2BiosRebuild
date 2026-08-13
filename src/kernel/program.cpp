// Loading a program from the archive: the part of EE-9 that is ordinary code.
//
// docs/spec/04-ee-kernel.md EE-9. `rom0:` is served by the IOP, so the file
// crosses the SIF before any of it is a program (EE-9d).
//
// The syscall entries stay in `program.S`: they jump into the program they
// loaded and never come back, which no compiler expresses; the reading,
// fetching and placing are here.

#include <stdint.h>

namespace {

enum class ElfType : uint16_t { Executable = 2 };
enum class SegmentType : uint32_t { Load = 1 };

struct ElfHeader {
    uint8_t ident[16];
    ElfType type;
    uint16_t machine;
    uint32_t version;
    uint32_t entry;
    uint32_t phoff;
    uint32_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
};

struct ProgramHeader {
    SegmentType type;
    uint32_t offset;
    uint32_t vaddr;
    uint32_t paddr;
    uint32_t filesz;
    uint32_t memsz;
    uint32_t flags;
    uint32_t align;
};

// `<algorithm>` needs a standard library this target has no build of, and
// `-fno-builtin` means the loops below stay loops rather than becoming calls to
// a `memcpy` the image does not link. Byte at a time is also the honest width:
// a segment's address comes out of the file and carries no alignment.
constexpr void copyBytes(uint8_t *to, const uint8_t *from, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        to[i] = from[i];
    }
}

constexpr void fillZero(uint8_t *to, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        to[i] = 0;
    }
}

// A program header is `phentsize` bytes on, which need not match `sizeof`.
[[nodiscard]] const ProgramHeader *segmentAt(const uint8_t *base,
                                             const ElfHeader &elf,
                                             uint16_t index) {
    return reinterpret_cast<const ProgramHeader *>(
        base + elf.phoff + uint32_t{index} * elf.phentsize);
}

}  // namespace

extern "C" {

// sif.S. BOOT-11: one exchange across the bus. Request 0 asks how large the
// file is and fills `sif_reply`; request 1 sends the bytes to `destination`.
void sifExchange(const char *name, int request, void *destination);

alignas(16) uint32_t sif_reply[4];
alignas(16) uint8_t staging[0x4000];    // where a program lands before it is one

// `extern` is load-bearing: a `const` object at namespace scope has internal
// linkage in C++, and `program.S` has to be able to name these.
extern const char kRom0Osdsys[] = "rom0:OSDSYS";
extern const char kBootBrowser[] = "BootBrowser";
extern const char kNoProgram[] =
    "# no program: the archive has nothing by that name.\n";

// The name after `:`, or the whole path when it names no device.
//
// Not `constexpr`: that implies `inline`, and a function `program.S` calls has
// to be emitted whether or not this translation unit uses it.
[[nodiscard]] const char *skipDevice(const char *path) {
    for (const char *at = path; *at != '\0'; at++) {
        if (*at == ':') {
            return at + 1;
        }
    }
    return path;
}

// Two exchanges: how large is it, then send it. The transfer itself no longer
// needs the size -- BOOT-11 has the sender frame it -- but the caller does,
// since it has to know how much of the buffer the answer filled.
[[nodiscard]] uint32_t fetchFile(const char *name, void *destination) {
    sifExchange(name, 0, sif_reply);
    const uint32_t size = sif_reply[0];
    if (size == 0) {
        return 0;
    }
    sifExchange(name, 1, destination);
    return size;
}

// An EE program is stored as the ELF it is, so the loader reads the program
// headers rather than being told a layout: each `PT_LOAD` segment is copied to
// the address it asks for, and the memory beyond what the file holds is zeroed,
// which is where a program's bss comes from.
[[nodiscard]] uint32_t placeSegments(const void *image) {
    const auto *base = static_cast<const uint8_t *>(image);
    const auto &elf = *reinterpret_cast<const ElfHeader *>(base);
    if (elf.type != ElfType::Executable) {
        return 0;
    }
    for (uint16_t index = 0; index < elf.phnum; index++) {
        const ProgramHeader &segment = *segmentAt(base, elf, index);
        if (segment.type != SegmentType::Load) {
            continue;
        }
        auto *to = reinterpret_cast<uint8_t *>(segment.vaddr);
        copyBytes(to, base + segment.offset, segment.filesz);
        fillZero(to + segment.filesz, segment.memsz - segment.filesz);
    }
    return elf.entry;
}

}  // extern "C"
