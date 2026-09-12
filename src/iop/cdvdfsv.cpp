// CDVDFSV: the EE-facing RPC surface over CDVDMAN's disc driver.
//
// docs/analysis/42-cdvd.md §5 is the analysis this file follows; there is no
// spec section for the CDVD pair yet (docs/project-state.md §6), so the
// requirement ids the other modules cite have no counterpart here -- the same
// position `src/iop/cdvdman.cpp` is in, and the same one `src/iop/fileio.cpp`
// started from before its own analysis settled.
//
// The reference registers five services across two long-lived threads, each
// thread building one RpcQueue and reusing it for every RegisterRpc on it,
// with no completion callback anywhere (§5). That split is kept: a blocking
// service on one thread must not stall a service the title polls from
// another.
//
//   thread A: 0x80000592, 0x8000059A, 0x80000593
//   thread B: 0x80000597, 0x80000595
//
// What each service is, and how much of it is served here:
//
//   0x80000592  init. Not fno-switched (§5a): the dispatcher never reads fno.
//               It calls sceCdInit with the request's first word. Served.
//   0x80000593  the 25-fno table (IOP-13b). The fnos that are one CDVDMAN ordinal
//               each, and whose ordinal this project's CDVDMAN implements,
//               are served; the rest answer zeroes. fno 22 is the
//               reference's own multi-way dispatcher and its sub-opcode
//               field is undecoded (§5, §9) -- not guessed at here.
//   0x80000595  a second, 14-fno table (§5d): the streaming and raw-command
//               groups. Every one of its fnos reaches CDVDMAN ordinals this
//               project's driver still stubs (§5d's own list), except fno 14,
//               which is a bare register check and is served.
//   0x80000597  sceCdSearchFile (§5c). Served in full, including the DMA of
//               the filled `sceCdlFILE` to the EE address the request names.
//   0x8000059A  sceCdDiskReady (§5c). Served: like the reference, it reads
//               the N-command status register directly rather than going
//               through CDVDMAN, which is why CDVDFSV imports no ordinal 13.
//
// Both fno-switched services take their bound as an unsigned compare on
// `fno - 1`, so fno 0 is rejected with everything above the table -- and a
// rejected fno is still *acknowledged*, from the same fixed reply context a
// served one uses (§5b). A retail title relies on that: `SLPS-25918` sends
// `fno 0x22` to `0x80000593`, which belongs to the later `XCDVDFSV`'s wider
// table, and carries on when this generation answers a no-op.
//
// The title binds 0x8000059C as well (docs/analysis/43 §11), which the
// reference's CDVDFSV does not register (§5) -- so it is not registered here
// either; it belongs to a later-generation module.
//
// Buffer sizes follow §5's own `.bss` gaps: each service's request buffer is
// sized to its own largest request, not to a common maximum, and each
// answers from a small fixed reply cell of its own. Like the reference,
// nothing here bounds-checks the EE's `send_size` against the buffer it
// names -- the same exposure `fileio.cpp` carries.

#include "module.hpp"
#include "sifrpc.hpp"

#include <stdint.h>

namespace {

using ps2::sifrpc::Queue;
using ps2::sifrpc::Server;

constexpr uint32_t kSidInit = 0x80000592;
constexpr uint32_t kSidMain = 0x80000593;       // the 25-fno table
constexpr uint32_t kSidStream = 0x80000595;     // the 14-fno table
constexpr uint32_t kSidSearch = 0x80000597;     // sceCdSearchFile
constexpr uint32_t kSidReady = 0x8000059A;      // sceCdDiskReady

// §5: TH_C, priority 0x51, a 6 KiB stack each.
constexpr uint32_t kThreadAttr = 0x02000000;    // TH_C [header]
constexpr uint32_t kThreadPriority = 0x51;
constexpr uint32_t kThreadStack = 0x1800;

// §2b: the N-command status register, read directly by two of the services
// exactly as the reference reads it -- `(status & 0xC0) == 0x40` is "the
// drive is ready to take a command".
constexpr uintptr_t kNStatus = 0xBF402005;
constexpr uint8_t kNStatusMask = 0xC0;
constexpr uint8_t kNStatusReady = 0x40;
constexpr uint32_t kReadyComplete = 2;          // SCECdComplete [header]
constexpr uint32_t kReadyNotReady = 6;          // SCECdNotReady [header]

struct ThreadParameters {
    uint32_t attr;
    uint32_t option;
    void (*entry)(void *);
    uint32_t stack_size;
    uint32_t priority;
};

// sceSifSetDma's entry [header]: IOP memory to EE memory.
struct Transfer {
    uint32_t src;
    uint32_t dest;
    uint32_t size;
    uint32_t attr;
};

extern "C" {
int _import_thbase_create(ThreadParameters *parameters);
int _import_thbase_start(uint32_t id, uint32_t arg);
int _import_thbase_get_id();
int _import_sifcmd_init_rpc(uint32_t mode);
int _import_sifcmd_register_rpc(Server *server, uint32_t sid, void *function, void *buffer,
                                void *cfunction, void *cbuffer, Queue *queue);
int _import_sifcmd_set_rpc_queue(Queue *queue, uint32_t thread_id);
int _import_sifcmd_rpc_loop(Queue *queue);
int _import_sifman_set_dma(const Transfer *list, uint32_t count);
int _import_sifman_dma_stat(uint32_t id);
int _import_intrman_suspend(uint32_t *state);
int _import_intrman_resume(uint32_t state);
int _import_cdvdman_init(int mode);
int _import_cdvdman_search_file(void *file, const char *name);
int _import_cdvdman_get_error();
int _import_cdvdman_get_disk_type();
int _import_cdvdman_status();
int _import_cdvdman_read_nvm(uint32_t address, uint16_t *data, uint8_t *status);
int _import_cdvdman_write_nvm(uint32_t address, uint16_t data, uint8_t *status);
int _import_cdvdman_open_config(uint32_t a, uint32_t b, uint32_t count,
                                uint32_t *status);
int _import_cdvdman_close_config(uint32_t *status);
int _import_cdvdman_read_config(uint8_t *buffer, uint32_t *status);
int _import_cdvdman_write_config(const uint8_t *buffer, uint32_t *status);
}

[[nodiscard]] uint8_t readStatus() {
    return *reinterpret_cast<volatile uint8_t *>(kNStatus);
}

// IOP-13c: every service answers through one fixed area of its own, so they
// are grouped rather than sized per fno. The largest single reply is the
// configuration read's `8 + 15 * count`; `count` is the caller's, and this
// bound is what a request for more than eight blocks is refused against.
constexpr uint32_t kConfigBlockBytes = 15;      // IOP-8k: what leaves CDVDMAN
constexpr uint32_t kConfigHeaderBytes = 8;      // IOP-13c: return, then status
constexpr uint32_t kReplyBytes = 0x80;
constexpr uint32_t kConfigBlocksMax =
    (kReplyBytes - kConfigHeaderBytes) / kConfigBlockBytes;

// §5e: the request-buffer sizes are the reference's own `.bss` gaps.
constexpr uint32_t kRequestInit = 0x10;
constexpr uint32_t kRequestMain = 0x410;
constexpr uint32_t kRequestStream = 0x40;
constexpr uint32_t kRequestSearch = 0x130;      // `sceCdlFILE` + path + EE address
constexpr uint32_t kRequestReady = 0x40;

template <uint32_t kRequestBytes>
struct Service {
    Server server;
    alignas(16) uint32_t request[kRequestBytes / 4];
    alignas(16) uint32_t reply[kReplyBytes / 4];

    uint32_t *cleared() {
        for (uint32_t k = 0; k < kReplyBytes / 4; k++) {
            reply[k] = 0;
        }
        return reply;
    }
};

Service<kRequestInit> init_service;
Service<kRequestMain> main_service;
Service<kRequestStream> stream_service;
Service<kRequestSearch> search_service;
Service<kRequestReady> ready_service;

Queue queue_a;
Queue queue_b;

// §5c: one transfer to EE memory, inside the critical section the reference
// takes around `sceSifSetDma`. The reference fires and forgets -- the reply
// packet its RpcLoop sends afterwards travels the same channel, so the data
// is already ahead of it. Waiting for the drain costs nothing and removes
// the assumption, so this waits, the way `fileio.cpp` does.
void sendToEe(const void *from, uint32_t ee_address, uint32_t bytes) {
    Transfer transfer;
    transfer.src = reinterpret_cast<uintptr_t>(from);
    transfer.dest = ee_address;
    transfer.size = bytes;
    transfer.attr = 0;
    uint32_t state;
    _import_intrman_suspend(&state);
    const int id = _import_sifman_set_dma(&transfer, 1);
    _import_intrman_resume(state);
    while (id != 0 && _import_sifman_dma_stat(static_cast<uint32_t>(id)) >= 0) {
    }
}

// §5b/§5d: the shared bound. The compare is unsigned on `fno - 1`, so fno 0
// wraps and is rejected along with everything past the table's end.
[[nodiscard]] constexpr bool inTable(uint32_t fno, uint32_t count) {
    return fno - 1 < count;
}

// --- 0x80000592: sceCdInit (§5a) ---------------------------------------------
//
// Not fno-switched: the dispatcher never reads fno. The reference also raises
// an "initialised" flag of its own here, which nothing reads.
void *serveInit(uint32_t, void *buffer, uint32_t size) {
    uint32_t *reply = init_service.cleared();
    const uint32_t mode = size >= 4 ? static_cast<const uint32_t *>(buffer)[0] : 0;
    reply[0] = static_cast<uint32_t>(_import_cdvdman_init(static_cast<int>(mode)));
    return reply;
}

// --- 0x80000593: the 25-fno table (§5b) --------------------------------------

constexpr uint32_t kMainFnoCount = 25;
constexpr uint32_t kFnoGetDiskType = 3;         // sceCdGetDiskType, ordinal 12
constexpr uint32_t kFnoGetError = 4;            // sceCdGetError, ordinal 8
constexpr uint32_t kFnoReadNvm = 8;             // ordinal 26
constexpr uint32_t kFnoWriteNvm = 9;            // ordinal 27
constexpr uint32_t kFnoStatus = 12;             // sceCdStatus, ordinal 28
constexpr uint32_t kFnoOpenConfig = 14;         // ordinal 31
constexpr uint32_t kFnoCloseConfig = 15;        // ordinal 32
constexpr uint32_t kFnoReadConfig = 16;         // ordinal 33
constexpr uint32_t kFnoWriteConfig = 17;        // ordinal 34

// IOP-13e: how many blocks the session was opened for, clamped to what the
// reply area can hold. **The clamped count is what `CDVDMAN` is opened with**,
// not just what is recorded here: the read hands `CDVDMAN` a pointer into the
// reply and `CDVDMAN` fills its own count of blocks, so a count this service
// accepted but did not pass on would be written past the end of the area. The
// request's count byte is the EE's, and nothing above this checks it.
uint32_t config_blocks;

// IOP-13e1: the NVM pair's out-pointers go **into the request buffer**, and
// the reply carries the request's first two words back behind the return.
// Nothing else in this table answers in that shape.
void serveNvm(uint32_t fno, void *buffer, uint32_t size, uint32_t *reply) {
    if (size < 8) {
        return;                                 // no room for the out-params
    }
    auto *request = static_cast<uint8_t *>(buffer);
    const uint32_t address = *reinterpret_cast<uint32_t *>(request);
    auto *data = reinterpret_cast<uint16_t *>(request + 4);
    auto *status = request + 6;
    const int result = fno == kFnoReadNvm
        ? _import_cdvdman_read_nvm(address, data, status)
        : _import_cdvdman_write_nvm(address, *data, status);
    reply[0] = static_cast<uint32_t>(result);
    reply[1] = *reinterpret_cast<uint32_t *>(request);
    reply[2] = *reinterpret_cast<uint32_t *>(request + 4);
}

void *serveMain(uint32_t fno, void *buffer, uint32_t size) {
    uint32_t *reply = main_service.cleared();
    if (!inTable(fno, kMainFnoCount)) {
        return reply;                           // IOP-13b: acknowledged no-op
    }
    // IOP-13c: the shared shape -- the ordinal's return at +0x0, the `status`
    // word it filled at +0x4, and a payload after that where there is one.
    auto *status = &reply[1];
    switch (fno) {
    case kFnoGetDiskType:
        reply[0] = static_cast<uint32_t>(_import_cdvdman_get_disk_type());
        break;
    case kFnoGetError:
        reply[0] = static_cast<uint32_t>(_import_cdvdman_get_error());
        break;
    case kFnoStatus:
        reply[0] = static_cast<uint32_t>(_import_cdvdman_status());
        break;
    case kFnoReadNvm:
    case kFnoWriteNvm:
        serveNvm(fno, buffer, size, reply);
        break;
    case kFnoOpenConfig: {
        // IOP-13e: one word, and its byte order is the wire's own -- byte 0
        // is the ordinal's *second* argument. Unpacking it the obvious way
        // opens the session on the wrong record, with no error anywhere.
        const uint32_t packed =
            size >= 4 ? static_cast<const uint32_t *>(buffer)[0] : 0;
        const uint32_t count = (packed >> 16) & 0xFF;
        config_blocks = count < kConfigBlocksMax ? count : kConfigBlocksMax;
        reply[0] = static_cast<uint32_t>(_import_cdvdman_open_config(
            (packed >> 8) & 0xFF, packed & 0xFF, config_blocks, status));
        break;
    }
    case kFnoCloseConfig:
        config_blocks = 0;
        reply[0] = static_cast<uint32_t>(_import_cdvdman_close_config(status));
        break;
    case kFnoReadConfig:
        // The blocks land in the reply itself, at +0x8. The request is not
        // read at all -- the count came from the open.
        reply[0] = static_cast<uint32_t>(_import_cdvdman_read_config(
            reinterpret_cast<uint8_t *>(&reply[2]), status));
        break;
    case kFnoWriteConfig:
        if (size >= config_blocks * kConfigBlockBytes) {
            reply[0] = static_cast<uint32_t>(_import_cdvdman_write_config(
                static_cast<const uint8_t *>(buffer), status));
        }
        break;
    default:
        break;                                  // zeroes, until §9 is closed
    }
    return reply;
}

// --- 0x80000595: the 14-fno table (§5d) --------------------------------------
//
// Thirteen of its fourteen fnos reach CDVDMAN ordinals this project's driver
// still stubs (5, 7, 9, 15, 20, 35, 38, 40, 44, 49, 50, 54); serving them
// would answer plausible-looking nonsense, so they answer zeroes until the
// driver has the ordinals. fno 14 needs no ordinal at all -- it is the same
// register check `0x8000059A` does, without the blocking mode.

constexpr uint32_t kStreamFnoCount = 14;
constexpr uint32_t kFnoStreamReady = 14;

void *serveStream(uint32_t fno, void *, uint32_t) {
    uint32_t *reply = stream_service.cleared();
    if (!inTable(fno, kStreamFnoCount)) {
        return reply;
    }
    if (fno == kFnoStreamReady) {
        reply[0] = (readStatus() & kNStatusMask) == kNStatusReady ? kReadyComplete
                                                                  : kReadyNotReady;
    }
    return reply;
}

// --- 0x80000597: sceCdSearchFile (§5c) ---------------------------------------
//
// `libcdvd-rpc.h`'s `rpc4` wire shape, confirmed against the dispatcher: the
// request buffer *is* the `sceCdlFILE` output struct, the path follows it at
// `+0x20`, and the EE address the filled struct goes back to is at `+0x120`.
// The reply cell carries the return code alone.

constexpr uint32_t kSearchPathOffset = 0x20;
constexpr uint32_t kSearchAddressOffset = 0x120;
constexpr uint32_t kSearchFileBytes = 0x20;     // sizeof(sceCdlFILE)

void *serveSearch(uint32_t, void *buffer, uint32_t) {
    uint32_t *reply = search_service.cleared();
    auto *bytes = static_cast<uint8_t *>(buffer);
    const auto *name = reinterpret_cast<const char *>(bytes + kSearchPathOffset);
    const int result = _import_cdvdman_search_file(bytes, name);
    const uint32_t destination =
        *reinterpret_cast<const uint32_t *>(bytes + kSearchAddressOffset);
    sendToEe(bytes, destination, kSearchFileBytes);
    reply[0] = static_cast<uint32_t>(result);
    return reply;
}

// --- 0x8000059A: sceCdDiskReady (§5c) ----------------------------------------
//
// Request word 0 is the mode: 0 spins until the drive is ready, anything else
// checks once. The reference reads the register itself rather than calling
// CDVDMAN's ordinal 13, which is why no CDVDFSV service imports it.
void *serveReady(uint32_t, void *buffer, uint32_t size) {
    uint32_t *reply = ready_service.cleared();
    const uint32_t mode = size >= 4 ? static_cast<const uint32_t *>(buffer)[0] : 0;
    if (mode == 0) {
        while ((readStatus() & kNStatusMask) != kNStatusReady) {
        }
    }
    reply[0] = (readStatus() & kNStatusMask) == kNStatusReady ? kReadyComplete
                                                              : kReadyNotReady;
    return reply;
}

template <uint32_t kRequestBytes>
void registerService(Service<kRequestBytes> &service, uint32_t sid, void *function,
                     Queue &queue) {
    _import_sifcmd_register_rpc(&service.server, sid, function, service.request,
                                nullptr, nullptr, &queue);
}

void threadA(void *) {
    _import_sifcmd_init_rpc(0);
    _import_sifcmd_set_rpc_queue(&queue_a, static_cast<uint32_t>(_import_thbase_get_id()));
    registerService(init_service, kSidInit, reinterpret_cast<void *>(serveInit), queue_a);
    registerService(ready_service, kSidReady, reinterpret_cast<void *>(serveReady), queue_a);
    registerService(main_service, kSidMain, reinterpret_cast<void *>(serveMain), queue_a);
    _import_sifcmd_rpc_loop(&queue_a);
}

void threadB(void *) {
    _import_sifcmd_init_rpc(0);
    _import_sifcmd_set_rpc_queue(&queue_b, static_cast<uint32_t>(_import_thbase_get_id()));
    registerService(search_service, kSidSearch, reinterpret_cast<void *>(serveSearch), queue_b);
    registerService(stream_service, kSidStream, reinterpret_cast<void *>(serveStream), queue_b);
    _import_sifcmd_rpc_loop(&queue_b);
}

[[nodiscard]] bool startThread(void (*entry)(void *)) {
    ThreadParameters parameters;
    parameters.attr = kThreadAttr;
    parameters.option = 0;
    parameters.entry = entry;
    parameters.stack_size = kThreadStack;
    parameters.priority = kThreadPriority;
    const int id = _import_thbase_create(&parameters);
    if (id < 0) {
        return false;
    }
    return _import_thbase_start(static_cast<uint32_t>(id), 0) >= 0;
}

}  // namespace

PS2_IMPORTS_BEGIN("thbase\0\0", 0x0101)
PS2_IMPORT(_import_thbase_create, 4)
PS2_IMPORT(_import_thbase_start, 6)
PS2_IMPORT(_import_thbase_get_id, 20)
PS2_IMPORTS_END()

PS2_IMPORTS_BEGIN("sifcmd\0\0", 0x0101)
PS2_IMPORT(_import_sifcmd_init_rpc, 14)
PS2_IMPORT(_import_sifcmd_register_rpc, 17)
PS2_IMPORT(_import_sifcmd_set_rpc_queue, 19)
PS2_IMPORT(_import_sifcmd_rpc_loop, 22)
PS2_IMPORTS_END()

PS2_IMPORTS_BEGIN("sifman\0\0", 0x0101)
PS2_IMPORT(_import_sifman_set_dma, 7)
PS2_IMPORT(_import_sifman_dma_stat, 8)
PS2_IMPORTS_END()

PS2_IMPORTS_BEGIN("intrman\0", 0x0102)
PS2_IMPORT(_import_intrman_suspend, 17)
PS2_IMPORT(_import_intrman_resume, 18)
PS2_IMPORTS_END()

PS2_IMPORTS_BEGIN("cdvdman\0", 0x0101)
PS2_IMPORT(_import_cdvdman_init, 4)
PS2_IMPORT(_import_cdvdman_get_error, 8)
PS2_IMPORT(_import_cdvdman_search_file, 10)
PS2_IMPORT(_import_cdvdman_get_disk_type, 12)
PS2_IMPORT(_import_cdvdman_status, 28)
PS2_IMPORT(_import_cdvdman_read_nvm, 26)
PS2_IMPORT(_import_cdvdman_write_nvm, 27)
PS2_IMPORT(_import_cdvdman_open_config, 31)
PS2_IMPORT(_import_cdvdman_close_config, 32)
PS2_IMPORT(_import_cdvdman_read_config, 33)
PS2_IMPORT(_import_cdvdman_write_config, 34)
PS2_IMPORTS_END()

extern "C" {

int _module_start(int, char **) {
    if (!startThread(threadA) || !startThread(threadB)) {
        return 1;
    }
    return 0;                                   // resident
}

}  // extern "C"
