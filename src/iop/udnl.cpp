// UDNL: the merge core a reboot's command line names.
//
// docs/analysis/45-iop-reboot.md §"UDNL: the merge core". A title asks for a
// reboot with `"rom0:UDNL cdrom0:\MODULES\IOPRP310.IMG;1"`; `MODLOAD`'s
// mode-2 bootup callback loads this module with the rest of that line as its
// arguments, and what it does is decide, name by name, which copy of each
// kernel module the next kernel is to be built from.
//
// **The staging and the hand-over are not built.** This module resolves the
// merge and says what it resolved; it does not copy the winners into a
// contiguous image, and it does not enter one. Those two steps were also the
// two the reference reading did not reach, so they are a reconstruction
// rather than a transcription, and they are worth doing against a merge
// already known to be right. Until they exist a reboot that names an image
// comes back with the modules `rom0` holds -- which is what it already did.
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
#include "module.hpp"

#include <stdint.h>

namespace {

using ps2::archive::align16;
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

// §"Every named source is opened": `ioman` 4 opens it and `ioman` 8 measures
// it; a source that will not open stops the reboot, as the reference's panic
// does. The whole file is read in, because the merge needs to walk its table
// and then copy out of it.
[[nodiscard]] bool openNamedSource(const char *path) {
    if (source_count == kSourcesMax) {
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
    const int block = _import_sysmem_allocate(1, length, 0);
    if (block == 0) {
        _import_ioman_close(fd);
        return false;
    }
    _import_ioman_lseek(fd, 0, kSeekSet);
    const int got = _import_ioman_read(fd, reinterpret_cast<void *>(block), length);
    _import_ioman_close(fd);
    if (got != length) {
        return false;
    }
    const auto start = static_cast<uintptr_t>(block);
    return openArchive(start, start + static_cast<uint32_t>(length), path,
                       sources[source_count++]);
}

// BOOT-9's grammar, plus the two directives `UDNL`'s parser has and
// `IOPBOOT`'s does not (§"IOPBTCONF is resolved by name"): `!addr ` stores a
// base of its own and `!include ` appends a tagged entry. Neither retail list
// carries either, so both are recognised and skipped rather than acted on --
// acting on them would be inventing a consumer this project has not read.
[[nodiscard]] bool directive(const char *text, uint32_t length) {
    return length != 0 && text[0] == '!';
}

// One byte more than a name: `named()` compares exactly ten, and printing
// one wants a terminator a ten-character name would otherwise not have.
char names[kNamesMax][kNameLength + 1];
uint32_t name_count;

// The order comes from the first `IOPBTCONF` found walking the sources newest
// first, which puts `rom0` last -- and in the retail case makes it the one
// that answers, a title's image carrying none.
[[nodiscard]] const Archive *resolveOrder() {
    for (uint32_t k = 0; k < source_count; k++) {
        const Archive &archive = sources[k];
        const Candidate list = lookup(archive, "IOPBTCONF");
        if (!list.found) {
            continue;
        }
        const auto *text = reinterpret_cast<const char *>(list.bytes);
        name_count = 0;
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
            if (text[token] == '@' || text[token] == '#') {
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
PS2_IMPORTS_END()

PS2_IMPORTS_BEGIN("stdio\0\0\0", 0x0102)
PS2_IMPORT(_import_stdio_printf, 4)
PS2_IMPORTS_END()

extern "C" {

// IRX-12: entry(argc, argv, 0, record). `argv[0]` is this module's own
// invocation path and `argv[1..]` the rest of the reboot's command line.
int _module_start(int argc, char **argv) {
    source_count = 0;
    for (int k = 1; k < argc && argv[k] != nullptr; k++) {
        if (!openNamedSource(argv[k])) {
            _import_stdio_printf("udnl: file '%s' can't open\n", argv[k]);
            return 1;                           // not resident
        }
    }
    if (!openArchive(kRomSearchStart, kRomSearchEnd, "rom0",
                     sources[source_count])) {
        _import_stdio_printf("udnl: panic ! 'rom0' not found\n");
        return 1;
    }
    source_count++;

    const Archive *order = resolveOrder();
    if (order == nullptr) {
        _import_stdio_printf("udnl: panic ! 'IOPBTCONF' not found\n");
        return 1;
    }
    _import_stdio_printf("udnl: order from %s (%d names)\n", order->label,
                         name_count);

    for (uint32_t n = 0; n < name_count; n++) {
        const Archive *winner = nullptr;
        Candidate best = {nullptr, 0, 0, false};
        for (uint32_t k = 0; k < source_count; k++) {
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
            _import_stdio_printf("udnl:   %-10s -- missing\n", names[n]);
            continue;
        }
        _import_stdio_printf("udnl:   %-10s %x.%02x <- %s\n", names[n],
                             best.version >> 8, best.version & 0xFF,
                             winner->label);
    }

    // The staging copy and the hand-over belong here. Until they do, the
    // boot goes on with the modules it already loaded.
    _import_stdio_printf("udnl: merge resolved; nothing staged\n");
    return 1;                                   // not resident: nothing to keep
}

}  // extern "C"
