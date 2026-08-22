// EESYNC: the IOP's half of the meeting with the EE, and the command service
// the EE talks to across the bus once they have met.
//
// docs/spec/03-boot-chain.md BOOT-10. The IOP boot does not end at the last
// module of the list; it ends waiting for the EE, which is why this module is
// last in `IOPBTCONF`. The EE publishes an address and raises a bit in MSFLG;
// this answers with an address of its own and a bit in SMFLG, then clears what
// the EE raised.
//
// BOOT-10c is the part worth being careful about: a write from this side
// *clears* bits in MSFLG and *sets* them in SMFLG, and the EE's writes do the
// reverse. Registers that merely stored would livelock rather than fail.
//
// What arrives afterwards is BOOT-12's command packets, at the receive buffer
// this module published, each announced by the receiving channel's interrupt
// and served inside its handler, as the reference's `SIFCMD` serves them.
// Three kinds of command are answered: the system commands the SDK's client sends to bring its RPC
// layer up (BOOT-12c), the RPC requests that layer then makes of a server
// (BOOT-12d -- one server, the module loader's id, which cannot load anything
// yet), and a command of our own by which the kernel asks for a file out of
// the archive (docs/implementation.md) -- the reference serves `rom0:`
// through ROMDRV over that same RPC, which does not exist here yet.

#include "module.hpp"
#include "sifrpc.hpp"

#include <stdint.h>

namespace {

constexpr uintptr_t kSifMscom = 0xBD000000;
constexpr uintptr_t kSifSmcom = 0xBD000010;
constexpr uintptr_t kSifMsflg = 0xBD000020;
constexpr uintptr_t kSifSmflg = 0xBD000030;
constexpr uintptr_t kSifCtrl = 0xBD000040;

constexpr uint32_t kHandshakeBit = 0x00010000;  // BOOT-10: SIF_STAT_SIFINIT
constexpr uint32_t kCommandBit = 0x00020000;    // BOOT-12b: SIF_STAT_CMDINIT

// BOOT-11f: the control register gates the two data paths, one bit each, and
// an emulator will move nothing across a path whose bit is clear. They are set
// one at a time, the way the reference's driver sets them, and the bit is
// consumed by the transfer it enables, so it is raised again for each one.
constexpr uint32_t kCtrlSif0Path = 0x00000020;
constexpr uint32_t kCtrlSif1Path = 0x00000040;

// The IOP's SIF DMA channels, in the second controller's bank. Which channels
// these are was derived by watching the reference's driver rather than assumed
// (docs/analysis/24-sif-data-path.md).
constexpr uintptr_t kDmaSif0 = 0xBF801520;      // to the EE
constexpr uintptr_t kDmaSif1 = 0xBF801530;      // from the EE
constexpr uintptr_t kMadr = 0x0;
constexpr uintptr_t kBcr = 0x4;
constexpr uintptr_t kChcr = 0x8;
constexpr uintptr_t kTadr = 0xC;

// BOOT-11e: starting a channel takes more than the busy bit. Each carries its
// direction and sync mode, and the block size the SIF moves in has to be in
// BCR -- values read off the reference's own driver, because an emulator
// enforces them where a lenient simulator does not.
constexpr uint32_t kDmaSendChcr = 0x01000701;   // ch9:  from memory, started
constexpr uint32_t kDmaRecvChcr = 0x41000300;   // ch10: to memory, started
constexpr uint32_t kDmaBusy = 0x01000000;
constexpr uint32_t kDmaBlock = 0x00000020;      // 32 words, the SIF's granularity

// BOOT-11j: the controller has a per-channel enable of its own in the second
// bank's DPCR, and a global enable above those which the reference's driver
// toggles around its critical sections and leaves set. A channel whose nibble
// is clear does not run, however its own CHCR is programmed.
constexpr uintptr_t kDmaDpcr2 = 0xBF801570;
constexpr uint32_t kDmaDpcr2All = 0x0777FF77;   // what the reference leaves there
constexpr uintptr_t kDmaDmacen = 0xBF801578;

// BOOT-11: what crosses the bus is framed. An incoming packet carries the
// address it lands at, so this end supplies none; an outgoing one is described
// by a 16-byte send block at TADR that also carries, ready-made, the tag the
// EE's channel pops to learn where the bytes go. A run of send blocks is one
// transfer to the EE's channel, which stops on the last block's tag.
constexpr uint32_t kTagEnd = 0x80000000;        // last send block of this run
constexpr uint32_t kTagDest = 0x10000000;       // an EE destination tag: `cnt`
constexpr uint32_t kTagDestEnd = 0x90000000;    // `cnt` with the interrupt bit,
                                                // which is what the reference
                                                // writes on a packet

// What the boot block left for us, at the address `src/boot/iopboot.S` fixes.
constexpr uintptr_t kBootList = 0x001F8100;
constexpr uintptr_t kBootListTable = 0x008;

constexpr uint32_t kNameLength = 10;            // spec/01 ARC-2
constexpr uint32_t kPayloadWords = 4096;        // up to 16 KiB per answer
constexpr uint32_t kVerbSize = 0;               // how big is it?
constexpr uint32_t kVerbContent = 1;            // send this window of it

// BOOT-12a: a command packet is at most 112 bytes; the buffer is the SDK's
// size for one.
constexpr uint32_t kPacketBytes = 128;

// BOOT-12: the command ids this service answers. The system ones are the
// SDK's; the file command is ours and `src/kernel/sif.cpp` names the same
// number.
constexpr uint32_t kCidSystem = 0x80000000;
constexpr uint32_t kCidChangeAddress = kCidSystem | 0;
constexpr uint32_t kCidSetSreg = kCidSystem | 1;
constexpr uint32_t kCidInitCmd = kCidSystem | 2;
constexpr uint32_t kCidRpcEnd = kCidSystem | 8;
constexpr uint32_t kCidRpcBind = kCidSystem | 9;
constexpr uint32_t kCidRpcCall = kCidSystem | 10;
constexpr uint32_t kCidFile = 0x10;

// BOOT-12d: the module loader's server, the one the SDK's `SifLoadModule`
// binds. Its request is 512 bytes -- the argument length, the result, the
// path at +8 and the arguments at +0x104 -- and its answer two words, the
// module id (or an error) and the module's own return. The reference's
// loader answers a name it has no file for with -203 (docs/analysis/34 §6).

// BOOT-12a: the sixteen bytes every packet begins with. `size` is `psize` in
// its low byte and `dsize` above it.
struct CommandHeader {
    uint32_t size;
    uint32_t dest;
    uint32_t cid;
    uint32_t opt;
};
static_assert(sizeof(CommandHeader) == 16);

// BOOT-12d: the RPC packets, as the SDK's client lays them out after the
// header. Every one carries the client's three words first.
struct RpcBind {
    CommandHeader header;
    uint32_t rec_id;
    uint32_t pkt_addr;
    uint32_t rpc_id;
    uint32_t client;        // the client's record, handed back in the answer
    uint32_t sid;
};

struct RpcCall {
    CommandHeader header;
    uint32_t rec_id;
    uint32_t pkt_addr;
    uint32_t rpc_id;
    uint32_t client;
    uint32_t function;
    uint32_t send_size;
    uint32_t receive;       // in EE memory: where the result goes
    uint32_t receive_size;
    uint32_t mode;          // 0: no END wanted
    uint32_t server;
};

struct RpcEnd {
    CommandHeader header;
    uint32_t rec_id;
    uint32_t pkt_addr;
    uint32_t rpc_id;
    uint32_t client;
    uint32_t cid;           // which request this answers
    uint32_t server;
    uint32_t buffer;
    uint32_t cbuffer;
};
static_assert(sizeof(RpcEnd) == 48);

// The kernel's file request, as the EE lays it out after the header
// (`src/kernel/sif.cpp`): eight words.
struct Request {
    char name[kNameLength];
    uint16_t pad;
    uint32_t verb;
    uint32_t destination;   // in EE memory, physical
    uint32_t offset;        // kVerbContent: first byte wanted
    uint32_t length;        // kVerbContent: how many, 0 for as many as fit
    uint32_t reserved;
};
static_assert(sizeof(Request) == 32);

// What our outgoing channel reads at TADR: where the bytes are and how many,
// then the tag the EE's channel pops.
struct SendBlock {
    uint32_t address;       // | kTagEnd on the last of a run
    uint32_t words;
    uint32_t tag;           // kTagDest[End] | quadwords
    uint32_t destination;
};

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

// BOOT-12d: the servers modules register through the `sifcmd` library
// (src/iop/sifrpc.hpp), and the queues whose threads answer their calls.
using ps2::sifrpc::Queue;
using ps2::sifrpc::Server;
Server *servers;
Queue *queues;

alignas(16) uint8_t receive[kPacketBytes];  // published in SMCOM: packets land here
alignas(16) SendBlock send_blocks[2];
alignas(16) uint32_t payload[kPayloadWords];
uint32_t ee_area;                          // what the EE published at the handshake
uint32_t ee_packet_buffer;                 // BOOT-12c: where the EE's client listens

extern "C" {
int _import_thbase_sleep();
int _import_thbase_iwakeup(uint32_t id);
int _import_intrman_suspend(uint32_t *state);
int _import_intrman_resume(uint32_t state);
}

[[nodiscard]] uint32_t readWord(uintptr_t address) {
    return *reinterpret_cast<volatile uint32_t *>(address);
}

void writeWord(uintptr_t address, uint32_t value) {
    *reinterpret_cast<volatile uint32_t *>(address) = value;
}

// The DMA controller writes `receive` behind the compiler's back; this keeps
// the compiler from reading it before the mark that says a packet is there.
void barrier() {
    asm volatile("" ::: "memory");
}

void waitUntilSet(uintptr_t address, uint32_t bits) {
    while ((readWord(address) & bits) == 0) {
    }
}

[[nodiscard]] uint32_t alignedSize(uint32_t size) {
    return (size + 15) & ~uint32_t{15};
}

[[nodiscard]] uint32_t quadwords(uint32_t words) {
    return (words + 3) / 4;
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

// One send block: `words` of `from` to `destination` in EE memory, tagged so
// the EE's channel stops there or goes on. BOOT-11k: the EE's client names
// its buffers through an uncached alias, and what goes into the tag has to be
// the physical address. The bus moves quadwords and the EE's channel counts
// them, so a short run is padded up to one; every source here has room.
void describe(SendBlock &block, const void *from, uint32_t words,
              uint32_t destination, bool last) {
    const uint32_t quads = quadwords(words);
    block.address = reinterpret_cast<uintptr_t>(from) | (last ? kTagEnd : 0);
    block.words = quads * 4;
    block.tag = (last ? kTagDestEnd : kTagDest) | quads;
    block.destination = destination & 0x1FFFFFFF;
}

// Start the run of send blocks at `send_blocks`: BOOT-11c, then the channel.
void send() {
    writeWord(kSifCtrl, kCtrlSif0Path);          // BOOT-11f, for this path
    barrier();
    writeWord(kDmaSif0 + kTadr, reinterpret_cast<uintptr_t>(send_blocks));
    writeWord(kDmaSif0 + kBcr, kDmaBlock);
    writeWord(kDmaSif0 + kChcr, kDmaSendChcr);   // start
}

// Send `words` of the payload to `destination`, as one packet.
void sendPayload(uint32_t words, uint32_t destination) {
    describe(send_blocks[0], payload, words, destination, true);
    send();
}

// BOOT-12c: the answer that completes the client's `SifInitRpc` -- SET_SREG
// of register 0 to 1, as a command packet to the buffer the client named.
void sendSetSreg(uint32_t index, uint32_t value) {
    if (ee_packet_buffer == 0) {
        return;                                  // nowhere to answer yet
    }
    auto *header = reinterpret_cast<CommandHeader *>(payload);
    header->size = sizeof(CommandHeader) + 8;    // psize 0x18, no dsize
    header->dest = 0;
    header->cid = kCidSetSreg;
    header->opt = 0;
    payload[4] = index;
    payload[5] = value;
    sendPayload(6, ee_packet_buffer);
}

// The kernel's file request: a name, a verb, and where the answer goes.
void serveFile(const Request &request) {
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
    sendPayload(words, request.destination);
}

// --- BOOT-12d: the RPC layer, as the `sifcmd` library ------------------------
//
// IOP-5f: a server module registers a queue bound to its own thread and a
// server on that queue, then calls RpcLoop, which sleeps until the handler
// has queued a call and answers it from the thread -- where a server's
// function may do what an interrupt handler may not, such as create threads.

[[nodiscard]] Server *findServer(uint32_t sid) {
    for (Server *server = servers; server != nullptr; server = server->next) {
        if (server->sid == sid) {
            return server;
        }
    }
    return nullptr;
}

int initRpc(uint32_t) {
    return 0;
}

int setRpcQueue(Queue *queue, uint32_t thread_id) {
    queue->thread_id = thread_id;
    queue->pending = nullptr;
    queue->next = queues;
    queues = queue;
    return 0;
}

int registerRpc(Server *server, uint32_t sid, void *function, void *buffer,
                void *cfunction, void *cbuffer, Queue *queue) {
    server->sid = sid;
    server->function = reinterpret_cast<void *(*)(uint32_t, void *, uint32_t)>(function);
    server->buffer = buffer;
    server->cfunction = reinterpret_cast<void *(*)(uint32_t, void *, uint32_t)>(cfunction);
    server->cbuffer = cbuffer;
    server->queue = queue;
    server->next_pending = nullptr;
    server->next = servers;
    servers = server;
    return 0;
}

void answerCall(Server &server);

// The queue's thread lives here: asleep until the handler wakes it with a
// call to answer, then back to sleep. Never returns.
int rpcLoop(Queue *queue) {
    for (;;) {
        uint32_t state;
        _import_intrman_suspend(&state);
        Server *server = queue->pending;
        if (server != nullptr) {
            queue->pending = server->next_pending;
            server->next_pending = nullptr;
        }
        _import_intrman_resume(state);
        if (server == nullptr) {
            _import_thbase_sleep();
            continue;
        }
        answerCall(*server);
    }
}

// The END packet every request is answered with, to the client's buffer.
// `cid` names the request; an unknown server is answered with none, which the
// SDK's client reads as "not bound" and asks again.
void fillEnd(RpcEnd &end, const RpcBind &request, uint32_t cid, const Server *server) {
    end.header.size = sizeof(RpcEnd);
    end.header.dest = 0;
    end.header.cid = kCidRpcEnd;
    end.header.opt = 0;
    end.rec_id = request.rec_id;
    end.pkt_addr = request.pkt_addr;
    end.rpc_id = request.rpc_id;
    end.client = request.client;
    end.cid = cid;
    end.server = server != nullptr ? reinterpret_cast<uintptr_t>(server) : 0;
    end.buffer = server != nullptr ? reinterpret_cast<uintptr_t>(server->buffer) : 0;
    end.cbuffer = end.buffer;
}

void serveBind(const RpcBind &request) {
    if (ee_packet_buffer == 0) {
        return;
    }
    auto &end = *reinterpret_cast<RpcEnd *>(payload);
    fillEnd(end, request, kCidRpcBind, findServer(request.sid));
    sendPayload(sizeof(RpcEnd) / 4, ee_packet_buffer);
}

// A call: the arguments have already landed in the server's buffer, ahead of
// this packet. The request is kept on the server and queued for its thread,
// which is woken to answer it (IOP-5f); nothing runs here but the wake.
void serveCall(const RpcCall &request) {
    auto *server = reinterpret_cast<Server *>(request.server);
    if (server == nullptr || server->queue == nullptr) {
        return;
    }
    server->client = request.client;
    server->rec_id = request.rec_id;
    server->pkt_addr = request.pkt_addr;
    server->rpc_id = request.rpc_id;
    server->fno = request.function;
    server->send_size = request.send_size;
    server->receive = request.receive;
    server->receive_size = request.receive_size;
    server->mode = request.mode;
    Queue &queue = *server->queue;
    server->next_pending = nullptr;
    if (queue.pending == nullptr) {
        queue.pending = server;
    } else {
        Server *tail = queue.pending;
        while (tail->next_pending != nullptr) {
            tail = tail->next_pending;
        }
        tail->next_pending = server;
    }
    _import_thbase_iwakeup(queue.thread_id);
}

// On the queue's thread: the function's answer goes to the client's receive
// buffer and the END packet after it, in one run, so that the EE's channel
// stops -- and its handler runs -- only once both are there.
void answerCall(Server &server) {
    void *answer = server.function(server.fno, server.buffer, server.send_size);
    if (ee_packet_buffer == 0) {
        return;
    }
    uint32_t blocks = 0;
    if (answer != nullptr && server.receive != 0 && server.receive_size != 0) {
        describe(send_blocks[blocks], answer, (server.receive_size + 3) / 4,
                 server.receive, server.mode == 0);
        blocks++;
    }
    if (server.mode != 0) {
        RpcBind request;
        request.rec_id = server.rec_id;
        request.pkt_addr = server.pkt_addr;
        request.rpc_id = server.rpc_id;
        request.client = server.client;
        auto &end = *reinterpret_cast<RpcEnd *>(payload);
        fillEnd(end, request, kCidRpcCall, &server);
        describe(send_blocks[blocks], payload, sizeof(RpcEnd) / 4,
                 ee_packet_buffer, true);
        blocks++;
    }
    if (blocks != 0) {
        send();
    }
}

// IRX-4: the library server modules import (spec/06 IOP-5f's ordinals).
[[gnu::used]] ps2::module::ExportTable<23> sifcmd_exports = {
    ps2::module::kExportMagic,
    0,
    0x0101,
    0,
    {'s', 'i', 'f', 'c', 'm', 'd', 0, 0},
    {
        ps2::module::slot(ps2::module::reservedHook),   // 0
        ps2::module::slot(ps2::module::reservedHook),   // 1
        ps2::module::slot(ps2::module::reservedHook),   // 2
        ps2::module::slot(ps2::module::reservedHook),   // 3
        ps2::module::slot(ps2::module::reservedHook),   // 4
        ps2::module::slot(ps2::module::reservedHook),   // 5
        ps2::module::slot(ps2::module::reservedHook),   // 6
        ps2::module::slot(ps2::module::reservedHook),   // 7
        ps2::module::slot(ps2::module::reservedHook),   // 8
        ps2::module::slot(ps2::module::reservedHook),   // 9
        ps2::module::slot(ps2::module::reservedHook),   // 10
        ps2::module::slot(ps2::module::reservedHook),   // 11
        ps2::module::slot(ps2::module::reservedHook),   // 12
        ps2::module::slot(ps2::module::reservedHook),   // 13
        ps2::module::slot(initRpc),                     // 14 sceSifInitRpc
        ps2::module::slot(ps2::module::reservedHook),   // 15
        ps2::module::slot(ps2::module::reservedHook),   // 16
        ps2::module::slot(registerRpc),                 // 17 sceSifRegisterRpc
        ps2::module::slot(ps2::module::reservedHook),   // 18
        ps2::module::slot(setRpcQueue),                 // 19 sceSifSetRpcQueue
        ps2::module::slot(ps2::module::reservedHook),   // 20
        ps2::module::slot(ps2::module::reservedHook),   // 21
        ps2::module::slot(rpcLoop),                     // 22 sceSifRpcLoop
        nullptr,
    },
};

// BOOT-11i: arm the receiving channel before waiting to be told anything. The
// EE's transfer runs only once this end is ready for it, so arming after a
// packet is due would leave each side waiting. The size byte is cleared first:
// it is the mark a landing packet sets.
void armReceive() {
    writeWord(reinterpret_cast<uintptr_t>(receive), 0);
    writeWord(kSifCtrl, kCtrlSif1Path);          // BOOT-11f, for this path
    writeWord(kDmaSif1 + kBcr, kDmaBlock);
    writeWord(kDmaSif1 + kChcr, kDmaRecvChcr);
}

// The receiving channel's interrupt (spec/06 IOP-2h: the second DMA bank's
// channel 10, irq 0x2B -- the number the reference's SIFCMD registers,
// docs/analysis/34 §1): a packet has landed whole, since the channel stops
// on the end bit the sender put in its header (BOOT-11a). Everything the
// packet asks for is done here, in the handler, the way the reference's
// SIFCMD does it; returning 1 keeps the line enabled (IOP-2g).
int servePacket(void *) {
    barrier();
    const auto &header = *reinterpret_cast<const CommandHeader *>(receive);
    const uint32_t *body = reinterpret_cast<const uint32_t *>(
        receive + sizeof(CommandHeader));
    switch (header.cid) {
    case kCidInitCmd:
        // BOOT-12c: `opt` 0 carries the client's receive buffer; `opt` 1
        // is the RPC layer asking to be told it may start.
        if (header.opt == 0) {
            ee_packet_buffer = body[0];
        } else {
            sendSetSreg(0, 1);
        }
        break;
    case kCidChangeAddress:
        ee_packet_buffer = body[0];
        break;
    case kCidRpcBind:
        serveBind(*reinterpret_cast<const RpcBind *>(receive));
        break;
    case kCidRpcCall:
        serveCall(*reinterpret_cast<const RpcCall *>(receive));
        break;
    case kCidFile:
        serveFile(*reinterpret_cast<const Request *>(body));
        break;
    default:
        break;                                   // nothing registered for it
    }
    armReceive();
    return 1;
}

constexpr uint32_t kSif1Irq = 0x2B;              // IOP_IRQ_DMA_SIF1 [header]

}  // namespace

PS2_IMPORTS_BEGIN("intrman\0", 0x0102)
PS2_IMPORT(_import_intrman_register, 4)
PS2_IMPORT(_import_intrman_enable, 6)
PS2_IMPORT(_import_intrman_cpu_enable, 9)
PS2_IMPORT(_import_intrman_suspend, 17)
PS2_IMPORT(_import_intrman_resume, 18)
PS2_IMPORTS_END()

PS2_IMPORTS_BEGIN("thbase\0\0", 0x0101)
PS2_IMPORT(_import_thbase_sleep, 24)
PS2_IMPORT(_import_thbase_iwakeup, 26)
PS2_IMPORTS_END()

extern "C" {
int _import_intrman_register(uint32_t irq, uint32_t mode, int (*handler)(void *), void *arg);
int _import_intrman_enable(uint32_t irq);
int _import_intrman_cpu_enable();
}

extern "C" {

// The module's entry, called by the loader as entry(argc, argv, 0, record)
// (spec/02 IRX-10).
int _module_start(int, char **) {
    // BOOT-10a: wait for the EE to raise its bit. Nothing else can proceed
    // until it does -- neither side may go on alone.
    waitUntilSet(kSifMsflg, kHandshakeBit);

    // BOOT-11j: enable the second bank's channels before anything uses them.
    writeWord(kDmaDpcr2, kDmaDpcr2All);
    writeWord(kDmaDmacen, 1);

    // BOOT-10b: answer with an address in our own RAM and a bit of our own,
    // and BOOT-11f: open both data paths first, one bit per write.
    writeWord(kSifCtrl, kCtrlSif0Path);
    writeWord(kSifCtrl, kCtrlSif1Path);
    writeWord(kSifSmcom, reinterpret_cast<uintptr_t>(receive));
    writeWord(kSifSmflg, kHandshakeBit);         // our write sets

    // Record what the EE published, then clear its flag: our write to MSFLG
    // is an acknowledgement, not a request.
    ee_area = readWord(kSifMscom);
    writeWord(kSifMsflg, kHandshakeBit);         // our write to MSFLG clears

    // BOOT-11i and BOOT-12b: the receiver armed and the interrupt that
    // announces its packets wired up before the EE is told anyone listens.
    armReceive();
    _import_intrman_register(kSif1Irq, 1, servePacket, nullptr);
    _import_intrman_enable(kSif1Irq);
    writeWord(kSifSmflg, kCommandBit);           // BOOT-12b: listening
    _import_intrman_cpu_enable();

    // The service runs in the handler; the entry returns, resident, and
    // the boot goes on (IRX-12).
    return 0;
}

}  // extern "C"
