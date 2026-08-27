// The EE's SIF client, shared by the programs the boot runs.
//
// See `sifclient.hpp` for why this is a translation unit rather than a header
// of `static` functions. Everything below the exported three is internal: the
// command packets' shapes, the one packet buffer the IOP answers into, and the
// polling that stands in for the SDK's channel-5 handler.

#include "sifclient.hpp"

namespace ps2::sifclient {
namespace {

// The two SIF registers the handshake runs through, and the bit in SMFLG the
// IOP sets once its own command layer is up (BOOT-12b).
constexpr uint32_t kRegSmcom = 2;
constexpr uint32_t kRegSmflg = 4;
constexpr uint32_t kStatCmdInit = 0x20000;

// --- the command layer (spec/03 BOOT-12) ---------------------------------------

struct CommandHeader {
    uint32_t size;                              // psize | dsize << 8
    uint32_t dest;
    uint32_t cid;
    uint32_t opt;
};

struct Transfer {                               // SYS-13c
    uint32_t src;
    uint32_t dest;
    uint32_t size;
    uint32_t attr;
};
constexpr uint32_t kAttrEnd = 0x40;
constexpr uint32_t kAttrInterruptIop = 0x04;

constexpr uint32_t kCidSystem = 0x80000000;
constexpr uint32_t kCidSetSreg = kCidSystem | 1;
constexpr uint32_t kCidInitCmd = kCidSystem | 2;
constexpr uint32_t kCidRpcEnd = kCidSystem | 8;
constexpr uint32_t kCidRpcBind = kCidSystem | 9;
constexpr uint32_t kCidRpcCall = kCidSystem | 10;

struct RpcBind {
    CommandHeader header;
    uint32_t rec_id;
    uint32_t pkt_addr;
    uint32_t rpc_id;
    uint32_t client;
    uint32_t sid;
};

struct RpcCall {
    CommandHeader header;
    uint32_t rec_id;
    uint32_t pkt_addr;
    uint32_t rpc_id;
    uint32_t client;
    uint32_t fno;
    uint32_t send_size;
    uint32_t receive;
    uint32_t receive_size;
    uint32_t mode;
    uint32_t server;
};

struct RpcEnd {
    CommandHeader header;
    uint32_t rec_id;
    uint32_t pkt_addr;
    uint32_t rpc_id;
    uint32_t client;
    uint32_t cid;
    uint32_t server;
    uint32_t buffer;
    uint32_t cbuffer;
};

alignas(64) uint8_t packet_buffer[128];         // where the IOP answers
alignas(64) uint8_t send_buffer[128];           // what goes out
uint32_t iop_buffer;                            // the IOP's receive buffer
uint32_t sregs[32];
uint32_t rpc_id_counter;

// A client's bound server.
struct Client {
    uint32_t server;
    uint32_t buffer;
    uint32_t cbuffer;
    bool answered;
    uint32_t answered_cid;
};
Client client;

[[nodiscard]] uint32_t physical(const void *pointer) {
    return reinterpret_cast<uintptr_t>(pointer) & 0x1FFFFFFF;
}

// The uncached view of the packet buffer: the channel writes memory, and the
// kernel maps this alias for exactly that (spec/04 EE-12).
[[nodiscard]] volatile uint32_t *uncachedPacket() {
    return reinterpret_cast<volatile uint32_t *>(physical(packet_buffer) | 0x20000000);
}

void sendCommand(uint32_t cid, void *packet, uint32_t psize, const void *extra,
                 uint32_t extra_dest, uint32_t extra_size) {
    auto *header = static_cast<CommandHeader *>(packet);
    header->size = psize | (extra_size << 8);
    header->dest = extra_size != 0 ? extra_dest : 0;
    header->cid = cid;
    Transfer list[2];
    uint32_t count = 0;
    if (extra_size != 0) {
        list[count++] = {physical(extra), extra_dest, extra_size, 0};
    }
    list[count++] = {physical(packet), iop_buffer, psize, kAttrEnd | kAttrInterruptIop};
    asm volatile("" ::: "memory");
    const uint32_t id = syscall(kSysSifSetDma, reinterpret_cast<uintptr_t>(list), count);
    while (id != 0 && syscall<int32_t>(kSysSifDmaStat, id) >= 0) {
    }
}

// Wait for one packet from the IOP and dispatch it; the polling stands in
// for the SDK's channel-5 handler.
void takePacket() {
    volatile uint32_t *view = uncachedPacket();
    while ((view[0] & 0xFF) == 0) {
    }
    uint32_t copy[32];
    const uint32_t words = ((view[0] & 0xFF) + 3) / 4;
    for (uint32_t k = 0; k < words && k < 32; k++) {
        copy[k] = view[k];
    }
    view[0] = 0;
    (void)syscall(kSysSifSetDChain);
    const auto *header = reinterpret_cast<const CommandHeader *>(copy);
    if (header->cid == kCidSetSreg) {
        if (copy[4] < 32) {
            sregs[copy[4]] = copy[5];
        }
    } else if (header->cid == kCidRpcEnd) {
        const auto *end = reinterpret_cast<const RpcEnd *>(copy);
        client.answered_cid = end->cid;
        client.server = end->server;
        client.buffer = end->buffer;
        client.cbuffer = end->cbuffer;
        client.answered = true;
    }
}

}  // namespace

// BOOT-12c: the two INIT_CMDs, and the IOP's SET_SREG that ends the second.
void initRpc() {
    while ((syscall(kSysSifGetReg, kRegSmflg) & kStatCmdInit) == 0) {
    }
    iop_buffer = syscall(kSysSifGetReg, kRegSmcom);
    (void)syscall(kSysSifSetDChain);
    auto *words = reinterpret_cast<uint32_t *>(send_buffer);
    words[3] = 0;                               // opt 0: here is our buffer
    words[4] = physical(packet_buffer);
    sendCommand(kCidInitCmd, send_buffer, 0x14, nullptr, 0, 0);
    words[3] = 1;                               // opt 1: may RPC start?
    sendCommand(kCidInitCmd, send_buffer, 0x10, nullptr, 0, 0);
    while (sregs[0] == 0) {
        takePacket();
    }
}

[[nodiscard]] bool bindRpc(uint32_t sid) {
    auto *bind = reinterpret_cast<RpcBind *>(send_buffer);
    bind->rec_id = 0;
    bind->pkt_addr = physical(send_buffer);
    bind->rpc_id = ++rpc_id_counter;
    bind->client = reinterpret_cast<uintptr_t>(&client);
    bind->sid = sid;
    client.answered = false;
    client.server = 0;
    sendCommand(kCidRpcBind, send_buffer, 0x40, nullptr, 0, 0);
    while (!client.answered) {
        takePacket();
    }
    return client.answered_cid == kCidRpcBind && client.server != 0;
}

void callRpc(uint32_t fno, const void *send, uint32_t send_size, void *receive,
             uint32_t receive_size) {
    auto *call = reinterpret_cast<RpcCall *>(send_buffer);
    call->rec_id = 0;
    call->pkt_addr = physical(send_buffer);
    call->rpc_id = ++rpc_id_counter;
    call->client = reinterpret_cast<uintptr_t>(&client);
    call->fno = fno;
    call->send_size = send_size;
    call->receive = physical(receive);
    call->receive_size = receive_size;
    call->mode = 1;
    call->server = client.server;
    client.answered = false;
    sendCommand(kCidRpcCall, send_buffer, 0x40, send, client.buffer, send_size);
    while (!client.answered) {
        takePacket();
    }
}


}  // namespace ps2::sifclient
