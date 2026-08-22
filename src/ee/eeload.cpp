// EELOAD: the stub that replaces the running program with another.
//
// docs/spec/04-ee-kernel.md EE-9 and docs/analysis/41. The kernel's program
// loader (slot 0x06) does not load a program itself: it stages this file at
// 0x00082000 and enters it with the name to load, and this asks the IOP's
// LOADFILE over the SIF to put that ELF in memory (fno 1, spec/06 IOP-5f),
// then hands the entry and gp to slot 0x07, which enters the program with
// the launcher's own registers (spec/05 SYS-8d). Called with no arguments --
// the boot's first call, as an emulator's hook expects it (docs/analysis/41
// §3) -- it loads `rom0:OSDSYS` and gives it the browser's argument.
//
// The SIF client below is the shape of the SDK's (docs/analysis/34): the
// command layer over slots 0x77-0x7A, one packet buffer the IOP answers into,
// and the RPC bind and call on top. It differs in one way: it polls the
// packet buffer for an answer rather than installing a channel-5 handler
// (docs/implementation.md), since the simulators that gate the boot deliver
// no EE interrupt, and the kernel's loader is what the boot runs through.

#include <stdint.h>

extern "C" {
alignas(16) uint8_t eeload_stack[0x4000];
alignas(16) uint32_t eeload_args[1 + 16 + 64];    // SYS-8b's block
[[noreturn]] void eeloadHalt();
[[noreturn]] void eeloadMain(int argc, char **argv);
}

namespace {

// --- syscalls (spec/05) -----------------------------------------------------

template <typename R = uint32_t, typename... Args>
[[nodiscard]] R syscall(int number, Args... args) {
    uint32_t words[4] = {0, 0, 0, 0};
    uint32_t k = 0;
    ((words[k++] = static_cast<uint32_t>(args)), ...);
    register uint32_t a0 asm("a0") = words[0];
    register uint32_t a1 asm("a1") = words[1];
    register uint32_t a2 asm("a2") = words[2];
    register uint32_t a3 asm("a3") = words[3];
    register int v1 asm("v1") = number;
    register uint32_t v0 asm("v0");
    asm volatile("syscall"
                 : "=r"(v0), "+r"(a0), "+r"(a1), "+r"(a2), "+r"(a3)
                 : "r"(v1)
                 : "memory", "$8", "$9", "$10", "$11", "$12", "$13", "$14", "$15",
                   "$24", "$25", "$31");
    return static_cast<R>(v0);
}

constexpr int kSysExecPs2 = 0x07;
constexpr int kSysSifDmaStat = 0x76;
constexpr int kSysSifSetDma = 0x77;
constexpr int kSysSifSetDChain = 0x78;
constexpr int kSysSifSetReg = 0x79;
constexpr int kSysSifGetReg = 0x7A;

constexpr uint32_t kRegSmcom = 2;
constexpr uint32_t kRegSmflg = 4;
constexpr uint32_t kStatCmdInit = 0x20000;      // BOOT-12b

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

// --- the loader (spec/06 IOP-5f's fno 1) -----------------------------------------

constexpr uint32_t kLoadfileServer = 0x80000006;
constexpr uint32_t kFunctionElfLoad = 1;
constexpr uint32_t kPathMax = 252;

// The request as the SDK's client lays it out [header]: `{ epc | result,
// gp, path[252], secname[252] }`; the answer overwrites the first words.
struct ElfLoadRequest {
    uint32_t epc;
    uint32_t gp;
    char path[kPathMax];
    char secname[kPathMax];
};
static_assert(sizeof(ElfLoadRequest) == 0x200);

alignas(16) ElfLoadRequest request;
alignas(16) uint32_t answer[4];

// The program's name when no one gives one. Kept as data, eight-byte
// aligned, so that an emulator's fast boot can write another name over it
// (docs/analysis/41 §3): the hook searches for this exact string.
alignas(8) char default_path[64] = "rom0:OSDSYS";
const char kBrowserArgument[] = "BootBrowser";    // EE-9c

void copyString(char *to, const char *from, uint32_t limit) {
    uint32_t k = 0;
    for (; k + 1 < limit && from[k] != '\0'; k++) {
        to[k] = from[k];
    }
    to[k] = '\0';
}

}  // namespace

extern "C" {

[[noreturn]] void eeloadHalt() {
    for (;;) {
    }
}

// argv = { "EELOAD", path, the program's own arguments... } from slot 0x06
// (docs/analysis/41 §2), or nothing at all from the boot.
[[noreturn]] void eeloadMain(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : default_path;
    int program_argc;
    char **program_argv;
    static char *browser[2] = {default_path, nullptr};
    if (argc > 2) {
        program_argc = argc - 2;
        program_argv = argv + 2;
    } else {
        browser[0] = const_cast<char *>(kBrowserArgument);
        program_argc = 1;
        program_argv = browser;
    }

    initRpc();
    bool bound = false;
    for (uint32_t attempt = 0; attempt < 0x1001 && !bound; attempt++) {
        bound = bindRpc(kLoadfileServer);
    }
    if (!bound) {
        eeloadHalt();
    }
    request.epc = 0;
    request.gp = 0;
    copyString(request.path, path, kPathMax);
    copyString(request.secname, "all", kPathMax);
    callRpc(kFunctionElfLoad, &request, sizeof(request), answer, sizeof(answer));
    if (static_cast<int32_t>(answer[0]) < 0) {
        eeloadHalt();
    }
    (void)syscall(kSysExecPs2, answer[0], answer[1],
                  static_cast<uint32_t>(program_argc), reinterpret_cast<uintptr_t>(program_argv));
    eeloadHalt();
}

}  // extern "C"
