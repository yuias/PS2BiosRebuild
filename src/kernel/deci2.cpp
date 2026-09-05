// SYS-3c: the sub-functions of Deci2Call that answer. docs/analysis/52.

#include <stdint.h>

extern "C" uint32_t _syscall_context[];

namespace {

// The reference's socket table: sixteen entries, 1 to 16, of a protocol
// halfword, an option word and a handler. The kernel's own manager holds 1 to
// 4 at boot, and a search for a free entry starts at 2 (docs/analysis/52).
struct Deci2Socket {
    uint16_t protocol;
    uint32_t option;
    uint32_t handler;
};

constexpr int kSockets = 16;
constexpr uint16_t kKernelProtocols[] = {0x0001, 0x0201, 0x021F, 0x0230};

Deci2Socket sockets[kSockets + 1];
bool sockets_ready;

void prepareSockets() {
    if (sockets_ready) {
        return;
    }
    sockets_ready = true;
    int index = 1;
    for (uint16_t protocol : kKernelProtocols) {
        sockets[index++] = {protocol, 0, 0};
    }
}

// 1..16 with a protocol registered.
bool validSocket(uint32_t socket) {
    return socket - 1 < static_cast<uint32_t>(kSockets)
        && sockets[socket].protocol != 0;
}

// The index holding `protocol`, 0 when none. An empty entry never matches,
// so asking for protocol 0 finds nothing.
int findSocket(uint16_t protocol) {
    for (int index = 1; index <= kSockets; index++) {
        if (sockets[index].protocol != 0 && sockets[index].protocol == protocol) {
            return index;
        }
    }
    return 0;
}

// fno 1: open(protocol, option, handler) -> socket | -3 taken | -4 full.
int32_t open(uint16_t protocol, uint32_t option, uint32_t handler) {
    if (findSocket(protocol) > 0) {
        return -3;
    }
    for (int index = 2; index <= kSockets; index++) {
        if (sockets[index].protocol == 0) {
            sockets[index] = {protocol, option, handler};
            return index;
        }
    }
    return -4;
}

// fno 2: close(socket) -> 1 | -2.
int32_t close(uint32_t socket) {
    if (!validSocket(socket)) {
        return -2;
    }
    sockets[socket].protocol = 0;
    return 1;
}

// fno 3: reqsend(socket, destination) -> 1 | -2 bad socket | -10 no link.
// The reference refuses a request for the host ('H') or the IOP ('I') while
// neither link is up, which on this image is always; any other destination
// is accepted and queued for its poll to serve, and that queue is not built
// (docs/implementation.md).
int32_t requestSend(uint32_t socket, int8_t destination) {
    if (!validSocket(socket)) {
        return -2;
    }
    if (destination == 'H' || destination == 'I') {
        return -10;
    }
    return 1;
}

// The frame slot a sub-function answers through: $v0's, as a doubleword,
// because the dispatcher restores every register from the frame and a
// sub-function that writes nothing is invisible.
void answer(int32_t value) {
    *reinterpret_cast<int64_t *>(&_syscall_context[2 * 4]) = value;
}

}  // namespace

// Slot 0x7C's body, entered from syscall.S with the caller's arguments intact.
// `param` is the SDK's block for the sub-function: open takes {protocol
// halfword, option, handler}, close and poll {socket}, reqsend {socket,
// destination byte}.
extern "C" void sysDeci2Call(uint32_t fno, const uint32_t *param)
    asm("_sys_deci2_call");
extern "C" void sysDeci2Call(uint32_t fno, const uint32_t *param) {
    prepareSockets();
    switch (fno) {
    case 1:
        answer(open(static_cast<uint16_t>(param[0]), param[1], param[2]));
        break;
    case 2:
        answer(close(param[0]));
        break;
    case 3:
        answer(requestSend(param[0], static_cast<int8_t>(param[1])));
        break;
    case 4: case 5: case 6: case 7: case 8: case 9:
        // Served by the manager's link driver on the reference; with no link
        // they touch nothing, and their callers see their own $v0 back.
        break;
    default:
        // 0xA-0xF are unassigned in the reference's table and share the
        // out-of-range answer; 0x10 computes one and then falls into the
        // same code, so it answers -1 too.
        answer(-1);
        break;
    }
}
