// Reading the machine's configuration record. See `config.hpp`.

#include "config.hpp"

#include "sifclient.hpp"

namespace ps2::config {

namespace {

using namespace ps2::sifclient;

// spec/06 IOP-13a and IOP-13d: the numbered service, and the four entry
// points of the session.
constexpr uint32_t kCdvdMainServer = 0x80000593;
constexpr uint32_t kFnoOpenConfig = 14;
constexpr uint32_t kFnoCloseConfig = 15;
constexpr uint32_t kFnoReadConfig = 16;

// IOP-13e's reply: the return at +0x0, the status at +0x4, the blocks at
// +0x8. A DMA destination, so quadword-aligned.
struct Reply {
    uint32_t returned;
    uint32_t status;
    uint8_t blocks[kBlocks * kBlockBytes];
    uint8_t pad[2];
};
static_assert(sizeof(Reply) == 40);

alignas(16) Reply reply;
alignas(16) uint32_t request[4];

// docs/analysis/27: the caller retries the whole call while either of these
// stands in the status it got back. Leaving one set is what makes the
// reference's own OSD spin here rather than fail, so a bound is ours to add:
// a machine whose mechacon never settles must not take the boot with it.
constexpr uint32_t kStatusRetry = 0x81;
constexpr uint32_t kAttempts = 8;

// IOP-13e: byte 0 is the *second* argument, byte 1 the first, byte 2 the
// count. The OSD's own call is open(1, 0, 2).
constexpr uint32_t kOpenFirst = 1;
constexpr uint32_t kOpenSecond = 0;

[[nodiscard]] bool callRetrying(uint32_t fno, uint32_t send_size) {
    for (uint32_t attempt = 0; attempt < kAttempts; attempt++) {
        reply.returned = 0;
        reply.status = 0;
        callRpc(fno, request, send_size, &reply, sizeof reply);
        if ((reply.status & kStatusRetry) == 0) {
            return reply.returned != 0;
        }
    }
    return false;
}

// IOP-13g's map. The two fields that straddle a byte boundary are the reason
// this is written out rather than cast over a struct.
void decode(const uint8_t *block, Record &record) {
    const uint8_t at0 = block[0];
    const uint8_t at1 = block[1];
    const uint8_t at2 = block[2];
    const uint8_t at3 = block[3];
    const uint8_t at4 = block[4];
    const uint8_t at5 = block[5];

    record.configured = (at2 & 0x80) != 0;        // IOP-13g2
    record.kernel_bit = (at0 & 0x08) != 0;

    // IOP-13g1: the gate decides which field is even read. Getting this wrong
    // is silent in both directions, so the answer records which one it took.
    record.wide_language = (at0 >> 5) != 0;
    record.language = record.wide_language ? (at1 & 0x1F)
                                           : ((at0 >> 4) & 1);
    record.language_name = record.language < kLanguages
        ? kLanguageNames[record.language] : nullptr;

    // Eleven bits: `+3` low, `+2`'s low three above it. In minutes, with
    // `+2` bit 3 adding an hour on top.
    const uint32_t minutes = at3 | (static_cast<uint32_t>(at2 & 0x07) << 8);
    record.timezone_minutes =
        static_cast<int32_t>(minutes) + ((at2 & 0x08) != 0 ? 60 : 0);
    record.clock_12_hour = (at2 & 0x10) != 0;

    record.unsettled = at5 | (static_cast<uint32_t>(at4 & 1) << 8);
}

// Field by field rather than `Record record = {}`: at this size the compiler
// answers an aggregate initialiser with a call to `memset`, and this program
// links no C library to answer it. The same trap `src/iop/loader.hpp` records.
void clear(Record &record) {
    record.read = false;
    record.configured = false;
    record.wide_language = false;
    record.language = 0;
    record.language_name = nullptr;
    record.timezone_minutes = 0;
    record.clock_12_hour = false;
    record.kernel_bit = false;
    record.unsettled = 0;
    for (uint32_t i = 0; i < kBlocks * kBlockBytes; i++) {
        record.raw[i] = 0;
    }
}

}  // namespace

const char *const kLanguageNames[kLanguages] = {
    "Japanese", "English", "French", "Spanish",
    "German", "Italian", "Dutch", "Portuguese",
};

Record read() {
    Record record;
    clear(record);
    if (!bindRpc(kCdvdMainServer)) {
        return record;                            // no such service on the IOP
    }

    request[0] = kOpenSecond | (kOpenFirst << 8) | (kBlocks << 16);
    if (!callRetrying(kFnoOpenConfig, sizeof(uint32_t))) {
        return record;
    }

    const bool got = callRetrying(kFnoReadConfig, 0)
        && reply.returned == kBlocks;
    if (got) {
        for (uint32_t i = 0; i < kBlocks * kBlockBytes; i++) {
            record.raw[i] = reply.blocks[i];
        }
        record.read = true;
        decode(&record.raw[kBlockBytes], record);  // IOP-13g: only block 1
    }

    // Closed whether or not the read worked: a session left open is one the
    // next reader cannot have.
    (void)callRetrying(kFnoCloseConfig, 0);
    return record;
}

}  // namespace ps2::config
