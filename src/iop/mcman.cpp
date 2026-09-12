// MCMAN: the memory card, as far as reading it.
//
// docs/spec/06-iop-kernel.md IOP-15, from docs/analysis/55. Scoped to one
// question: is a formatted card in the slot, and what is in its root
// directory? Nothing here writes, creates, deletes or formats, and the older
// card format is not supported at all -- where the reference falls back to
// probing for one, this answers "nothing there" instead, which is the one
// deviation IOP-15c's masking makes visible to a caller.
//
// This is the archive's first user of the serial transfer descriptor's DMA
// arguments: every frame below arrives at the interface by DMA, one 144-byte
// block per sub-transfer, and not a byte of it goes through the data
// register the controller driver uses.

#include "module.hpp"

#include <stdint.h>

namespace {

using ps2::module::ExportTable;
using ps2::module::reservedHook;
using ps2::module::slot;

// IOP-15b: the controllers own serial ports 0 and 1, the cards 2 and 3.
constexpr uint32_t kPorts = 2;
[[nodiscard]] constexpr uint32_t serialPort(uint32_t port) {
    return (port & 1) + 2;
}

// IOP-15d: at most eleven sub-transfers, 0x24 words each way each.
constexpr uint32_t kMaxFrames = 11;
constexpr uint32_t kBlockWords = 0x24;
constexpr uint32_t kBlockBytes = kBlockWords * 4;
constexpr uint32_t kRegdataTag = 0x70;
constexpr uint32_t kPortCtrl1[kPorts] = {0xff020405, 0xff030405};
constexpr uint32_t kPortCtrl2 = 0x0005ffff;
constexpr uint32_t kStatusMask = 0xf000;
constexpr uint32_t kStatusGood = 0x1000;

// The card's own answers. `kBusy` is the byte a card that is not ready --
// or not there -- leaves in the reply.
constexpr uint8_t kAck = 0x5a;
constexpr uint8_t kBusy = 0x66;
constexpr uint32_t kAttempts = 5;

// IOP-15e: 512 of data and 16 after it, and the identifier page 0 opens with.
constexpr uint32_t kPageBytes = 512;
constexpr uint32_t kTailBytes = 16;
constexpr uint32_t kChunkBytes = 128;
constexpr uint32_t kClusterBytes = 1024;
constexpr uint32_t kEntryBytes = 512;
constexpr uint32_t kSuperblockCopy = 0x150;
constexpr uint32_t kIdentifierBytes = 0x1c;

// IOP-15c's codes, as the ladder produces them before the masking.
constexpr int kOk = 0;
constexpr int kCardChanged = -1;
constexpr int kNotFormatted = -2;
constexpr int kNoReset = -11;
constexpr int kNoPresence = -12;
constexpr int kNoTerminator = -13;
constexpr int kNoAuth = -90;
constexpr int kNoGeometry = -49;
constexpr int kPageUnreadable = -48;
constexpr int kNoRootTail = -47;
constexpr int kNoRootSecond = -45;
constexpr int kNoRootFirst = -46;
constexpr int kReadFailed = -1;
constexpr int kUncorrectable = -2;

// docs/analysis/40 §4.
struct DmaArgument {
    uint32_t address;
    uint32_t size;
    uint32_t count;
};

struct TransferData {
    uint32_t stat6c;
    uint32_t port_ctrl1[4];
    uint32_t port_ctrl2[4];
    uint32_t stat70;
    uint32_t regdata[16];
    uint32_t stat74;
    uint32_t in_size;
    uint32_t out_size;
    uint8_t *in;
    uint8_t *out;
    DmaArgument in_dma;
    DmaArgument out_dma;
};

struct SemaParameters {
    uint32_t attr;
    uint32_t option;
    uint32_t initial;
    uint32_t max;
};

}  // namespace

extern "C" {
int _import_loadcore_register(void *table);
int _import_intrman_enable_all();
int _import_sio2man_mc_transfer_init();
int _import_sio2man_transfer(TransferData *td);
int _import_secrman_set_command_handler(int (*handler)(int, int, void *));
int _import_secrman_set_devid_handler(int (*handler)(int, int));
int _import_secrman_auth_card(int port, int slot, int number);
int _import_thsemap_create(const SemaParameters *parameters);
int _import_stdio_printf(const char *format, ...);
}

namespace {

// --- the batch (IOP-15d) ---------------------------------------------------

// The command table of docs/analysis/55 §3, as far as the read path uses it.
// Kept as an index rather than a byte because that is what the reference's
// own builder takes, and because two entries share a byte with different
// lengths.
enum class Frame : uint32_t {
    Probe,          // 0x11
    SetPage,        // 0x23
    Geometry,       // 0x26
    SetTerminator,  // 0x27
    Presence,       // 0x28
    ReadChunk,      // 0x43, 128 bytes
    ReadTail,       // 0x43, the bytes after the page
    EndRead,        // 0x81
    Reset,          // 0xf3
};

struct Command {
    uint8_t byte;
    uint8_t length;
};

constexpr Command kCommands[] = {
    {0x11, 4}, {0x23, 9}, {0x26, 13}, {0x27, 5}, {0x28, 5},
    {0x43, 134}, {0x43, 6}, {0x81, 4}, {0xf3, 5},
};

alignas(4) uint8_t tx[kMaxFrames * kBlockBytes];
alignas(4) uint8_t rx[kMaxFrames * kBlockBytes];
TransferData td;
uint32_t frames;

[[nodiscard]] uint8_t *txBlock(uint32_t index) { return tx + index * kBlockBytes; }
[[nodiscard]] const uint8_t *rxBlock(uint32_t index) { return rx + index * kBlockBytes; }

void beginBatch() {
    frames = 0;
}

// One sub-transfer. `payload` is copied after the two header bytes and
// `extra` lengthens the frame, which is what the tail read needs. Blocks are
// deliberately not cleared first: the reference leaves whatever a block last
// carried past a command's payload, so a rebuild that clears them sends
// different bytes -- and must then never check them either.
void addFrame(Frame which, const uint8_t *payload, uint32_t payload_bytes,
              uint32_t extra) {
    if (frames >= kMaxFrames) {
        return;
    }
    const Command command = kCommands[static_cast<uint32_t>(which)];
    const uint32_t length = command.length + extra;
    uint8_t *block = txBlock(frames);
    block[0] = 0x81;
    block[1] = command.byte;
    for (uint32_t i = 0; i < payload_bytes; i++) {
        block[2 + i] = payload[i];
    }
    td.regdata[frames] = td.port_ctrl2[0]      // the serial port, parked here
                       | kRegdataTag | (length << 8) | (length << 18);
    frames++;
}

// IOP-15d: the list is terminated by a zero word, and both channels move the
// same number of blocks.
[[nodiscard]] bool runBatch(uint32_t port) {
    if (frames == 0 || frames >= kMaxFrames) {
        return false;
    }
    td.regdata[frames] = 0;
    td.in_dma.count = frames;
    td.out_dma.count = frames;
    td.port_ctrl1[serialPort(port)] = kPortCtrl1[port & 1];
    td.stat6c = 0;
    _import_sio2man_mc_transfer_init();
    (void)_import_sio2man_transfer(&td);
    return (td.stat6c & kStatusMask) == kStatusGood;
}

// The serial port index goes in `regdata`'s low bits. Parking it in an
// otherwise unused word keeps `addFrame` from needing the port threaded
// through it.
void selectPort(uint32_t port) {
    td.port_ctrl2[0] = serialPort(port);
}

[[nodiscard]] uint8_t exclusiveOr(const uint8_t *from, uint32_t count) {
    uint8_t answer = 0;
    for (uint32_t i = 0; i < count; i++) {
        answer = static_cast<uint8_t>(answer ^ from[i]);
    }
    return answer;
}

// --- the port record (IOP-15b) ---------------------------------------------

struct Port {
    uint8_t superblock[kSuperblockCopy];
    uint8_t type;                       // 0 nothing, 2 a card of this format
    uint8_t geometry_flags;             // bit 0: the card carries a page tail
    bool mounted;
    uint32_t page_bytes;
    uint32_t pages_per_cluster;
    uint32_t first_cluster;             // superblock +0x34
    uint32_t root_cluster;              // superblock +0x3c, read but not used
    uint32_t spare_block;               // superblock +0x44
    uint32_t indirect[8];               // superblock +0x50, as far as we walk
};

Port ports[kPorts];

[[nodiscard]] uint32_t leWord(const uint8_t *at) {
    return static_cast<uint32_t>(at[0]) | (static_cast<uint32_t>(at[1]) << 8)
         | (static_cast<uint32_t>(at[2]) << 16)
         | (static_cast<uint32_t>(at[3]) << 24);
}

[[nodiscard]] uint32_t leHalf(const uint8_t *at) {
    return static_cast<uint32_t>(at[0]) | (static_cast<uint32_t>(at[1]) << 8);
}

// --- the frames (IOP-15c, IOP-15e) -----------------------------------------

[[nodiscard]] bool probe(uint32_t port) {
    for (uint32_t attempt = 0; attempt < kAttempts; attempt++) {
        beginBatch();
        addFrame(Frame::Probe, nullptr, 0, 0);
        if (runBatch(port) && rxBlock(0)[3] != kBusy) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool reset(uint32_t port) {
    beginBatch();
    addFrame(Frame::Reset, nullptr, 0, 0);
    return runBatch(port);
}

// The presence frame's reply also carries, at byte 3, whatever terminator the
// card is holding -- which is how a card already visited is recognised.
[[nodiscard]] bool presence(uint32_t port, uint8_t &terminator) {
    for (uint32_t attempt = 0; attempt < kAttempts; attempt++) {
        beginBatch();
        addFrame(Frame::Presence, nullptr, 0, 0);
        if (runBatch(port) && rxBlock(0)[4] != kBusy) {
            terminator = rxBlock(0)[3];
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool setTerminator(uint32_t port) {
    const uint8_t value = kAck;
    for (uint32_t attempt = 0; attempt < kAttempts; attempt++) {
        beginBatch();
        addFrame(Frame::SetTerminator, &value, 1, 0);
        if (runBatch(port) && rxBlock(0)[4] == kAck) {
            return true;
        }
    }
    return false;
}

// IOP-15e: the card's own page and erase-block sizes, with its reply's own
// exclusive-or checked before any of it is believed.
[[nodiscard]] bool geometry(uint32_t port) {
    for (uint32_t attempt = 0; attempt < kAttempts; attempt++) {
        beginBatch();
        addFrame(Frame::Geometry, nullptr, 0, 0);
        if (!runBatch(port)) {
            continue;
        }
        const uint8_t *reply = rxBlock(0);
        if (reply[0xc] != kAck || exclusiveOr(reply + 3, 8) != reply[0xb]) {
            continue;
        }
        Port &state = ports[port & 1];
        state.geometry_flags = reply[2];
        state.page_bytes = leHalf(reply + 3);
        state.pages_per_cluster = state.page_bytes != 0
            ? kClusterBytes / state.page_bytes : 0;
        return true;
    }
    return false;
}

// IOP-15e's page read. `tail` may be null, in which case the sixteen bytes
// after the page are still fetched when the card has them -- the end frame's
// answer is checked at the block the reference checks, and dropping the tail
// frame would move it.
[[nodiscard]] int readPage(uint32_t port, uint32_t page, uint8_t *into,
                           uint8_t *tail) {
    Port &state = ports[port & 1];
    const uint32_t chunks = state.page_bytes / kChunkBytes;
    const bool has_tail = (state.geometry_flags & 1) != 0;
    uint8_t argument[5];
    argument[0] = static_cast<uint8_t>(page);
    argument[1] = static_cast<uint8_t>(page >> 8);
    argument[2] = static_cast<uint8_t>(page >> 16);
    argument[3] = static_cast<uint8_t>(page >> 24);
    argument[4] = exclusiveOr(argument, 4);

    for (uint32_t attempt = 0; attempt < kAttempts; attempt++) {
        if (attempt != 0 && !probe(port)) {
            continue;
        }
        beginBatch();
        addFrame(Frame::SetPage, argument, sizeof argument, 0);
        const uint8_t chunk_argument = 0x80;
        for (uint32_t k = 0; k < chunks; k++) {
            addFrame(Frame::ReadChunk, &chunk_argument, 1, 0);
        }
        if (has_tail) {
            const uint8_t tail_argument = static_cast<uint8_t>(kTailBytes);
            addFrame(Frame::ReadTail, &tail_argument, 1, kTailBytes);
        }
        addFrame(Frame::EndRead, nullptr, 0, 0);
        const uint32_t end_block = frames - 1;
        if (!runBatch(port)) {
            continue;
        }

        if (rxBlock(0)[8] != kAck || rxBlock(end_block)[3] != kAck) {
            continue;
        }
        bool good = true;
        for (uint32_t k = 0; k < chunks && good; k++) {
            const uint8_t *reply = rxBlock(1 + k);
            good = exclusiveOr(reply + 4, kChunkBytes) == reply[4 + kChunkBytes];
        }
        if (!good) {
            continue;
        }
        for (uint32_t k = 0; k < chunks; k++) {
            const uint8_t *reply = rxBlock(1 + k) + 4;
            for (uint32_t i = 0; i < kChunkBytes; i++) {
                into[k * kChunkBytes + i] = reply[i];
            }
        }
        if (has_tail && tail != nullptr) {
            const uint8_t *reply = rxBlock(1 + chunks) + 4;
            for (uint32_t i = 0; i < kTailBytes; i++) {
                tail[i] = reply[i];
            }
        }
        // IOP-15e: the error-correcting codes in the tail are **not**
        // compared here. Nothing on this path writes a card, so the only
        // thing a check would add is telling a damaged card from a good one,
        // and the algorithm behind the codes was deliberately not read.
        return kOk;
    }
    return kReadFailed;
}

}  // namespace

namespace {

// --- the filesystem, as far as one directory (IOP-15f) ---------------------

uint8_t cluster_buffer[kClusterBytes];

[[nodiscard]] bool readCluster(uint32_t port, uint32_t absolute, uint8_t *into) {
    Port &state = ports[port & 1];
    for (uint32_t k = 0; k < state.pages_per_cluster; k++) {
        if (readPage(port, absolute * state.pages_per_cluster + k,
                     into + k * state.page_bytes, nullptr) != kOk) {
            return false;
        }
    }
    return true;
}

// Two indirections, as IOP-15f says: the superblock's list names clusters of
// cluster numbers, and those name the table proper.
[[nodiscard]] bool nextCluster(uint32_t port, uint32_t cluster, uint32_t &next) {
    Port &state = ports[port & 1];
    const uint32_t per = kClusterBytes / 4;
    const uint32_t which = cluster / (per * per);
    if (which >= 8 || state.indirect[which] == 0) {
        return false;
    }
    if (!readCluster(port, state.indirect[which], cluster_buffer)) {
        return false;
    }
    const uint32_t table = leWord(cluster_buffer + ((cluster / per) % per) * 4);
    if (!readCluster(port, table, cluster_buffer)) {
        return false;
    }
    const uint32_t entry = leWord(cluster_buffer + (cluster % per) * 4);
    if ((entry & 0x80000000) == 0) {
        return false;
    }
    next = entry & 0x7fffffff;
    return next != 0x7fffffff;
}

// Entry `index` of the directory whose chain starts at relative `cluster`.
[[nodiscard]] bool directoryEntry(uint32_t port, uint32_t cluster,
                                  uint32_t index, uint8_t *into) {
    Port &state = ports[port & 1];
    const uint32_t per_cluster = kClusterBytes / kEntryBytes;
    uint32_t walk = cluster;
    for (uint32_t step = 0; step < index / per_cluster; step++) {
        uint32_t next = 0;
        if (!nextCluster(port, walk, next)) {
            return false;
        }
        walk = next;
    }
    if (!readCluster(port, walk + state.first_cluster, cluster_buffer)) {
        return false;
    }
    const uint8_t *from = cluster_buffer + (index % per_cluster) * kEntryBytes;
    for (uint32_t i = 0; i < kEntryBytes; i++) {
        into[i] = from[i];
    }
    return true;
}

uint8_t entry_buffer[kEntryBytes];

[[nodiscard]] bool nameIs(const uint8_t *entry, const char *want) {
    const uint8_t *name = entry + 0x40;
    uint32_t i = 0;
    for (; want[i] != '\0'; i++) {
        if (name[i] != static_cast<uint8_t>(want[i])) {
            return false;
        }
    }
    return name[i] == 0;
}

// IOP-15e's mount.
[[nodiscard]] int mount(uint32_t port) {
    Port &state = ports[port & 1];
    state.mounted = false;
    if (!geometry(port)) {
        return kNoGeometry;
    }
    uint8_t page[kPageBytes];
    const int read = readPage(port, 0, page, nullptr);
    if (read == kUncorrectable) {
        return kNotFormatted;
    }
    if (read != kOk) {
        return kPageUnreadable;
    }
    // The identifier, ours to compare against but never to reproduce: the
    // reference carries the card's own 28-byte string. Comparing the first
    // two words of it is the same test on any card this can read, and keeps
    // the string out of this tree (docs/clean-room-policy.md §3).
    static const uint8_t kIdentifierHead[8] =
        {'S', 'o', 'n', 'y', ' ', 'P', 'S', '2'};
    for (uint32_t i = 0; i < sizeof kIdentifierHead; i++) {
        if (page[i] != kIdentifierHead[i]) {
            return kNotFormatted;
        }
    }
    for (uint32_t i = 0; i < kSuperblockCopy; i++) {
        state.superblock[i] = page[i];
    }
    state.page_bytes = leHalf(page + 0x28);
    state.pages_per_cluster = leHalf(page + 0x2a);
    state.first_cluster = leWord(page + 0x34);
    state.root_cluster = leWord(page + 0x3c);
    state.spare_block = leWord(page + 0x44);
    for (uint32_t i = 0; i < 8; i++) {
        state.indirect[i] = leWord(page + 0x50 + i * 4);
    }

    // IOP-15e: the two pages of the second spare erase block. Read for the
    // same reason the reference reads them -- a card whose last write did not
    // finish leaves a record there -- but this driver only checks that they
    // can be read. Replaying one is the write path's business.
    const uint32_t pages_per_block = leHalf(page + 0x2c);
    uint8_t journal[kPageBytes];
    if (readPage(port, state.spare_block * pages_per_block, journal, nullptr) != kOk
        || readPage(port, state.spare_block * pages_per_block + 1, journal, nullptr) != kOk) {
        return kNoRootTail;
    }

    if (!directoryEntry(port, 0, 0, entry_buffer)) {
        return kNoRootFirst;
    }
    if (!nameIs(entry_buffer, ".")) {
        return kNotFormatted;
    }
    if (!directoryEntry(port, 0, 1, entry_buffer)) {
        return kNoRootSecond;
    }
    if (!nameIs(entry_buffer, "..")) {
        return kNotFormatted;
    }
    state.mounted = true;
    state.type = 2;
    return kOk;
}

// --- the exports (IOP-15g) -------------------------------------------------

uint32_t resident;
uint32_t lock;

// IOP-15c's ladder, before the masking export 5 applies.
[[nodiscard]] int detectLadder(uint32_t port, uint32_t slot) {
    selectPort(port);
    Port &state = ports[port & 1];
    bool authenticated = false;
    if (!probe(port)) {
        if (!reset(port)) {
            return kNoReset;
        }
        if (_import_secrman_auth_card(static_cast<int>(serialPort(port)),
                                      static_cast<int>(slot),
                                      static_cast<int>((port & 1) << 3)) == 0) {
            return kNoAuth;
        }
        authenticated = true;
    }
    uint8_t terminator = 0;
    if (!presence(port, terminator)) {
        return kNoPresence;
    }
    if (terminator == kAck) {
        if (state.mounted) {
            return kOk;                 // the same card, still mounted
        }
    } else {
        if (!authenticated
            && _import_secrman_auth_card(static_cast<int>(serialPort(port)),
                                         static_cast<int>(slot),
                                         static_cast<int>((port & 1) << 3)) == 0) {
            return kNoAuth;
        }
        if (!setTerminator(port)) {
            return kNoTerminator;
        }
    }
    const int mounted = mount(port);
    return mounted == kOk ? kCardChanged : mounted;
}

// Export 5. IOP-15c: anything below -1 is masked. The reference replaces it
// with a probe for the older card format; this has none, so it answers the
// code that probe gives an empty slot, and the difference is recorded in
// docs/implementation.md.
constexpr int kNothingThere = -11;

int detect(int port, int slot) {
    if (port < 0 || static_cast<uint32_t>(port) >= kPorts) {
        return kNothingThere;
    }
    const uint32_t index = static_cast<uint32_t>(port);
    const int answer = detectLadder(index, static_cast<uint32_t>(slot));
    if (answer >= kCardChanged) {
        ports[index & 1].type = 2;
        return answer;
    }
    if (answer == kNotFormatted) {
        ports[index & 1].type = 2;
        return kNotFormatted;
    }
    ports[index & 1].type = 0;
    ports[index & 1].mounted = false;
    return kNothingThere;
}

// Export 39.
int cardType(int port) {
    if (port < 0 || static_cast<uint32_t>(port) >= kPorts) {
        return 0;
    }
    return ports[port & 1].type;
}

// Export 12's record (IOP-15g), 0x40 bytes.
struct Listing {
    uint8_t created[8];
    uint8_t modified[8];
    uint32_t length;
    uint16_t mode;
    uint16_t spare;
    uint32_t attributes;
    uint8_t pad[4];
    uint8_t name[32];
};
static_assert(sizeof(Listing) == 0x40);

constexpr uint16_t kEntryInUse = 1u << 15;

// Export 12, narrowed: this rebuild lists the root and nothing else, so the
// path is read only far enough to reject anything that is not it. IOP-15g's
// continuation contract is kept -- a mode of zero starts, anything else
// continues, and a count of zero is the end.
uint32_t listing_position[kPorts];
uint32_t listing_bound[kPorts];

int getDir(int port, int slot, const char *path, int mode, int max, void *out) {
    if (port < 0 || static_cast<uint32_t>(port) >= kPorts || max <= 0) {
        return 0;
    }
    const uint32_t index = static_cast<uint32_t>(port);
    const int found = detect(port, slot);
    if (found < kCardChanged) {
        return 0;
    }
    if (path == nullptr || path[0] != '/') {
        return 0;                       // only the root, in this rebuild
    }
    if (mode == 0) {
        // IOP-15g: the root's two conventional entries are not listed, and
        // the walk stops at the directory's own length. A slot past it reads
        // as every bit set, which passes an in-use test made of one bit --
        // so the length is the bound, not the bit.
        listing_position[index] = 2;
        listing_bound[index] = 0;
        if (directoryEntry(index, 0, 0, entry_buffer)) {
            listing_bound[index] = leWord(entry_buffer + 4);
        }
    }

    auto *records = static_cast<Listing *>(out);
    int written = 0;
    while (written < max) {
        if (listing_position[index] >= listing_bound[index]
            || !directoryEntry(index, 0, listing_position[index],
                               entry_buffer)) {
            break;
        }
        listing_position[index]++;
        const uint32_t entry_mode = leWord(entry_buffer);
        if ((entry_mode & kEntryInUse) == 0) {
            continue;
        }
        Listing &record = records[written];
        for (uint32_t i = 0; i < 8; i++) {
            record.created[i] = entry_buffer[8 + i];
            record.modified[i] = entry_buffer[0x18 + i];
        }
        record.length = leWord(entry_buffer + 4);
        record.mode = static_cast<uint16_t>(entry_mode);
        record.spare = static_cast<uint16_t>(leHalf(entry_buffer + 2));
        record.attributes = leWord(entry_buffer + 0x20);
        for (uint32_t i = 0; i < 4; i++) {
            record.pad[i] = 0;
        }
        for (uint32_t i = 0; i < 32; i++) {
            record.name[i] = entry_buffer[0x40 + i];
        }
        written++;
    }
    return written;
}

// IOP-15h: the service hands the driver its own transport for the
// authentication exchange. Ours never runs one, so this is registered for the
// same reason the reference registers it -- the interface answers 0 to a
// driver that has not -- and is never called.
int commandHandler(int, int, void *descriptor) {
    _import_sio2man_mc_transfer_init();
    return _import_sio2man_transfer(static_cast<TransferData *>(descriptor));
}

int deviceIdHandler(int port, int) {
    return (port & 1) << 3;
}

extern "C" int _module_start(int, char **);

PS2_EXPORT_TABLE ExportTable<43> mcman_exports = {
    ps2::module::kExportMagic,
    0,
    0x0101,
    0,
    {'m', 'c', 'm', 'a', 'n', 0, 0, 0},
    {
        slot(_module_start),            // 0
        slot(reservedHook),             // 1
        slot(reservedHook),             // 2
        slot(reservedHook),             // 3
        slot(_module_start),            // 4  aliases the entry
        slot(detect),                   // 5
        slot(reservedHook),             // 6  open
        slot(reservedHook),             // 7  close
        slot(reservedHook),             // 8  read
        slot(reservedHook),             // 9  write
        slot(reservedHook),             // 10 seek
        slot(reservedHook),             // 11 format
        slot(getDir),                   // 12
        slot(reservedHook),             // 13 delete
        slot(reservedHook),             // 14 flush
        slot(reservedHook),             // 15 chdir
        slot(reservedHook),             // 16 set info
        slot(reservedHook),             // 17 erase block
        slot(reservedHook),             // 18 read page
        slot(reservedHook),             // 19 write page
        slot(reservedHook),             // 20 error-correcting code
        slot(reservedHook),             // 21
        slot(reservedHook),             // 22
        slot(reservedHook),             // 23
        slot(reservedHook),             // 24
        slot(reservedHook),             // 25
        slot(reservedHook),             // 26
        slot(reservedHook),             // 27
        slot(reservedHook),             // 28
        slot(reservedHook),             // 29 older-format page read
        slot(reservedHook),             // 30 older-format page write
        slot(reservedHook),             // 31
        slot(reservedHook),             // 32
        slot(reservedHook),             // 33
        slot(reservedHook),             // 34
        slot(reservedHook),             // 35
        slot(reservedHook),             // 36 unmount
        slot(reservedHook),             // 37
        slot(reservedHook),             // 38 free clusters
        slot(cardType),                 // 39
        slot(reservedHook),             // 40
        slot(reservedHook),             // 41
        slot(reservedHook),             // 42
        nullptr,
    },
};

}  // namespace

PS2_IMPORTS_BEGIN("loadcore", 0x0101)
PS2_IMPORT(_import_loadcore_register, 6)
PS2_IMPORTS_END()

PS2_IMPORTS_BEGIN("intrman\0", 0x0102)
PS2_IMPORT(_import_intrman_enable_all, 9)
PS2_IMPORTS_END()

PS2_IMPORTS_BEGIN("sio2man\0", 0x0101)
PS2_IMPORT(_import_sio2man_mc_transfer_init, 24)
PS2_IMPORT(_import_sio2man_transfer, 25)
PS2_IMPORTS_END()

PS2_IMPORTS_BEGIN("secrman\0", 0x0103)
PS2_IMPORT(_import_secrman_set_command_handler, 4)
PS2_IMPORT(_import_secrman_set_devid_handler, 5)
PS2_IMPORT(_import_secrman_auth_card, 6)
PS2_IMPORTS_END()

PS2_IMPORTS_BEGIN("thsemap\0", 0x0101)
PS2_IMPORT(_import_thsemap_create, 4)
PS2_IMPORTS_END()

PS2_IMPORTS_BEGIN("stdio\0\0\0", 0x0102)
PS2_IMPORT(_import_stdio_printf, 4)
PS2_IMPORTS_END()

extern "C" {

int _module_start(int, char **) {
    if (_import_loadcore_register(&mcman_exports) < 0 || resident != 0) {
        return 1;
    }
    resident = 1;
    (void)_import_intrman_enable_all();

    // IOP-15d: the parts of the descriptor that never change again.
    for (uint32_t i = 0; i < 4; i++) {
        td.port_ctrl1[i] = 0;
        td.port_ctrl2[i] = 0;
    }
    for (uint32_t i = 0; i < 16; i++) {
        td.regdata[i] = 0;
    }
    td.port_ctrl2[2] = kPortCtrl2;
    td.port_ctrl2[3] = kPortCtrl2;
    td.in_size = 0;
    td.out_size = 0;
    td.in = nullptr;
    td.out = nullptr;
    td.in_dma.address = reinterpret_cast<uintptr_t>(tx);
    td.in_dma.size = kBlockWords;
    td.in_dma.count = 0;
    td.out_dma.address = reinterpret_cast<uintptr_t>(rx);
    td.out_dma.size = kBlockWords;
    td.out_dma.count = 0;

    (void)_import_secrman_set_command_handler(commandHandler);
    (void)_import_secrman_set_devid_handler(deviceIdHandler);

    SemaParameters parameters;
    parameters.attr = 1;
    parameters.option = 0;
    parameters.initial = 1;
    parameters.max = 1;
    const int id = _import_thsemap_create(&parameters);
    lock = id < 0 ? 0 : static_cast<uint32_t>(id);

    _import_stdio_printf("MCMAN: card driver, read only\n");
    return 0;                                   // resident
}

}  // extern "C"
