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

// BOOT-12f: our half of the SREG file. The EE writes it with SET_SREG and
// `sifcmd` ordinals 6 and 7 read and write it locally. A title's own SIF
// bridge handshakes through register 1, both directions.
constexpr uint32_t kSregs = 32;
uint32_t sregs[kSregs];

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
              uint32_t destination, bool last, bool ends_ee_channel = true) {
    const uint32_t quads = quadwords(words);
    block.address = reinterpret_cast<uintptr_t>(from) | (last ? kTagEnd : 0);
    block.words = quads * 4;
    block.tag = (last && ends_ee_channel ? kTagDestEnd : kTagDest) | quads;
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

// sifcmd ordinals 24 and 25 (BOOT-12i), the inverses of 17 and 19. Both
// answer the record they took out -- the reference answers its predecessor
// when it had one, which no observed caller looks at -- or null when it was
// not on the list.
Server *removeRpc(Server *server, Queue *queue) {
    uint32_t state;
    _import_intrman_suspend(&state);
    Server *found = nullptr;
    for (Server **link = &servers; *link != nullptr; link = &(*link)->next) {
        if (*link == server) {
            *link = server->next;
            found = server;
            break;
        }
    }
    // The reference keeps its servers per queue and has nothing else to
    // unhook. Ours parks a pending call on the queue itself, so a request
    // waiting there would wake the queue's thread with a record its module
    // has just taken back.
    if (found != nullptr && queue != nullptr) {
        for (Server **link = &queue->pending; *link != nullptr;
             link = &(*link)->next_pending) {
            if (*link == server) {
                *link = server->next_pending;
                break;
            }
        }
        server->next_pending = nullptr;
    }
    _import_intrman_resume(state);
    return found;
}

Queue *removeRpcQueue(Queue *queue) {
    uint32_t state;
    _import_intrman_suspend(&state);
    Queue *found = nullptr;
    for (Queue **link = &queues; *link != nullptr; link = &(*link)->next) {
        if (*link == queue) {
            *link = queue->next;
            found = queue;
            break;
        }
    }
    _import_intrman_resume(state);
    return found;
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

// --- the sifman library: a module's own transfers to the EE ------------------
//
// sceSifSetDma(list, count) [header]: each entry `{ src, dest, size, attr }`
// names IOP memory to send and where in EE memory it lands. The run ends on
// the IOP side only: the EE's channel is left armed, so that the packet a
// server sends after its data (answerCall) is what ends it, as the SDK's
// client expects. sceSifDmaStat(id) answers -1 once the run has finished --
// the sending channel's own interrupt says so (irq 0x2A, the reference's
// SIFMAN number) -- and 0 while it runs.

constexpr uint32_t kSif0Irq = 0x2A;              // IOP_IRQ_DMA_SIF0 [header]
constexpr uint32_t kTransfersMax = 2;

struct Transfer {
    uint32_t src;
    uint32_t dest;
    uint32_t size;
    uint32_t attr;
};

volatile uint32_t send_running;

// sifman 32's completion callback, if the run that is finishing carried one.
// The reference keeps a table of them per batch; our sender runs one batch at
// a time, so one slot is exactly that table.
void (*send_done)(void *);
void *send_done_arg;

int sendFinished(void *) {
    send_running = 0;
    if (send_done != nullptr) {
        void (*const callback)(void *) = send_done;
        void *const arg = send_done_arg;
        send_done = nullptr;
        callback(arg);
    }
    return 1;
}

int sifSetDma(const Transfer *list, uint32_t count) {
    if (count == 0 || count > kTransfersMax || send_running) {
        return 0;                                // BOOT-12g: not queued
    }
    for (uint32_t k = 0; k < count; k++) {
        describe(send_blocks[k], reinterpret_cast<const void *>(list[k].src),
                 (list[k].size + 3) / 4, list[k].dest, k + 1 == count, false);
    }
    send_running = 1;
    send();
    return 1;
}

// sifman 32: ordinal 7 with a completion callback, called once from the
// sending channel's interrupt after the whole run has gone. A null callback
// makes it ordinal 7 exactly.
int sifSetDmaIntr(const Transfer *list, uint32_t count, void (*function)(void *),
                  void *arg) {
    if (send_running) {
        return 0;
    }
    send_done = function;
    send_done_arg = arg;
    const int answer = sifSetDma(list, count);
    if (answer == 0) {
        send_done = nullptr;
    }
    return answer;
}

int sifDmaStat(uint32_t) {
    return send_running ? 0 : -1;
}

// sifcmd ordinals 6 and 7 (BOOT-12f). The reference bounds-checks neither;
// ours does, because an out-of-range index there writes over whatever
// follows the array.
uint32_t sifGetSreg(uint32_t index) {
    return index < kSregs ? sregs[index] : 0;
}

uint32_t sifSetSreg(uint32_t index, uint32_t value) {
    if (index < kSregs) {
        sregs[index] = value;
    }
    return value;
}

// BOOT-12g: `sceSifSendCmd` and, for a caller already inside a handler,
// `isceSifSendCmd`. The packet goes out of the caller's own buffer -- the
// library fills in only `psize`, `cid` and, when there is one, the
// out-of-band block's size and destination.
constexpr uint32_t kSendPacketMin = 16;
constexpr uint32_t kSendPacketMax = 112;

int sendCmd(uint32_t cid, void *packet, uint32_t psize, const void *src_extra,
            uint32_t dest_extra, uint32_t size_extra) {
    if (psize < kSendPacketMin || psize > kSendPacketMax) {
        return 0;
    }
    // The reference chains up to 32 transfers; ours runs one at a time, so a
    // caller is told "not queued" instead and loops, which is what the
    // return value is for (docs/implementation.md).
    if (ee_packet_buffer == 0 || send_running) {
        return 0;
    }
    auto *header = static_cast<CommandHeader *>(packet);
    uint32_t blocks = 0;
    if (static_cast<int32_t>(size_extra) > 0) {
        header->size = psize | (size_extra << 8);
        header->dest = dest_extra;
        describe(send_blocks[blocks], src_extra, (size_extra + 3) / 4, dest_extra,
                 false, false);
        blocks++;
    } else {
        header->size = psize;
        header->dest = 0;
    }
    header->cid = cid;
    describe(send_blocks[blocks], packet, (psize + 3) / 4, ee_packet_buffer, true);
    send_running = 1;
    send();
    return 1;
}

int sendCmdNormal(uint32_t cid, void *packet, uint32_t psize, const void *src_extra,
                  uint32_t dest_extra, uint32_t size_extra) {
    uint32_t state;
    _import_intrman_suspend(&state);
    const int answer = sendCmd(cid, packet, psize, src_extra, dest_extra, size_extra);
    _import_intrman_resume(state);
    return answer;
}

// Ordinal 13 is ordinal 12 without the bracket: its caller is already in a
// handler, with interrupts closed by the exception.
int sendCmdInterrupt(uint32_t cid, void *packet, uint32_t psize, const void *src_extra,
                     uint32_t dest_extra, uint32_t size_extra) {
    return sendCmd(cid, packet, psize, src_extra, dest_extra, size_extra);
}

// Ordinals 28 and 29: ordinals 12 and 13 with a completion callback, which
// the reference routes through `sifman` 32 rather than 7. A caller blocks on
// a semaphore the callback signals, so a send that answers "queued" without
// ever calling back is worse than one that answers 0.
int sendCmdIntr(uint32_t cid, void *packet, uint32_t psize, const void *src_extra,
                uint32_t dest_extra, uint32_t size_extra, void (*function)(void *),
                void *arg) {
    uint32_t state;
    _import_intrman_suspend(&state);
    send_done = function;
    send_done_arg = arg;
    const int answer = sendCmd(cid, packet, psize, src_extra, dest_extra, size_extra);
    if (answer == 0) {
        send_done = nullptr;
    }
    _import_intrman_resume(state);
    return answer;
}

int sendCmdIntrInterrupt(uint32_t cid, void *packet, uint32_t psize, const void *src_extra,
                         uint32_t dest_extra, uint32_t size_extra,
                         void (*function)(void *), void *arg) {
    send_done = function;
    send_done_arg = arg;
    const int answer = sendCmd(cid, packet, psize, src_extra, dest_extra, size_extra);
    if (answer == 0) {
        send_done = nullptr;
    }
    return answer;
}

void armReceive();

// sifman ordinal 6, `sceSifSetDChain`: re-arm the receiving channel. REBOOT
// needs it because a reboot discards the channel's in-flight state
// (docs/analysis/45 §1's outside lead, and the same thing an emulator does on
// the reset command).
int sifSetDChain() {
    armReceive();
    return 0;
}

// sifman ordinals 5 and 29 (BOOT-12h). The reference's `sceSifInit` is
// idempotent through a latch and `sceSifCheckInit` reads that same latch;
// every client is written as `if (!CheckInit()) Init()`. Our entry does the
// handshake before anything can call either, so the latch is raised there and
// ordinal 5 has nothing left to do -- but both have to exist and answer,
// because the alternative is a client "initialising" a bus that is already
// carrying traffic.
uint32_t sif_initialised;

int sifInit() {
    sif_initialised = 1;
    return 0;
}

int sifCheckInit() {
    return static_cast<int>(sif_initialised);
}

// Ordinal 22 writes MSFLG and ordinal 24 SMFLG (docs/analysis/34 §0, against
// the two register addresses). From this side a write to SMFLG only ever
// *sets* (BOOT-10c), which is what makes ordinal 24 safe to call after a
// reboot; the same write to MSFLG *clears*, so ordinal 22 takes the EE's
// flags down rather than putting them up.
int sifSetMsFlag(uint32_t bits) {
    writeWord(kSifMsflg, bits);
    return 0;
}

int sifSetSmFlag(uint32_t bits) {
    writeWord(kSifSmflg, bits);
    return 0;
}

// docs/analysis/34 §5 and 45 §1: a module can claim a command id of its own
// and be called with the packet when one arrives, in the handler's own
// context. The reference keeps two such tables, one for the system range and
// one for the rest; ours is one, since a registration names its whole cid.
struct CommandHandler {
    uint32_t cid;
    void (*function)(void *packet, void *arg);
    void *arg;
};

constexpr uint32_t kCommandHandlers = 8;
CommandHandler command_handlers[kCommandHandlers];

// sifcmd ordinal 10, `sceSifAddCmdHandler(cid, handler, harg)` (analysis 45
// §1 names the argument shape from REBOOT's own call site). A cid the built-in
// dispatch already answers cannot be claimed.
int addCmdHandler(uint32_t cid, void *function, void *arg) {
    for (auto &entry : command_handlers) {
        if (entry.cid == 0 || entry.cid == cid) {
            entry.arg = arg;
            entry.function = reinterpret_cast<void (*)(void *, void *)>(function);
            entry.cid = cid;
            return 0;
        }
    }
    return -1;
}

[[nodiscard]] const CommandHandler *findCommandHandler(uint32_t cid) {
    for (const auto &entry : command_handlers) {
        if (entry.cid == cid && entry.function != nullptr) {
            return &entry;
        }
    }
    return nullptr;
}

[[gnu::used]] ps2::module::ExportTable<36> sifman_exports = {
    ps2::module::kExportMagic,
    0,
    0x0101,
    0,
    {'s', 'i', 'f', 'm', 'a', 'n', 0, 0},
    {
        ps2::module::slot(ps2::module::reservedHook),   // 0
        ps2::module::slot(ps2::module::reservedHook),   // 1
        ps2::module::slot(ps2::module::reservedHook),   // 2
        ps2::module::slot(ps2::module::reservedHook),   // 3
        ps2::module::slot(ps2::module::reservedHook),   // 4
        ps2::module::slot(sifInit),                     // 5  sceSifInit
        ps2::module::slot(sifSetDChain),                // 6  sceSifSetDChain
        ps2::module::slot(sifSetDma),                   // 7  sceSifSetDma
        ps2::module::slot(sifDmaStat),                  // 8  sceSifDmaStat
        ps2::module::slot(ps2::module::reservedHook),   // 9
        ps2::module::slot(ps2::module::reservedHook),   // 10
        ps2::module::slot(ps2::module::reservedHook),   // 11
        ps2::module::slot(ps2::module::reservedHook),   // 12
        ps2::module::slot(ps2::module::reservedHook),   // 13
        ps2::module::slot(ps2::module::reservedHook),   // 14
        ps2::module::slot(ps2::module::reservedHook),   // 15
        ps2::module::slot(ps2::module::reservedHook),   // 16
        ps2::module::slot(ps2::module::reservedHook),   // 17
        ps2::module::slot(ps2::module::reservedHook),   // 18
        ps2::module::slot(ps2::module::reservedHook),   // 19
        ps2::module::slot(ps2::module::reservedHook),   // 20
        ps2::module::slot(ps2::module::reservedHook),   // 21
        ps2::module::slot(sifSetMsFlag),                // 22 writes MSFLG
        ps2::module::slot(ps2::module::reservedHook),   // 23
        ps2::module::slot(sifSetSmFlag),                // 24 writes SMFLG
        ps2::module::slot(ps2::module::reservedHook),   // 25
        ps2::module::slot(ps2::module::reservedHook),   // 26
        ps2::module::slot(ps2::module::reservedHook),   // 27
        ps2::module::slot(ps2::module::reservedHook),   // 28
        ps2::module::slot(sifCheckInit),                // 29 sceSifCheckInit
        ps2::module::slot(ps2::module::reservedHook),   // 30
        ps2::module::slot(ps2::module::reservedHook),   // 31
        ps2::module::slot(sifSetDmaIntr),               // 32 sceSifSetDmaIntr
        ps2::module::slot(ps2::module::reservedHook),   // 33
        ps2::module::slot(ps2::module::reservedHook),   // 34
        ps2::module::slot(ps2::module::reservedHook),   // 35
        nullptr,
    },
};

// IRX-4: the library server modules import (spec/06 IOP-5f's ordinals).
[[gnu::used]] ps2::module::ExportTable<32> sifcmd_exports = {
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
        ps2::module::slot(sifGetSreg),                  // 6  sceSifGetSreg
        ps2::module::slot(sifSetSreg),                  // 7  sceSifSetSreg
        ps2::module::slot(ps2::module::reservedHook),   // 8
        ps2::module::slot(ps2::module::reservedHook),   // 9
        ps2::module::slot(addCmdHandler),               // 10 sceSifAddCmdHandler
        ps2::module::slot(ps2::module::reservedHook),   // 11
        ps2::module::slot(sendCmdNormal),               // 12 sceSifSendCmd
        ps2::module::slot(sendCmdInterrupt),            // 13 isceSifSendCmd
        ps2::module::slot(initRpc),                     // 14 sceSifInitRpc
        ps2::module::slot(ps2::module::reservedHook),   // 15
        ps2::module::slot(ps2::module::reservedHook),   // 16
        ps2::module::slot(registerRpc),                 // 17 sceSifRegisterRpc
        ps2::module::slot(ps2::module::reservedHook),   // 18
        ps2::module::slot(setRpcQueue),                 // 19 sceSifSetRpcQueue
        ps2::module::slot(ps2::module::reservedHook),   // 20
        ps2::module::slot(ps2::module::reservedHook),   // 21
        ps2::module::slot(rpcLoop),                     // 22 sceSifRpcLoop
        ps2::module::slot(ps2::module::reservedHook),   // 23
        ps2::module::slot(removeRpc),                   // 24 sceSifRemoveRpc
        ps2::module::slot(removeRpcQueue),              // 25 sceSifRemoveRpcQueue
        ps2::module::slot(ps2::module::reservedHook),   // 26
        ps2::module::slot(ps2::module::reservedHook),   // 27
        ps2::module::slot(sendCmdIntr),                 // 28 sceSifSendCmdIntr
        ps2::module::slot(sendCmdIntrInterrupt),        // 29 isceSifSendCmdIntr
        ps2::module::slot(ps2::module::reservedHook),   // 30
        ps2::module::slot(ps2::module::reservedHook),   // 31
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
    case kCidSetSreg:
        // BOOT-12f: the EE writing one word of our register file.
        (void)sifSetSreg(body[0], body[1]);
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
        if (const CommandHandler *entry = findCommandHandler(header.cid)) {
            entry->function(receive, entry->arg);
        }
        break;
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
    _import_intrman_register(kSif0Irq, 1, sendFinished, nullptr);
    _import_intrman_enable(kSif0Irq);
    writeWord(kSifSmflg, kCommandBit);           // BOOT-12b: listening
    (void)sifInit();                             // BOOT-12h: the latch 29 reads
    _import_intrman_cpu_enable();

    // The service runs in the handler; the entry returns, resident, and
    // the boot goes on (IRX-12).
    return 0;
}

}  // extern "C"
