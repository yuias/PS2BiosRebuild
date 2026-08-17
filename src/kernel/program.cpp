// Loading a program from the archive: EE-9.
//
// docs/spec/04-ee-kernel.md EE-9. `rom0:` is served by the IOP, so the file
// crosses the SIF before any of it is a program (EE-9d). Slot 0x06 is the
// loader, slot 0x7B the same call with the path pinned to `rom0:OSDSYS`, and
// the default boot invokes it with a single argument. A successful load never
// returns: the program replaces the caller.

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

// A content request answers at most this much (the IOP's payload buffer), so a
// file is fetched a window at a time.
constexpr uint32_t kWindow = 0x4000;
// The ELF header and program headers are read first, and this is as much of
// the file as that needs.
constexpr uint32_t kHeaderBytes = 0x400;
constexpr uint32_t kVerbSize = 0;
constexpr uint32_t kVerbContent = 1;

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

// sif.cpp. BOOT-11: one exchange across the bus. Request kVerbSize asks how
// large the file is and fills `sif_reply`; kVerbContent sends the window
// [offset, offset + length) of its bytes to `destination`.
void *sifExchange(const char *name, uint32_t verb, void *destination,
                  uint32_t offset, uint32_t length);
void print(const char *text) asm("_print");

int sysLoadProgram(const char *path, int argc, char **argv)
    asm("_sys_load_program");
int sysLoadOsd(int argc, char **argv) asm("_sys_load_osd");
int bootDefault();

alignas(16) uint32_t sif_reply[4];
alignas(16) uint8_t staging[kHeaderBytes];   // a file's ELF and program headers
alignas(16) uint8_t bounce[kWindow];         // a window bound for an unaligned address

}  // extern "C"

namespace {

constexpr char kRom0Osdsys[] = "rom0:OSDSYS";
constexpr char kBootBrowser[] = "BootBrowser";
constexpr char kNoProgram[] =
    "# no program: the archive has nothing by that name.\n";

// The name after `:`, or the whole path when it names no device.
[[nodiscard]] const char *skipDevice(const char *path) {
    for (const char *at = path; *at != '\0'; at++) {
        if (*at == ':') {
            return at + 1;
        }
    }
    return path;
}

// Fetch a segment's bytes to where it lives. The channel writes whole
// quadwords at a quadword-aligned address, so a segment that starts on one
// takes its windows straight from the bus -- the last of them may run up to
// fifteen bytes past `filesz`, into the part of `memsz` that is zeroed after
// -- and any other segment goes through the staging buffer a window at a time.
void fetchSegment(const char *name, uint8_t *to, uint32_t offset,
                  uint32_t size) {
    const bool direct = (reinterpret_cast<uintptr_t>(to) & 15) == 0;
    for (uint32_t done = 0; done < size; done += kWindow) {
        const uint32_t length = size - done < kWindow ? size - done : kWindow;
        if (direct) {
            sifExchange(name, kVerbContent, to + done, offset + done, length);
        } else {
            sifExchange(name, kVerbContent, bounce, offset + done, length);
            copyBytes(to + done, bounce, length);
        }
    }
}

// An EE program is stored as the ELF it is, so the loader reads the program
// headers rather than being told a layout: each `PT_LOAD` segment is fetched
// to the address it asks for, and the memory beyond what the file holds is
// zeroed, which is where a program's bss comes from. Returns the entry point,
// or 0 when the archive has no such file or it is not an executable.
[[nodiscard]] uint32_t loadProgram(const char *name) {
    sifExchange(name, kVerbSize, sif_reply, 0, 0);
    if (sif_reply[0] == 0) {
        return 0;
    }
    // The header and the program headers come first, on their own.
    sifExchange(name, kVerbContent, staging, 0, kHeaderBytes);
    const auto &elf = *reinterpret_cast<const ElfHeader *>(staging);
    if (elf.type != ElfType::Executable
        || elf.phoff + uint32_t{elf.phnum} * elf.phentsize > kHeaderBytes) {
        return 0;
    }
    for (uint16_t index = 0; index < elf.phnum; index++) {
        const ProgramHeader &segment = *segmentAt(staging, elf, index);
        if (segment.type != SegmentType::Load) {
            continue;
        }
        auto *to = reinterpret_cast<uint8_t *>(segment.vaddr);
        fetchSegment(name, to, segment.offset, segment.filesz);
        fillZero(to + segment.filesz, segment.memsz - segment.filesz);
    }
    return elf.entry;
}

using Entry = void (*)(int argc, char **argv);

}  // namespace

extern "C" {

// Slot 0x06: load(path, argc, argv). The path is `rom0:NAME`; only the IOP can
// read that device (EE-9d), and it wants the bare archive name. Comes back
// only when there was nothing to run, with -1.
int sysLoadProgram(const char *path, int argc, char **argv) {
    const uint32_t entry = loadProgram(skipDevice(path));
    if (entry == 0) {
        print(kNoProgram);
        return -1;
    }
    reinterpret_cast<Entry>(entry)(argc, argv);   // EE-9: the program replaces us
    __builtin_unreachable();
}

// Slot 0x7B: EE-9b, the same call with the path pinned. The reference is a
// four-instruction wrapper that shifts the arguments along; so is this.
int sysLoadOsd(int argc, char **argv) {
    return sysLoadProgram(kRom0Osdsys, argc, argv);
}

// EE-9c: the default boot runs that program with one argument.
int bootDefault() {
    const char *argv[] = {kBootBrowser};
    return sysLoadProgram(kRom0Osdsys, 1, const_cast<char **>(argv));
}

}  // extern "C"
