// Reading the memory card. See `card.hpp`.

#include "card.hpp"

#include "sifclient.hpp"

namespace ps2::card {

namespace {

using namespace ps2::sifclient;

// IOP-15h: the service and the two entry points a shell needs.
constexpr uint32_t kCardServer = 0x80000400;
constexpr uint32_t kFnoGetDir = 0x76;
constexpr uint32_t kFnoSlotInfo = 0x78;

// IOP-15c's codes, as far as they reach a caller.
constexpr int32_t kSameCard = 0;
constexpr int32_t kCardChanged = -1;
constexpr int32_t kNotFormatted = -2;

// IOP-15g's record, and the fields this program reads out of it.
constexpr uint32_t kRecordBytes = 0x40;
constexpr uint32_t kOffLength = 0x10;
constexpr uint32_t kOffMode = 0x14;
constexpr uint32_t kOffName = 0x20;
constexpr uint16_t kModeDirectory = 1u << 5;

constexpr uintptr_t kUncached = 0x20000000;

// Asking for a service a thread on the other processor is still registering
// needs a pause between attempts, not just a count. Without one this program
// answers every refusal with another request, the other processor spends all
// its time answering them, and the low-priority thread that would register
// the service never runs -- the bind fails for as long as it is retried.
constexpr uint32_t kBindAttempts = 64;
constexpr uint32_t kBindPause = 200000;

// Transfer destinations, so 64-byte aligned and never read through the cache:
// the other processor writes them and this program never does.
alignas(64) volatile uint8_t slot_record[kRecordBytes];
alignas(64) volatile uint8_t listing[kEntries * kRecordBytes];

alignas(16) uint8_t request[0x40];
alignas(16) uint32_t reply[4];

bool bound;

[[nodiscard]] uint32_t physical(const volatile void *address) {
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(address) & 0x1FFFFFFF);
}

[[nodiscard]] const volatile uint8_t *uncached(const volatile void *address) {
    return reinterpret_cast<const volatile uint8_t *>(kUncached | physical(address));
}

void setWord(uint32_t index, uint32_t value) {
    *reinterpret_cast<uint32_t *>(request + index * 4) = value;
}

void clearRequest() {
    for (uint32_t i = 0; i < sizeof request; i++) {
        request[i] = 0;
    }
}

[[nodiscard]] int32_t call(uint32_t fno, uint32_t send_bytes) {
    reply[0] = 0;
    callRpc(fno, request, send_bytes, reply, sizeof(uint32_t));
    return static_cast<int32_t>(reply[0]);
}

}  // namespace

bool begin() {
    // The serial interface, then the driver, then the service: each imports
    // the one before it, and a module whose imports are unbound goes quiet
    // rather than failing (IRX-9).
    if (!loadIopModule("rom0:SIO2MAN") || !loadIopModule("rom0:MCMAN")
        || !loadIopModule("rom0:MCSERV")) {
        return false;
    }
    // The load returns when the service's entry does, and its entry only
    // starts the thread that registers the service -- so the first bind races
    // that thread and loses. EELOAD's own bind of the module loader retries
    // for the same reason.
    for (uint32_t attempt = 0; attempt < kBindAttempts && !bound; attempt++) {
        bound = bindRpc(kCardServer);
        for (volatile uint32_t spin = 0; !bound && spin < kBindPause; spin++) {
        }
    }
    return bound;
}

State read() {
    State state;
    state.asked = false;
    state.present = false;
    state.formatted = false;
    state.code = 0;
    state.entries = 0;
    for (uint32_t i = 0; i < kEntries; i++) {
        state.entry[i].name[0] = '\0';
        state.entry[i].length = 0;
        state.entry[i].mode = 0;
        state.entry[i].directory = false;
    }
    if (!bound && !bindRpc(kCardServer)) {
        return state;
    }
    bound = true;

    // IOP-15h's first entry point: port, slot, ask for the type, and where to
    // put the record. The record itself carries nothing this program needs
    // beyond the answer, but the address must be given or nothing is sent.
    clearRequest();
    setWord(1, 0);
    setWord(2, 0);
    setWord(3, 1);
    setWord(7, physical(slot_record));
    state.code = call(kFnoSlotInfo, 0x20);
    state.asked = true;
    state.present = state.code == kSameCard || state.code == kCardChanged
                 || state.code == kNotFormatted;
    state.formatted = state.code == kSameCard || state.code == kCardChanged;
    if (!state.formatted) {
        return state;
    }

    clearRequest();
    setWord(0, 0);
    setWord(1, 0);
    setWord(2, 0);                      // zero starts a listing
    setWord(3, kEntries);
    setWord(4, physical(listing));
    request[0x14] = '/';
    request[0x15] = '*';
    request[0x16] = '\0';
    const int32_t got = call(kFnoGetDir, 0x20);
    if (got <= 0) {
        return state;
    }
    state.entries = static_cast<uint32_t>(got) < kEntries
        ? static_cast<uint32_t>(got) : kEntries;
    for (uint32_t i = 0; i < state.entries; i++) {
        const volatile uint8_t *at = uncached(listing + i * kRecordBytes);
        Entry &entry = state.entry[i];
        entry.length = static_cast<uint32_t>(at[kOffLength])
            | (static_cast<uint32_t>(at[kOffLength + 1]) << 8)
            | (static_cast<uint32_t>(at[kOffLength + 2]) << 16)
            | (static_cast<uint32_t>(at[kOffLength + 3]) << 24);
        entry.mode = static_cast<uint16_t>(
            static_cast<uint32_t>(at[kOffMode])
            | (static_cast<uint32_t>(at[kOffMode + 1]) << 8));
        entry.directory = (entry.mode & kModeDirectory) != 0;
        uint32_t k = 0;
        for (; k < kNameBytes; k++) {
            const char c = static_cast<char>(at[kOffName + k]);
            if (c == '\0') {
                break;
            }
            entry.name[k] = c;
        }
        entry.name[k] = '\0';
    }
    return state;
}

}  // namespace ps2::card
