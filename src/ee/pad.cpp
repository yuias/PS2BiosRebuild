// Reading the controller. See `pad.hpp`.

#include "pad.hpp"

#include "sifclient.hpp"

namespace ps2::pad {

namespace {

using namespace ps2::sifclient;

// spec/03 BOOT-12e: the module loader's service and its fno 0.
constexpr uint32_t kLoadfileServer = 0x80000006;
constexpr uint32_t kFunctionLoad = 0;
constexpr uint32_t kPathMax = 252;

struct LoadRequest {
    uint32_t argument_length;
    uint32_t pad;
    char path[kPathMax];
    char arguments[kPathMax];
};
static_assert(sizeof(LoadRequest) == 0x200);

// IOP-14a/14b: the service, and the function codes that reach it.
constexpr uint32_t kPadServer = 0x8000010f;
constexpr uint32_t kCodeOpen = 0x80000100;

// IOP-14g: two records, and the fields this reads out of one.
constexpr uint32_t kRecordBytes = 0x40;
constexpr uint32_t kAreaBytes = 2 * kRecordBytes;
constexpr uint32_t kOffFrame = 0x00;
constexpr uint32_t kOffSlotState = 0x04;
constexpr uint32_t kOffValid = 0x06;
constexpr uint32_t kOffId = 0x09;
constexpr uint32_t kOffButtons = 0x0a;

// The driver writes this from the other processor and this program never
// does, so every read goes through the uncached alias: a line the data cache
// filled once would answer with the same bytes for ever, and a controller
// that never changes is exactly what that looks like.
constexpr uintptr_t kUncached = 0x20000000;

// 64-byte aligned: the destination of a DMA run, and the two halves have to
// sit at a fixed stride the driver computes from its own frame counter.
alignas(64) volatile uint8_t area[kAreaBytes];

alignas(16) LoadRequest load_request;
alignas(16) uint32_t load_answer[4];
alignas(16) uint32_t request[8];
alignas(16) uint32_t reply[8];

bool opened;

void copyString(char *to, const char *from, uint32_t limit) {
    uint32_t k = 0;
    for (; k + 1 < limit && from[k] != '\0'; k++) {
        to[k] = from[k];
    }
    to[k] = '\0';
}

[[nodiscard]] const volatile uint8_t *uncached(uint32_t index) {
    const uintptr_t physical = reinterpret_cast<uintptr_t>(&area[index]) & 0x1FFFFFFF;
    return reinterpret_cast<const volatile uint8_t *>(kUncached | physical);
}

[[nodiscard]] uint32_t frameOf(uint32_t half) {
    const volatile uint8_t *at = uncached(half * kRecordBytes + kOffFrame);
    return static_cast<uint32_t>(at[0]) | (static_cast<uint32_t>(at[1]) << 8)
         | (static_cast<uint32_t>(at[2]) << 16) | (static_cast<uint32_t>(at[3]) << 24);
}

// One module, by the path the archive knows it as. The answer's first word is
// the module id or a negative error (BOOT-12e).
[[nodiscard]] bool loadModule(const char *path) {
    load_request.argument_length = 0;
    load_request.pad = 0;
    copyString(load_request.path, path, kPathMax);
    load_request.arguments[0] = '\0';
    load_answer[0] = 0;
    callRpc(kFunctionLoad, &load_request, sizeof load_request,
            load_answer, sizeof load_answer);
    return static_cast<int32_t>(load_answer[0]) >= 0;
}

}  // namespace

const char *const kButtonNames[kButtons] = {
    "select", "L3", "R3", "start", "up", "right", "down", "left",
    "L2", "R2", "L1", "R1", "triangle", "circle", "cross", "square",
};

bool begin() {
    // Through the alias, not the cached address: a cached store here leaves
    // dirty lines over the driver's own DMA target, and the eviction lands on
    // whatever it has pushed since. Neither emulator models a write-back data
    // cache, so getting this wrong passes both and fails a machine.
    for (uint32_t i = 0; i < kAreaBytes; i++) {
        *const_cast<volatile uint8_t *>(uncached(i)) = 0;
    }
    if (!bindRpc(kLoadfileServer)) {
        return false;
    }
    // The serial interface first: the driver imports it, so loading them the
    // other way round leaves the driver's imports unbound and, per IRX-9, it
    // goes quiet rather than failing.
    if (!loadModule("rom0:SIO2MAN") || !loadModule("rom0:PADMAN")) {
        return false;
    }
    if (!bindRpc(kPadServer)) {
        return false;
    }

    // IOP-14c: the driver DMAs to this address and does not check it, so what
    // goes across is the physical one. The uncached alias would be wrong here
    // -- it is a bus address the other processor cannot resolve.
    request[0] = kCodeOpen;
    request[1] = 0;                             // port
    request[2] = 0;                             // slot
    request[3] = 0;
    request[4] = static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(&area[0]) & 0x1FFFFFFF);
    reply[3] = 0;
    callRpc(0, request, 5 * sizeof(uint32_t), reply, sizeof reply);
    opened = reply[3] != 0;
    return opened;
}

State read() {
    State state;
    state.present = false;
    state.buttons = 0;
    state.id = 0;
    state.slot_state = 0;
    state.frame = 0;
    if (!opened) {
        return state;
    }

    // IOP-14g: the driver alternates halves, so the newer one is whichever
    // carries the larger counter. Both start at zero, which is why `present`
    // also needs the validity byte and not just a non-zero counter.
    const uint32_t first = frameOf(0);
    const uint32_t second = frameOf(1);
    const uint32_t half = second > first ? 1 : 0;
    const volatile uint8_t *record = uncached(half * kRecordBytes);

    state.frame = half != 0 ? second : first;
    state.slot_state = record[kOffSlotState];
    if (record[kOffValid] == 0) {
        return state;
    }
    state.id = record[kOffId];
    const uint16_t raw = static_cast<uint16_t>(record[kOffButtons])
        | static_cast<uint16_t>(static_cast<uint16_t>(record[kOffButtons + 1]) << 8);
    state.buttons = static_cast<uint16_t>(~raw);
    state.present = true;
    return state;
}

}  // namespace ps2::pad
