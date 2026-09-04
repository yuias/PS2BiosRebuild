// UDNL: the merge core a reboot's command line names.
//
// docs/analysis/45-iop-reboot.md §"UDNL: the merge core". A title asks for a
// reboot with `"rom0:UDNL cdrom0:\MODULES\IOPRP310.IMG;1"`; `MODLOAD`'s
// mode-2 bootup callback loads this module with the rest of that line as its
// arguments, and what it does is decide, name by name, which copy of each
// kernel module the next kernel is to be built from.
//
// The staging and the hand-over are `docs/analysis/51-udnl-staging.md`, and
// the shape of both is less than the name "staging" suggests. **No module is
// ever copied.** One buffer holds a boot block, a boot list, and the whole of
// every source file read in; the list is one word per name, the address of the
// winning module's bytes *where they already lie* -- inside the buffer for a
// module a title supplied, in the ROM window for one `rom0` won. The staged
// kernel then loads its modules out of that.
//
// The hand-over is `src/boot/iopboot.cpp`'s own sequence, which is the point:
// place `SYSMEM`, call its entry with the machine's RAM in bytes, take the
// address it answers as where `LOADCORE`'s record goes, place `LOADCORE`,
// tell the block where `SYSMEM` actually landed, and call `LOADCORE` with the
// block (BOOT-8c). It does not return.
//
// The rule, from §"The merge rule":
//
// - Every named source is opened, and `rom0` is always one more, unnamed.
// - `IOPBTCONF` is resolved by name across that pool, newest source first,
//   and the first one found supplies the **order**. A title's image carries
//   none, so in practice `rom0`'s does.
// - Per name, every source is walked and the **strictly greater** version
//   wins, so the newest source wins ties. The version compared is the
//   archive entry's own `EXTINFO` record (`spec/01` ARC-6c), not anything
//   read out of the ELF: a module's version and the version of a library it
//   *exports* are independent fields, and `ROMDRV` is the case where they
//   differ.
//
// `tools/iopmerge.py` is the same rule in Python, and the two are meant to
// agree name for name -- which is how this one is judged, there being no
// simulator that can run it.

#include "../boot/archive.hpp"
#include "loader.hpp"
#include "module.hpp"

#include <stdint.h>

namespace {

using ps2::archive::align16;
using ps2::archive::hexDigit;
using ps2::archive::Entry;
using ps2::archive::kNameLength;
using ps2::archive::named;
using ps2::archive::packName;
using ps2::archive::terminator;

constexpr int kOpenRead = 1;                    // O_RDONLY [header]
constexpr int kSeekSet = 0;
constexpr int kSeekEnd = 2;

// The boot block's own window, re-scanned here exactly as `IOPBOOT` scans it:
// §"Every named source is opened" -- `rom0` joins the pool whether or not the
// command line mentions it.
constexpr uintptr_t kRomSearchStart = 0xBFC00000;
constexpr uintptr_t kRomSearchEnd = 0xBFC80000;

constexpr uint32_t kSourcesMax = 4;
constexpr uint32_t kNamesMax = 64;

// docs/analysis/51 §1: the buffer opens with the eight-word boot block and a
// 256-word boot list, and the sources are read in behind them.
constexpr uint32_t kBlockSize = 0x20;
constexpr uint32_t kListMax = 256;
constexpr uint32_t kHeaderSize = kBlockSize + kListMax * 4;   // 0x420

// BOOT-8b: the mode the staged kernel is told it booted in.
constexpr uint32_t kModeMerged = 3;

// BOOT-8c's word offsets, as indices into the block.
constexpr uint32_t kBiRamMiB = 0;
constexpr uint32_t kBiMode = 1;
constexpr uint32_t kBiCommandLine = 2;
constexpr uint32_t kBiSysmemBase = 3;
constexpr uint32_t kBiReservedBase = 4;
constexpr uint32_t kBiReservedSize = 5;
constexpr uint32_t kBiListCount = 6;
constexpr uint32_t kBiList = 7;

// ARC-6: an `EXTINFO` record is a 16-bit value, a payload length and a type,
// then that many payload bytes. Type 2's value is the version.
constexpr uint8_t kExtinfoVersion = 0x02;

struct Archive {
    const Entry *table;                         // the ROMDIR table
    uintptr_t base;                             // the archive's first byte
    const uint8_t *extinfo;                     // the EXTINFO file, or null
    uint32_t extinfo_size;
    const char *label;
};

struct Candidate {
    const uint8_t *bytes;
    uint32_t size;
    uint32_t version;
    bool found;
};

Archive sources[kSourcesMax];
uint32_t source_count;

extern "C" {
int _import_ioman_open(const char *path, int flags);
int _import_ioman_close(int fd);
int _import_ioman_read(int fd, void *buffer, int size);
int _import_ioman_lseek(int fd, int offset, int whence);
int _import_sysmem_allocate(int mode, int size, int address);
int _import_sysmem_mem_size();
int _import_intrman_disable(uint32_t *state);
int _import_stdio_printf(const char *format, ...);
}

// ARC-4: a table is found by its `RESET` first entry, and the archive's own
// first byte is that entry's file, which sits immediately below the table.
// The `EXTINFO` file is located in the same walk, because every version this
// module reads comes out of it.
[[nodiscard]] bool openArchive(uintptr_t start, uintptr_t end, const char *label,
                               Archive &out) {
    for (uintptr_t at = start; at + sizeof(Entry) <= end; at += sizeof(Entry)) {
        const auto &first = *reinterpret_cast<const Entry *>(at);
        if (!named(first, packName("RESET")) || (first.size & 15) != 0) {
            continue;
        }
        out.table = &first;
        out.base = at - align16(first.size);
        out.label = label;
        out.extinfo = nullptr;
        out.extinfo_size = 0;
        uintptr_t file = out.base;
        for (const Entry *entry = &first; !terminator(*entry); entry++) {
            if (named(*entry, packName("EXTINFO"))) {
                out.extinfo = reinterpret_cast<const uint8_t *>(file);
                out.extinfo_size = entry->size;
                break;
            }
            file += align16(entry->size);
        }
        return true;
    }
    return false;
}

// ARC-6c: the version an entry declares, or 0 when it declares none.
// `IGREETING` and `SIFINIT` are resources rather than modules and carry no
// version record; the merge rule as read says nothing about them, and every
// such name has exactly one candidate, so answering 0 decides nothing that
// the rule would have decided differently.
[[nodiscard]] uint32_t extinfoVersion(const Archive &archive, uint32_t offset,
                                      uint32_t length) {
    if (archive.extinfo == nullptr || offset + length > archive.extinfo_size) {
        return 0;
    }
    const uint8_t *at = archive.extinfo + offset;
    const uint8_t *end = at + length;
    while (at + 4 <= end) {
        const uint32_t value = static_cast<uint32_t>(at[0]) | (at[1] << 8);
        const uint32_t payload = at[2];
        const uint32_t type = at[3];
        if (type == kExtinfoVersion) {
            return value;
        }
        at += 4 + payload;
    }
    return 0;
}

// One name in one archive: its bytes, its size and the version the merge
// compares. The file offsets and the `EXTINFO` offsets both accumulate over
// the same walk (ARC-3 and ARC-6).
[[nodiscard]] Candidate lookup(const Archive &archive, const char *name) {
    Candidate answer = {nullptr, 0, 0, false};
    uintptr_t file = archive.base;
    uint32_t extinfo_offset = 0;
    for (const Entry *entry = archive.table; !terminator(*entry); entry++) {
        if (named(*entry, name)) {
            answer.bytes = reinterpret_cast<const uint8_t *>(file);
            answer.size = entry->size;
            answer.version = extinfoVersion(archive, extinfo_offset,
                                            entry->extinfo_size);
            answer.found = true;
            return answer;
        }
        file += align16(entry->size);
        extinfo_offset += entry->extinfo_size;
    }
    return answer;
}

// §"Every named source is opened", in two passes, because the buffer they are
// read into cannot be sized until every one of them has been measured. The
// descriptors stay open between the two, which is what the reference does --
// it never closes one at all.
struct NamedSource {
    int fd;
    uint32_t size;
    const char *path;
};

NamedSource named_sources[kSourcesMax];
uint32_t named_count;

// Pass one: open and measure. `ioman` 8 is `lseek`; a source that will not
// open stops the reboot, as the reference's panic does.
[[nodiscard]] bool measureNamedSource(const char *path) {
    if (named_count == kSourcesMax) {
        return false;
    }
    const int fd = _import_ioman_open(path, kOpenRead);
    if (fd < 0) {
        return false;
    }
    const int length = _import_ioman_lseek(fd, 0, kSeekEnd);
    if (length <= 0) {
        _import_ioman_close(fd);
        return false;
    }
    named_sources[named_count].fd = fd;
    named_sources[named_count].size = static_cast<uint32_t>(length);
    named_sources[named_count].path = path;
    named_count++;
    return true;
}

// Pass two: read one in at the cursor and take its table. A source whose
// archive will not open leaves the cursor where it was, so the next one lands
// on top of it -- the reference's behaviour, and the only thing it does about
// a file that is not an archive.
[[nodiscard]] uint32_t readNamedSource(uint32_t index, uint32_t cursor) {
    const NamedSource &source = named_sources[index];
    _import_ioman_lseek(source.fd, 0, kSeekSet);
    const int got = _import_ioman_read(source.fd,
                                       reinterpret_cast<void *>(cursor),
                                       static_cast<int>(source.size));
    if (got != static_cast<int>(source.size)) {
        return cursor;
    }
    if (!openArchive(cursor, cursor + source.size, source.path,
                     sources[source_count])) {
        return cursor;
    }
    source_count++;
    return cursor + align16(source.size);
}

// BOOT-9's grammar, plus the two directives `UDNL`'s parser has and
// `IOPBOOT`'s does not (docs/analysis/51 §4): `!addr <hex>` appends BOOT-8d's
// tagged list entry, and `!include <name>` resolves a nested list and recurses
// into it. Neither retail list carries either, so both are recognised and
// skipped rather than acted on -- this module resolves the merge and stages
// nothing yet, and a tagged entry has no list to go into.
[[nodiscard]] bool directive(const char *text, uint32_t length) {
    return length != 0 && text[0] == '!';
}

// One byte more than a name: `named()` compares exactly ten, and printing
// one wants a terminator a ten-character name would otherwise not have.
char names[kNamesMax][kNameLength + 1];
uint32_t name_count;

// BOOT-9's `@` token, which is where the staged `SYSMEM`'s record goes: the
// block's `+0x0c` starts as this and ends as where `SYSMEM` actually landed.
uint32_t list_base;

// The order comes from the first `IOPBTCONF` found walking the sources newest
// first -- the last opened backwards to `rom0` -- which in the retail case
// makes `rom0` the one that answers, a title's image carrying none.
[[nodiscard]] const Archive *resolveOrder() {
    for (uint32_t k = source_count; k-- > 0;) {
        const Archive &archive = sources[k];
        const Candidate list = lookup(archive, "IOPBTCONF");
        if (!list.found) {
            continue;
        }
        const auto *text = reinterpret_cast<const char *>(list.bytes);
        name_count = 0;
        list_base = 0;
        uint32_t i = 0;
        while (i < list.size && name_count < kNamesMax) {
            while (i < list.size && text[i] < 0x20) {
                i++;
            }
            const uint32_t token = i;
            while (i < list.size && text[i] >= 0x20) {
                i++;
            }
            const uint32_t length = i - token;
            if (length == 0) {
                break;
            }
            if (text[token] == '@') {
                list_base = 0;
                for (uint32_t c = 1; c < length; c++) {
                    list_base = (list_base << 4) | hexDigit(static_cast<uint8_t>(text[token + c]));
                }
                continue;
            }
            if (text[token] == '#') {
                continue;
            }
            if (directive(&text[token], length)) {
                // `!addr ` and `!include ` each take a hex argument, which
                // whitespace splitting leaves as the next token; skipping it
                // keeps it from being read as a module name.
                while (i < list.size && text[i] < 0x20) {
                    i++;
                }
                while (i < list.size && text[i] >= 0x20) {
                    i++;
                }
                continue;
            }
            for (uint32_t c = 0; c <= kNameLength; c++) {
                names[name_count][c] = c < length ? text[token + c] : '\0';
            }
            name_count++;
        }
        return &archive;
    }
    return nullptr;
}

}  // namespace

PS2_IMPORTS_BEGIN("ioman\0\0\0", 0x0102)
PS2_IMPORT(_import_ioman_open, 4)
PS2_IMPORT(_import_ioman_close, 5)
PS2_IMPORT(_import_ioman_read, 6)
PS2_IMPORT(_import_ioman_lseek, 8)
PS2_IMPORTS_END()

PS2_IMPORTS_BEGIN("sysmem\0\0", 0x0101)
PS2_IMPORT(_import_sysmem_allocate, 4)
PS2_IMPORT(_import_sysmem_mem_size, 6)
PS2_IMPORTS_END()

PS2_IMPORTS_BEGIN("intrman\0", 0x0102)
PS2_IMPORT(_import_intrman_disable, 8)
PS2_IMPORTS_END()

PS2_IMPORTS_BEGIN("stdio\0\0\0", 0x0102)
PS2_IMPORT(_import_stdio_printf, 4)
PS2_IMPORTS_END()

extern "C" {

// IRX-12: entry(argc, argv, 0, record). `argv[0]` is this module's own
// invocation path and `argv[1..]` the rest of the reboot's command line.
int _module_start(int argc, char **argv) {
    // `rom0` is index 0 because it is the oldest source, and every walk over
    // the pool runs from the last index down (docs/analysis/51 §4): the
    // newest source answers first, and a tie leaves it standing.
    source_count = 0;
    if (!openArchive(kRomSearchStart, kRomSearchEnd, "rom0",
                     sources[source_count])) {
        _import_stdio_printf("udnl: panic ! 'rom0' not found\n");
        return 1;
    }
    source_count++;

    // §1: measure every named source first, because the buffer holds all of
    // them and its size is their sizes -- taken before a single name is
    // resolved, so it has nothing to do with which modules win.
    named_count = 0;
    uint32_t total = kHeaderSize;
    for (int k = 1; k < argc && argv[k] != nullptr; k++) {
        if (!measureNamedSource(argv[k])) {
            _import_stdio_printf("udnl: file '%s' can't open\n", argv[k]);
            return 1;                           // not resident
        }
        total += align16(named_sources[named_count - 1].size);
    }

    // §2: one allocation. The reference walks the block records with `sysmem`
    // 9 and 10 and takes the first free block above its own image with mode
    // 2; ours has neither ordinal, and mode 0 grows upward from the last
    // thing allocated, which is the same place.
    const int block_address = _import_sysmem_allocate(0, static_cast<int>(total), 0);
    if (block_address == 0) {
        _import_stdio_printf("udnl: pannic ! can not alloc memory\n");
        return 1;
    }
    const auto buffer = static_cast<uint32_t>(block_address);
    auto *block = reinterpret_cast<volatile uint32_t *>(buffer);
    for (uint32_t k = 0; k < kBlockSize / 4; k++) {
        block[k] = 0;
    }

    // §3: read each source in behind the header, in the order the command
    // line named them, and take its table where it now lies.
    uint32_t cursor = buffer + kHeaderSize;
    for (uint32_t k = 0; k < named_count; k++) {
        cursor = readNamedSource(k, cursor);
    }

    const Archive *order = resolveOrder();
    if (order == nullptr) {
        _import_stdio_printf("udnl: panic ! 'IOPBTCONF' not found\n");
        return 1;
    }
    _import_stdio_printf("udnl: order from %s (%d names)\n", order->label,
                         name_count);

    // §4: the list is one word per name, the address of the winning module's
    // bytes -- in the buffer for a source a title supplied, in the ROM window
    // for one `rom0` won. Nothing is copied.
    auto *list = reinterpret_cast<volatile uint32_t *>(buffer + kBlockSize);
    uint32_t list_count = 0;
    for (uint32_t n = 0; n < name_count && list_count + 1 < kListMax; n++) {
        const Archive *winner = nullptr;
        Candidate best = {nullptr, 0, 0, false};
        for (uint32_t k = source_count; k-- > 0;) {
            const Candidate candidate = lookup(sources[k], names[n]);
            if (!candidate.found) {
                continue;
            }
            if (winner == nullptr || candidate.version > best.version) {
                winner = &sources[k];
                best = candidate;
            }
        }
        if (winner == nullptr) {
            // The reference stops here without a word. Saying which name it
            // was costs one line and is the difference between a diagnosable
            // reboot and a dead one.
            _import_stdio_printf("udnl: panic ! '%s' not found\n", names[n]);
            return 1;
        }
        _import_stdio_printf("udnl:   %-10s %x.%02x <- %s\n", names[n],
                             best.version >> 8, best.version & 0xFF,
                             winner->label);
        list[list_count++] = reinterpret_cast<uint32_t>(best.bytes);
    }
    list[list_count] = 0;                       // BOOT-8d's terminator

    // §5: the block. The reserved region is the images themselves, because
    // the staged kernel loads its modules out of them and must not allocate
    // over them while it does.
    const auto ram_size = static_cast<uint32_t>(_import_sysmem_mem_size());
    const uint32_t reserved = (buffer + kHeaderSize) & ~uint32_t{0xFF};
    block[kBiRamMiB] = ram_size >> 20;
    block[kBiMode] = kModeMerged;
    block[kBiCommandLine] = 0;
    block[kBiSysmemBase] = list_base;
    block[kBiReservedBase] = reserved;
    block[kBiReservedSize] = cursor - reserved + 0x100;
    block[kBiListCount] = list_count;
    block[kBiList] = buffer + kBlockSize;

    // §5: the hand-over. From here the kernel that is running is being
    // overwritten, so nothing may be called that belongs to it.
    uint32_t state = 0;
    (void)_import_intrman_disable(&state);

    const ps2::loader::Loaded sysmem =
        ps2::loader::placeModule(list[0], list_base);
    if (sysmem.next_base == 0) {
        for (;;) {
        }
    }
    // BOOT-8c: RAM in bytes, and the answer is where `LOADCORE`'s record
    // goes. Neither module is bound and neither is registered here: this
    // registry belongs to the kernel being replaced, and the staged
    // `LOADCORE` builds its own from the block.
    const uint32_t placement = ps2::loader::callEntry(
        sysmem.entry, sysmem.gp, ram_size, nullptr, sysmem.record);
    const ps2::loader::Loaded loadcore = ps2::loader::placeModule(
        list[1], placement != 0 ? placement : sysmem.next_base);
    if (loadcore.next_base == 0) {
        for (;;) {
        }
    }
    block[kBiSysmemBase] = list_base + ps2::loader::kRecordSize;
    (void)ps2::loader::callEntry(loadcore.entry, loadcore.gp, buffer, nullptr,
                                 loadcore.record);
    for (;;) {                                  // it does not come back
    }
}

}  // extern "C"
