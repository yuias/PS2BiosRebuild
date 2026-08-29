// FILEIO: the RPC server the EE's SDK-runtime `fileXio`/`fio` layer talks to
// for cdrom0:/host: style access, once the title itself brings the file up.
//
// docs/spec/06-iop-kernel.md IOP-5f/g/h (LOADFILE's own thread/RPC shape,
// mirrored here) and docs/analysis/43-fileio-and-title-boot.md §1-§5, which
// is the analysis this file follows. The module exports nothing: its entry
// starts two threads, both priority 0x60, and each registers one server and
// loops on its own queue forever. `sid 0x80000001` is the file RPC proper,
// on a 4 KiB stack; `sid 0x80000003` is the IOP-heap service -- allocate,
// free, and load a whole file into IOP memory -- on a 2 KiB one. The second
// was analysis §12's "registered, dispatch read, no caller identified"; the
// caller is a retail title, which binds it immediately after rebooting the
// IOP and before loading any of its own modules.
//
// Deviation from the reference (docs/implementation.md carries the summary):
// our EESYNC does not implement `sceSifGetOtherData` (sifcmd ordinal 23,
// analysis §0), which the reference uses to place `read`'s unaligned head/
// tail and `dread`'s/`getstat`'s reply structs at an EE address of any
// alignment. `getstat` here goes out through a plain `sceSifSetDma` instead
// (matching analysis §3's own "(+ sceSifSetDma)" annotation for that fno);
// `read`'s unaligned edges go out the same way `loadfile.cpp`'s `sendSegment`
// already moves a segment's own unaligned lead: the whole 16-byte block the
// edge falls in is sent, zero-filled around the requested bytes. This
// clobbers up to 15 bytes on either side of the caller's own range -- the
// SDK's own EE client passes buffers it owns outright, so for M2 this is a
// bounded cost rather than a live correctness bug.

#include "module.hpp"
#include "sifrpc.hpp"

#include <stdint.h>

namespace {

using ps2::sifrpc::Queue;
using ps2::sifrpc::Server;

constexpr uint32_t kServerId = 0x80000001;
constexpr uint32_t kRequestBytes = 0x200;
constexpr uint32_t kThreadPriority = 0x60;
constexpr uint32_t kThreadStack = 0x1000;
constexpr uint32_t kThreadAttr = 0x02000000;    // TH_C [header]

// The heap service (§1): its own server, buffers and thread.
constexpr uint32_t kHeapServerId = 0x80000003;
constexpr uint32_t kHeapRequestBytes = 0x100;   // §1: what the reference's .bss leaves it
constexpr uint32_t kHeapThreadStack = 0x800;

struct ThreadParameters {
    uint32_t attr;
    uint32_t option;
    void (*entry)(void *);
    uint32_t stack_size;
    uint32_t priority;
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
int _import_ioman_open(const char *path, int flags);
int _import_ioman_close(int fd);
int _import_ioman_read(int fd, void *buffer, int size);
int _import_ioman_lseek(int fd, int offset, int whence);
int _import_ioman_getstat(const char *path, void *stat);
int _import_intrman_suspend(uint32_t *state);
int _import_intrman_resume(uint32_t state);
int _import_sysmem_allocate(uint32_t mode, uint32_t size, uint32_t address);
int _import_sysmem_release(uint32_t address);
}

alignas(16) uint32_t request[kRequestBytes / 4];
alignas(16) int32_t answer[4];
Server server;
Queue queue;

alignas(16) uint32_t heap_request[kHeapRequestBytes / 4];
alignas(16) int32_t heap_answer[4];
Server heap_server;
Queue heap_queue;

// --- fno dispatch table (analysis §3; only what M2 needs is served) ---------

constexpr uint32_t kFnoOpen = 0;
constexpr uint32_t kFnoClose = 1;
constexpr uint32_t kFnoRead = 2;
constexpr uint32_t kFnoLseek = 4;
constexpr uint32_t kFnoGetstat = 12;
constexpr uint32_t kFnoLimit = 17;              // analysis §3: fno < 0x11 served at all
constexpr int kOtherwiseUnserved = -1;

// sceSifSetDma's entry [header]: IOP memory to EE memory.
struct Transfer {
    uint32_t src;
    uint32_t dest;
    uint32_t size;
    uint32_t attr;
};

extern "C" {
int _import_sifman_set_dma(const Transfer *list, uint32_t count);
int _import_sifman_dma_stat(uint32_t id);
}

// analysis §5: the bounce buffer's chunk size. The reference probes for the
// largest allocation it can get, up to 16 KiB, halving on failure down to
// 128 bytes (analysis §2); ours is one fixed `.bss` allocation instead, since
// a rebuild does not need SYSMEM's failure path to be faithful here.
constexpr uint32_t kBounceBytes = 0x1000;
alignas(16) uint8_t bounce[kBounceBytes];

// Scratch for an unaligned edge: at most 15 bytes either side of a 16-byte
// boundary, so 32 bytes covers even a sub-16-byte request that itself spans
// two blocks (deviation note above).
alignas(16) uint8_t edge[32];

constexpr uint32_t kStatBytes = 0x28;           // analysis §4: iox_stat_t-sized
alignas(16) uint8_t stat_block[kStatBytes];

[[nodiscard]] constexpr uint32_t alignUp16(uint32_t value) {
    return (value + 15) & ~uint32_t{15};
}

// One transfer to EE memory, waited for, inside the critical section the
// reference takes around `sceSifSetDma` (analysis §5: intrman 17/18).
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

// analysis §5: `read` splits by the 16-byte alignment of `dest_ee`, not of
// the file offset -- an unaligned head, an aligned middle through the bounce
// buffer, an unaligned tail. A request under 16 bytes is just a head: the
// single block its bytes fall in (at most two, since offset and length are
// each under 16) is zero-filled and sent whole.
[[nodiscard]] int32_t doRead(uint32_t fd, uint32_t dest_ee, uint32_t length) {
    if (length == 0) {
        return 0;
    }
    const uint32_t offset = dest_ee & 0xF;
    if (length < 16) {
        const uint32_t block_bytes = alignUp16(offset + length);
        for (uint32_t k = 0; k < block_bytes; k++) {
            edge[k] = 0;
        }
        const int got = _import_ioman_read(static_cast<int>(fd), edge + offset,
                                           static_cast<int>(length));
        if (got < 0) {
            return got;
        }
        sendToEe(edge, dest_ee - offset, block_bytes);
        return got;
    }

    const uint32_t head = (16 - offset) & 0xF;
    uint32_t cursor = dest_ee;
    uint32_t total = 0;

    if (head != 0) {
        for (uint32_t k = 0; k < 16; k++) {
            edge[k] = 0;
        }
        const int got = _import_ioman_read(static_cast<int>(fd), edge + offset,
                                           static_cast<int>(head));
        if (got < 0) {
            return got;
        }
        sendToEe(edge, dest_ee - offset, 16);
        total += static_cast<uint32_t>(got);
        cursor += static_cast<uint32_t>(got);
        if (static_cast<uint32_t>(got) < head) {
            return static_cast<int32_t>(total);    // short read: EOF inside the head
        }
    }

    const uint32_t remaining = length - head;
    const uint32_t middle = remaining - (remaining % 16);
    uint32_t done = 0;
    while (done < middle) {
        uint32_t wanted = middle - done;
        if (wanted > kBounceBytes) {
            wanted = kBounceBytes;
        }
        const int got = _import_ioman_read(static_cast<int>(fd), bounce, static_cast<int>(wanted));
        if (got < 0) {
            return total != 0 ? static_cast<int32_t>(total) : got;
        }
        sendToEe(bounce, cursor, static_cast<uint32_t>(got));
        total += static_cast<uint32_t>(got);
        cursor += static_cast<uint32_t>(got);
        done += static_cast<uint32_t>(got);
        if (static_cast<uint32_t>(got) < wanted) {
            return static_cast<int32_t>(total);    // short read: EOF inside the middle
        }
    }

    const uint32_t tail = remaining - middle;
    if (tail != 0) {
        for (uint32_t k = 0; k < 16; k++) {
            edge[k] = 0;
        }
        const int got = _import_ioman_read(static_cast<int>(fd), edge, static_cast<int>(tail));
        if (got < 0) {
            return total != 0 ? static_cast<int32_t>(total) : got;
        }
        sendToEe(edge, cursor, 16);
        total += static_cast<uint32_t>(got);
    }
    return static_cast<int32_t>(total);
}

// analysis §4: field order is per-handler, not a shared convention -- `open`
// carries its path at `+4` and `getstat` its destination at `+0`, both taken
// exactly as the reference lays them out. Every other in-range fno answers
// `{ result = -1 }`; an out-of-range fno gets no reply at all (IOP-5f).
void *serve(uint32_t fno, void *buffer, uint32_t) {
    if (fno >= kFnoLimit) {
        return nullptr;
    }
    auto *words = static_cast<uint32_t *>(buffer);
    answer[1] = 0;
    answer[2] = 0;
    answer[3] = 0;
    switch (fno) {
    case kFnoOpen: {
        const auto *path = reinterpret_cast<const char *>(words + 1);
        answer[0] = _import_ioman_open(path, static_cast<int>(words[0]));
        break;
    }
    case kFnoClose:
        answer[0] = _import_ioman_close(static_cast<int>(words[0]));
        break;
    case kFnoRead:
        answer[0] = doRead(words[0], words[1], words[2]);
        break;
    case kFnoLseek:
        answer[0] = _import_ioman_lseek(static_cast<int>(words[0]), static_cast<int>(words[1]),
                                        static_cast<int>(words[2]));
        break;
    case kFnoGetstat: {
        const uint32_t dest_ee = words[0];
        const auto *path = reinterpret_cast<const char *>(words + 1);
        const int result = _import_ioman_getstat(path, stat_block);
        if (result >= 0) {
            sendToEe(stat_block, dest_ee, kStatBytes);
        }
        answer[0] = result;
        break;
    }
    default:
        answer[0] = kOtherwiseUnserved;
        break;
    }
    return answer;
}

// --- the heap service, `sid 0x80000003` (analysis §1) ------------------------
//
// Its three functions are switched on the RPC `fno` itself, not on a word in
// the request, and every one of them -- including an fno it does not know --
// answers from the same four-byte cell. An unknown fno leaves that cell
// alone, so the reply carries whatever the previous call left there; that is
// the reference's own behaviour, the same acknowledged-no-op shape
// `CDVDFSV`'s two tables use (`docs/analysis/42` §5b).

constexpr uint32_t kHeapAlloc = 1;
constexpr uint32_t kHeapFree = 2;
constexpr uint32_t kHeapLoad = 3;
constexpr uint32_t kAllocLowest = 0;            // SMEM_Low [header]
constexpr int kOpenReadOnly = 1;
constexpr int kSeekSet = 0;
constexpr int kSeekEnd = 2;
constexpr uint32_t kHeapPathOffset = 4;

// fno 3: read a whole file straight into the IOP address the request names.
// No staging through the bounce buffer and no chunking, and the file's size
// is never reported back -- a refused `open` is the only failure answered.
[[nodiscard]] int32_t loadIntoHeap(const uint32_t *words) {
    const auto *path = reinterpret_cast<const char *>(
        reinterpret_cast<const uint8_t *>(words) + kHeapPathOffset);
    const int fd = _import_ioman_open(path, kOpenReadOnly);
    if (fd < 0) {
        return -1;
    }
    const int size = _import_ioman_lseek(fd, 0, kSeekEnd);
    _import_ioman_lseek(fd, 0, kSeekSet);
    (void)_import_ioman_read(fd, reinterpret_cast<void *>(words[0]), size);
    _import_ioman_close(fd);
    return 0;
}

void *serveHeap(uint32_t fno, void *buffer, uint32_t) {
    auto *words = static_cast<uint32_t *>(buffer);
    switch (fno) {
    case kHeapAlloc:
        heap_answer[0] = _import_sysmem_allocate(kAllocLowest, words[0], 0);
        break;
    case kHeapFree:
        heap_answer[0] = _import_sysmem_release(words[0]);
        break;
    case kHeapLoad:
        heap_answer[0] = loadIntoHeap(words);
        break;
    default:
        break;                                  // acknowledged, cell untouched
    }
    return heap_answer;
}

void heapThread(void *) {
    _import_sifcmd_init_rpc(0);
    _import_sifcmd_set_rpc_queue(&heap_queue,
                                 static_cast<uint32_t>(_import_thbase_get_id()));
    _import_sifcmd_register_rpc(&heap_server, kHeapServerId,
                                reinterpret_cast<void *>(serveHeap), heap_request,
                                nullptr, nullptr, &heap_queue);
    _import_sifcmd_rpc_loop(&heap_queue);
}

void serverThread(void *) {
    _import_sifcmd_init_rpc(0);
    _import_sifcmd_set_rpc_queue(&queue, static_cast<uint32_t>(_import_thbase_get_id()));
    _import_sifcmd_register_rpc(&server, kServerId, reinterpret_cast<void *>(serve),
                                request, nullptr, nullptr, &queue);
    _import_sifcmd_rpc_loop(&queue);
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

PS2_IMPORTS_BEGIN("ioman\0\0\0", 0x0102)
PS2_IMPORT(_import_ioman_open, 4)
PS2_IMPORT(_import_ioman_close, 5)
PS2_IMPORT(_import_ioman_read, 6)
PS2_IMPORT(_import_ioman_lseek, 8)
PS2_IMPORT(_import_ioman_getstat, 16)
PS2_IMPORTS_END()

PS2_IMPORTS_BEGIN("sifman\0\0", 0x0101)
PS2_IMPORT(_import_sifman_set_dma, 7)
PS2_IMPORT(_import_sifman_dma_stat, 8)
PS2_IMPORTS_END()

PS2_IMPORTS_BEGIN("intrman\0", 0x0102)
PS2_IMPORT(_import_intrman_suspend, 17)
PS2_IMPORT(_import_intrman_resume, 18)
PS2_IMPORTS_END()

PS2_IMPORTS_BEGIN("sysmem\0\0", 0x0101)
PS2_IMPORT(_import_sysmem_allocate, 4)
PS2_IMPORT(_import_sysmem_release, 5)
PS2_IMPORTS_END()

extern "C" {

[[nodiscard]] bool startThread(void (*entry)(void *), uint32_t stack_size) {
    ThreadParameters parameters;
    parameters.attr = kThreadAttr;
    parameters.option = 0;
    parameters.entry = entry;
    parameters.stack_size = stack_size;
    parameters.priority = kThreadPriority;
    const int id = _import_thbase_create(&parameters);
    if (id < 0) {
        return false;
    }
    return _import_thbase_start(static_cast<uint32_t>(id), 0) >= 0;
}

int _module_start(int, char **) {
    if (!startThread(serverThread, kThreadStack) ||
        !startThread(heapThread, kHeapThreadStack)) {
        return 1;
    }
    return 0;                                   // resident
}

}  // extern "C"
