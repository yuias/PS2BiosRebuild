// EESYNC: the last entry on the boot list, and the file service the kernel
// asks its questions of.
//
// docs/spec/03-boot-chain.md BOOT-10 and BOOT-12b. Being last is this
// module's function: the EE is told the IOP is listening only once every
// service that could answer a packet exists, so the bit goes up here rather
// than in `SIFCMD`, which raises none. The reference's `EESYNC` is the same
// shape -- the smallest module on the list that still exports something, and
// `sifman` 24 is one of the four ordinals it imports (docs/analysis/12).
//
// The file command is ours, not the SDK's: the kernel asks for a file out of
// the archive and this answers with its size or its bytes
// (docs/implementation.md). The reference serves `rom0:` through `ROMDRV`
// over RPC, which does not exist here yet. It is reached the way any module's
// own command is, through `sceSifAddCmdHandler`.

#include "module.hpp"
#include "sif.hpp"

#include <stdint.h>

namespace {

using ps2::sif::Transfer;

// What the boot block left for us, at the address `src/boot/iopboot.S` fixes.
constexpr uintptr_t kBootList = 0x001F8100;
constexpr uintptr_t kBootListTable = 0x008;

constexpr uint32_t kNameLength = 10;            // spec/01 ARC-2
constexpr uint32_t kPayloadWords = 4096;        // up to 16 KiB per answer
constexpr uint32_t kVerbSize = 0;               // how big is it?

// The command id the kernel sends; `src/kernel/sif.cpp` names the same number.
constexpr uint32_t kCidFile = 0x10;

// spec/01 ARC-2: a table entry.
struct RomdirEntry {
    char name[kNameLength];
    uint16_t extinfo_size;
    uint32_t size;
};

struct File {
    const uint8_t *bytes;   // nullptr when there is no such file
    uint32_t size;
};

// The kernel's file request, as the EE lays it out after the 16-byte command
// header (`src/kernel/sif.cpp`): eight words.
struct Request {
    char name[kNameLength];
    uint16_t pad;
    uint32_t verb;
    uint32_t destination;   // in EE memory, physical
    uint32_t offset;        // the content verb: first byte wanted
    uint32_t length;        // the content verb: how many, 0 for as many as fit
    uint32_t reserved;
};
static_assert(sizeof(Request) == 32);

constexpr uint32_t kHeaderBytes = 16;           // BOOT-12a

alignas(16) uint32_t payload[kPayloadWords];

extern "C" {
int _import_sifcmd_add_cmd_handler(uint32_t cid, void *function, void *arg);
int _import_sifman_set_dma(const Transfer *list, uint32_t count);
int _import_sifman_set_smflag(uint32_t bits);
}

[[nodiscard]] uint32_t alignedSize(uint32_t size) {
    return (size + 15) & ~uint32_t{15};
}

// Walk the archive's table, which the boot block left at kBootList +
// kBootListTable, accumulating aligned sizes the way ARC-3 says offsets are
// implied rather than stored. Entry 0 is RESET and the table follows it, so
// the image base is the table less that file's size.
[[nodiscard]] File lookup(const char *name) {
    const auto *entry = *reinterpret_cast<const RomdirEntry *const *>(
        kBootList + kBootListTable);
    if (entry == nullptr) {
        return {nullptr, 0};
    }
    const uint8_t *at = reinterpret_cast<const uint8_t *>(entry)
                        - alignedSize(entry->size);
    for (;; entry++) {
        if (entry->name[0] == '\0' && entry->extinfo_size == 0
            && entry->size == 0) {
            return {nullptr, 0};
        }
        bool same = true;
        for (uint32_t k = 0; k < kNameLength && same; k++) {
            same = entry->name[k] == name[k];
        }
        if (same) {
            return {at, entry->size};
        }
        at += alignedSize(entry->size);
    }
}

// Copy a window of the file into the payload, and say how many words to send.
[[nodiscard]] uint32_t fillContent(const File &file, uint32_t offset,
                                   uint32_t length) {
    if (file.bytes == nullptr || offset >= file.size) {
        payload[0] = 0;
        return 4;
    }
    uint32_t available = file.size - offset;
    if (length == 0 || length > available) {
        length = available;
    }
    if (length > kPayloadWords * 4) {
        length = kPayloadWords * 4;
    }
    const uint8_t *from = file.bytes + offset;
    auto *to = reinterpret_cast<uint8_t *>(payload);
    // A word copy when the window is word-aligned in the ROM: it is what the
    // loader asks for, and it is four times fewer instructions on a simulator
    // that runs each one in turn.
    if ((reinterpret_cast<uintptr_t>(from) & 3) == 0) {
        const auto *fw = reinterpret_cast<const uint32_t *>(from);
        for (uint32_t k = 0; k < length / 4; k++) {
            payload[k] = fw[k];
        }
        for (uint32_t k = length & ~uint32_t{3}; k < length; k++) {
            to[k] = from[k];
        }
    } else {
        for (uint32_t k = 0; k < length; k++) {
            to[k] = from[k];
        }
    }
    return (length + 3) / 4;
}

// The kernel's file request, served in the receiving channel's handler. The
// answer is one run that ends the EE's channel, so the asker's own handler
// runs once all of it is there.
void serveFile(void *packet, void *) {
    const auto &request = *reinterpret_cast<const Request *>(
        static_cast<uint8_t *>(packet) + kHeaderBytes);
    const File file = lookup(request.name);
    uint32_t words;
    if (request.verb == kVerbSize) {
        // One quadword, so the asker knows how large the file is before
        // asking for its bytes.
        payload[0] = file.size;
        payload[1] = 0;
        payload[2] = 0;
        payload[3] = 0;
        words = 4;
    } else {
        words = fillContent(file, request.offset, request.length);
    }
    Transfer transfer;
    transfer.src = reinterpret_cast<uintptr_t>(payload);
    transfer.dest = request.destination;
    transfer.size = words * 4;
    transfer.attr = ps2::sif::kAttrEndEe;
    (void)_import_sifman_set_dma(&transfer, 1);
}

}  // namespace

PS2_IMPORTS_BEGIN("sifcmd\0\0", 0x0101)
PS2_IMPORT(_import_sifcmd_add_cmd_handler, 10)
PS2_IMPORTS_END()

PS2_IMPORTS_BEGIN("sifman\0\0", 0x0101)
PS2_IMPORT(_import_sifman_set_dma, 7)
PS2_IMPORT(_import_sifman_set_smflag, 24)
PS2_IMPORTS_END()

extern "C" {

// The module's entry, called by the loader as entry(argc, argv, 0, record)
// (spec/02 IRX-10).
int _module_start(int, char **) {
    (void)_import_sifcmd_add_cmd_handler(kCidFile,
                                         reinterpret_cast<void *>(serveFile),
                                         nullptr);

    // BOOT-12b: the boot list is done and every server is registered, so the
    // EE may start sending. A write to SMFLG from this side sets (BOOT-10c).
    (void)_import_sifman_set_smflag(ps2::sif::kFlagCmdInit);

    return 0;                                    // resident
}

}  // extern "C"
