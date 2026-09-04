// SIFCMD: the packets that cross the bus once the two processors have met,
// and the RPC layer built on them.
//
// docs/spec/03-boot-chain.md BOOT-12. Packets land at the receive buffer this
// module publishes in SMCOM, each announced by the receiving channel's
// interrupt and served inside its handler, as the reference's `SIFCMD` serves
// them. Two kinds are answered here: the system commands the SDK's client
// sends to bring its RPC layer up (BOOT-12c), and the RPC requests that layer
// then makes of a registered server (BOOT-12d). Anything else is offered to
// the handlers modules claim through ordinal 10 -- which is how `EESYNC`'s
// file service and `REBOOT`'s reset command are reached.
//
// The bytes themselves are `SIFMAN`'s: everything outgoing goes through
// ordinals 7 and 32, and the handshake through ordinal 5.

#include "module.hpp"
#include "sif.hpp"
#include "sifrpc.hpp"

#include <stdint.h>

namespace {

using ps2::sif::Transfer;

// BOOT-12a: a command packet is at most 112 bytes; the buffer is the SDK's
// size for one.
constexpr uint32_t kPacketBytes = 128;

// BOOT-12: the command ids this service answers, the SDK's system ones.
constexpr uint32_t kCidSystem = 0x80000000;
constexpr uint32_t kCidChangeAddress = kCidSystem | 0;
constexpr uint32_t kCidSetSreg = kCidSystem | 1;
constexpr uint32_t kCidInitCmd = kCidSystem | 2;
constexpr uint32_t kCidRpcEnd = kCidSystem | 8;
constexpr uint32_t kCidRpcBind = kCidSystem | 9;
constexpr uint32_t kCidRpcCall = kCidSystem | 10;

constexpr uint32_t kSif1Irq = 0x2B;              // IOP_IRQ_DMA_SIF1 [header]

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

// BOOT-12d: the servers modules register through this library
// (src/iop/sifrpc.hpp), and the queues whose threads answer their calls.
using ps2::sifrpc::Queue;
using ps2::sifrpc::Server;
Server *servers;
Queue *queues;

alignas(16) uint8_t receive[kPacketBytes];  // published in SMCOM: packets land here

// What this module sends of its own: an END packet or a SET_SREG, never both
// at once, since every one of them is built and queued without leaving the
// handler or the thread that built it.
alignas(16) uint32_t outgoing[16];

uint32_t ee_packet_buffer;                 // BOOT-12c: where the EE's client listens

// BOOT-12f: our half of the SREG file. The EE writes it with SET_SREG and
// ordinals 6 and 7 read and write it locally. A title's own SIF bridge
// handshakes through register 1, both directions.
constexpr uint32_t kSregs = 32;
uint32_t sregs[kSregs];

extern "C" {
int _import_thbase_sleep();
int _import_thbase_iwakeup(uint32_t id);
int _import_intrman_register(uint32_t irq, uint32_t mode, int (*handler)(void *), void *arg);
int _import_intrman_enable(uint32_t irq);
int _import_intrman_cpu_enable();
int _import_intrman_suspend(uint32_t *state);
int _import_intrman_resume(uint32_t state);
int _import_sifman_init();
int _import_sifman_set_dchain();
int _import_sifman_set_dma(const Transfer *list, uint32_t count);
int _import_sifman_set_dma_intr(const Transfer *list, uint32_t count,
                                void (*function)(void *), void *arg);
int _import_loadcore_register(void *table);
}

void writeWord(uintptr_t address, uint32_t value) {
    *reinterpret_cast<volatile uint32_t *>(address) = value;
}

// The DMA controller writes `receive` behind the compiler's back; this keeps
// the compiler from reading it before the mark that says a packet is there.
void barrier() {
    asm volatile("" ::: "memory");
}

// One transfer of `words` from IOP memory to `destination` in EE memory,
// ending the EE's channel when it is the last of its run.
void describe(Transfer &transfer, const void *from, uint32_t words,
              uint32_t destination, bool ends_ee_channel) {
    transfer.src = reinterpret_cast<uintptr_t>(from);
    transfer.dest = destination;
    transfer.size = words * 4;
    transfer.attr = ends_ee_channel ? ps2::sif::kAttrEndEe : 0;
}

// BOOT-11i: arm the receiving channel before waiting to be told anything. The
// EE's transfer runs only once this end is ready for it, so arming after a
// packet is due would leave each side waiting. The size byte is cleared
// first: it is the mark a landing packet sets.
void armReceive() {
    writeWord(reinterpret_cast<uintptr_t>(receive), 0);
    (void)_import_sifman_set_dchain();
}

// BOOT-12c: the answer that completes the client's `SifInitRpc` -- SET_SREG
// of register 0 to 1, as a command packet to the buffer the client named.
void sendSetSreg(uint32_t index, uint32_t value) {
    if (ee_packet_buffer == 0) {
        return;                                  // nowhere to answer yet
    }
    auto *header = reinterpret_cast<CommandHeader *>(outgoing);
    header->size = sizeof(CommandHeader) + 8;    // psize 0x18, no dsize
    header->dest = 0;
    header->cid = kCidSetSreg;
    header->opt = 0;
    outgoing[4] = index;
    outgoing[5] = value;
    Transfer transfer;
    describe(transfer, outgoing, 6, ee_packet_buffer, true);
    (void)_import_sifman_set_dma(&transfer, 1);
}

// --- BOOT-12d: the RPC layer -------------------------------------------------
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

// Ordinals 24 and 25 (BOOT-12i), the inverses of 17 and 19. Both answer the
// record they took out -- the reference answers its predecessor when it had
// one, which no observed caller looks at -- or null when it was not on the
// list.
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
    auto &end = *reinterpret_cast<RpcEnd *>(outgoing);
    fillEnd(end, request, kCidRpcBind, findServer(request.sid));
    Transfer transfer;
    describe(transfer, outgoing, sizeof(RpcEnd) / 4, ee_packet_buffer, true);
    (void)_import_sifman_set_dma(&transfer, 1);
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
    Transfer run[2];
    uint32_t blocks = 0;
    if (answer != nullptr && server.receive != 0 && server.receive_size != 0) {
        describe(run[blocks], answer, (server.receive_size + 3) / 4,
                 server.receive, server.mode == 0);
        blocks++;
    }
    if (server.mode != 0) {
        RpcBind request;
        request.rec_id = server.rec_id;
        request.pkt_addr = server.pkt_addr;
        request.rpc_id = server.rpc_id;
        request.client = server.client;
        auto &end = *reinterpret_cast<RpcEnd *>(outgoing);
        fillEnd(end, request, kCidRpcCall, &server);
        describe(run[blocks], outgoing, sizeof(RpcEnd) / 4, ee_packet_buffer, true);
        blocks++;
    }
    if (blocks != 0) {
        (void)_import_sifman_set_dma(run, blocks);
    }
}

// Ordinals 6 and 7 (BOOT-12f). The reference bounds-checks neither; ours
// does, because an out-of-range index there writes over whatever follows the
// array.
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
            uint32_t dest_extra, uint32_t size_extra,
            void (*done)(void *) = nullptr, void *done_arg = nullptr) {
    if (psize < kSendPacketMin || psize > kSendPacketMax) {
        return 0;
    }
    if (ee_packet_buffer == 0) {
        return 0;
    }
    auto *header = static_cast<CommandHeader *>(packet);
    Transfer run[2];
    uint32_t blocks = 0;
    if (static_cast<int32_t>(size_extra) > 0) {
        header->size = psize | (size_extra << 8);
        header->dest = dest_extra;
        describe(run[blocks], src_extra, (size_extra + 3) / 4, dest_extra, false);
        blocks++;
    } else {
        header->size = psize;
        header->dest = 0;
    }
    header->cid = cid;
    describe(run[blocks], packet, (psize + 3) / 4, ee_packet_buffer, true);
    blocks++;
    return _import_sifman_set_dma_intr(run, blocks, done, done_arg);
}

int sendCmdNormal(uint32_t cid, void *packet, uint32_t psize, const void *src_extra,
                  uint32_t dest_extra, uint32_t size_extra) {
    return sendCmd(cid, packet, psize, src_extra, dest_extra, size_extra);
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
    return sendCmd(cid, packet, psize, src_extra, dest_extra, size_extra, function, arg);
}

int sendCmdIntrInterrupt(uint32_t cid, void *packet, uint32_t psize, const void *src_extra,
                         uint32_t dest_extra, uint32_t size_extra,
                         void (*function)(void *), void *arg) {
    return sendCmd(cid, packet, psize, src_extra, dest_extra, size_extra, function, arg);
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

// Ordinal 10, `sceSifAddCmdHandler(cid, handler, harg)` (analysis 45 §1 names
// the argument shape from REBOOT's own call site). A cid the built-in
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

// IRX-4: the library server modules import (spec/06 IOP-5f's ordinals).
PS2_EXPORT_TABLE ps2::module::ExportTable<32> sifcmd_exports = {
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
    default:
        if (const CommandHandler *entry = findCommandHandler(header.cid)) {
            entry->function(receive, entry->arg);
        }
        break;
    }
    armReceive();
    return 1;
}

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

PS2_IMPORTS_BEGIN("sifman\0\0", 0x0101)
PS2_IMPORT(_import_sifman_init, 5)
PS2_IMPORT(_import_sifman_set_dchain, 6)
PS2_IMPORT(_import_sifman_set_dma, 7)
PS2_IMPORT(_import_sifman_set_dma_intr, 32)
PS2_IMPORTS_END()

PS2_IMPORTS_BEGIN("loadcore", 0x0101)
PS2_IMPORT(_import_loadcore_register, 6)
PS2_IMPORTS_END()

extern "C" {

// The module's entry, called by the loader as entry(argc, argv, 0, record)
// (spec/02 IRX-10).
int _module_start(int, char **) {
    // IRX-10a: registered before anything else runs.
    if (_import_loadcore_register(&sifcmd_exports) < 0) {
        return 1;
    }

    // BOOT-10b: the address the EE is to send packets to is published before
    // the handshake announces that this side is up. The reference reaches
    // SMCOM through a `sifman` ordinal this project has not decoded, so the
    // register is written here, where the buffer is (docs/implementation.md).
    writeWord(ps2::sif::kSmcom, reinterpret_cast<uintptr_t>(receive));

    // BOOT-10a: wait for the EE to raise its bit. Nothing else can proceed
    // until it does -- neither side may go on alone.
    (void)_import_sifman_init();

    // BOOT-11i and BOOT-12b: the receiver armed and the interrupt that
    // announces its packets wired up. The bit that tells the EE anyone is
    // listening is not raised here: `EESYNC` raises it, once the boot list
    // has run out and every service that answers a packet exists.
    armReceive();
    _import_intrman_register(kSif1Irq, 1, servePacket, nullptr);
    _import_intrman_enable(kSif1Irq);
    _import_intrman_cpu_enable();

    // The service runs in the handler; the entry returns, resident, and
    // the boot goes on (IRX-12).
    return 0;
}

}  // extern "C"
