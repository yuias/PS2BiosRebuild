// The EE's half of the meeting with the IOP, and the transfers that follow it.
//
// docs/spec/03-boot-chain.md BOOT-10. The EE opens the bus, publishes an
// address in its own RAM and raises a bit; the IOP answers with an address in
// its RAM and a bit of its own. Neither side may proceed alone.
//
// BOOT-10c: from this side a write to MSFLG *sets* bits and a write to SMFLG
// *clears* them; the IOP's writes do the reverse. Getting that backwards does
// not fail, it livelocks -- each side's acknowledgement erasing the other's
// request -- which is why it is written out rather than assumed.

#include <stdint.h>

namespace {

constexpr uintptr_t kSifMscom = 0xB000F200;
constexpr uintptr_t kSifSmcom = 0xB000F210;
constexpr uintptr_t kSifMsflg = 0xB000F220;
constexpr uintptr_t kSifSmflg = 0xB000F230;
constexpr uintptr_t kSifCtrl = 0xB000F240;

constexpr uint32_t kHandshakeBit = 0x00010000;
constexpr uint32_t kRequestBit = 0x00000002;    // we have sent the IOP something
constexpr uint32_t kReplyBit = 0x00000002;      // and it has answered
constexpr uint32_t kCtrlOpen = 0x00000100;

// The EE's two SIF DMA channels, and the registers each carries.
constexpr uintptr_t kDmaSif0 = 0xB000C000;      // from the IOP
constexpr uintptr_t kDmaSif1 = 0xB000C400;      // to the IOP
constexpr uintptr_t kChcr = 0x00;
constexpr uintptr_t kTadr = 0x30;

// BOOT-11b: both channels run in chain mode -- MOD=chain, TIE and STR, with TTE
// clear so the tags stay out of the data.
constexpr uint32_t kDmaChain = 0x00000184;
constexpr uint32_t kDmaStart = 0x00000100;      // CHCR.STR, which hardware clears

// BOOT-11g: a channel's own STR is not enough. The controller has a master
// enable of its own and a hold register that starts out holding, and until both
// are dealt with a started channel simply does not run. A simulator that looks
// only at STR will not notice; an emulator will move nothing.
constexpr uintptr_t kDmaCtrl = 0xB000E000;      // bit 0: the controller is on
constexpr uintptr_t kDmaStat = 0xB000E010;
constexpr uintptr_t kDmaEnableW = 0xB000F590;   // bit 16 holds every channel off
constexpr uint32_t kDmaEnabled = 0x00002000;    // what the reference leaves there
constexpr uint32_t kDmaStatClear = 0x0000E31F;

constexpr uint32_t kTagRefe = 0x00000000;       // a source tag pointing elsewhere
constexpr uint32_t kTagEnd = 0x80000000;        // the IOP-facing header's end bit

constexpr uint32_t kNameLength = 10;            // spec/01 ARC-2
constexpr uint32_t kPacketQuads = 3;            // the header quadword and two more

[[nodiscard]] uint32_t readWord(uintptr_t address) {
    return *reinterpret_cast<volatile uint32_t *>(address);
}

void writeWord(uintptr_t address, uint32_t value) {
    *reinterpret_cast<volatile uint32_t *>(address) = value;
}

void waitUntilSet(uintptr_t address, uint32_t bits) {
    while ((readWord(address) & bits) == 0) {
    }
}

// BOOT-11h: a transfer is not instantaneous, and raising a flag before the
// channel has drained tells the other side to read a buffer the bytes have not
// reached yet.
void waitForChannel(uintptr_t channel) {
    while ((readWord(channel + kChcr) & kDmaStart) != 0) {
    }
}

// The request the IOP reads: an entry name in its fixed-width field, what to do
// with it, and where the answer is to land back here.
struct Request {
    char name[kNameLength];
    uint16_t pad;
    uint32_t verb;
    uint32_t destination;
    uint32_t reserved[3];
};
static_assert(sizeof(Request) == 32);

// The header the IOP's channel reads: the address it published to us, and how
// many words follow.
struct Packet {
    uint32_t header;
    uint32_t words;
    uint32_t reserved[2];
};

// One source tag covers three quadwords -- the header and the two the request
// occupies -- so the two have to be **contiguous**. Separate objects would let
// the compiler order or pad them apart and the transfer would carry whatever
// happened to follow the header instead.
struct Message {
    Packet header;
    Request request;
};
static_assert(sizeof(Message) == kPacketQuads * 16);

// TTE is clear, so the tag itself does not travel and the header leads the data.
struct SourceTag {
    uint32_t control;
    uint32_t address;
    uint32_t reserved[2];
};

alignas(16) uint8_t sif_area[256];               // what we publish to the IOP
alignas(16) SourceTag sif_tag;
alignas(16) Message sif_message;

}  // namespace

extern "C" {

uint32_t sif_peer;                               // what the IOP publishes to us

// Returns once the IOP has answered, with its published address.
uint32_t sifHandshake() {
    // BOOT-11g: bring the DMA controller up before anything asks it to move.
    writeWord(kDmaStat, kDmaStatClear);          // acknowledge what reset left
    writeWord(kDmaEnableW, kDmaEnabled);         // release the hold on every one
    writeWord(kDmaCtrl, 1);

    writeWord(kSifCtrl, kCtrlOpen);
    writeWord(kSifMscom, reinterpret_cast<uintptr_t>(sif_area));  // BOOT-10a
    writeWord(kSifMsflg, kHandshakeBit);         // our write sets

    waitUntilSet(kSifSmflg, kHandshakeBit);      // BOOT-10b: wait for the answer

    sif_peer = readWord(kSifSmcom);
    writeWord(kSifSmflg, kHandshakeBit);         // our write to SMFLG clears: ack
    return sif_peer;
}

// One request out and one answer back. The EE cannot read the IOP's memory and
// the IOP cannot read the EE's, so everything crosses as a transfer.
//
// BOOT-11: neither channel is told where its bytes belong by its own driver.
// What goes out is a packet headed by a quadword naming an address in *IOP*
// memory, and what comes back is headed by a tag naming an address in ours --
// which is why the destination travels in the request rather than into MADR.
void *sifExchange(const char *name, uint32_t verb, void *destination) {
    // BOOT-11i: arm the receiving channel first. A transfer runs only when both
    // ends are ready, so a side that arms its receiver after asking has arranged
    // for neither end to move.
    writeWord(kDmaSif0 + kChcr, kDmaChain);

    // A caller's string ends at its NUL and whatever follows it in memory is not
    // part of the name, so the copy stops there and the rest is filled in.
    uint32_t index = 0;
    for (; index < kNameLength && name[index] != '\0'; index++) {
        sif_message.request.name[index] = name[index];
    }
    for (; index < kNameLength; index++) {
        sif_message.request.name[index] = '\0';
    }
    sif_message.request.pad = 0;
    sif_message.request.verb = verb;
    sif_message.request.destination = reinterpret_cast<uintptr_t>(destination);
    sif_message.request.reserved[0] = 0;
    sif_message.request.reserved[1] = 0;
    sif_message.request.reserved[2] = 0;

    // The end bit stops the IOP's channel after this packet.
    sif_message.header.header = sif_peer | kTagEnd;
    sif_message.header.words = sizeof(Request) / sizeof(uint32_t);
    sif_message.header.reserved[0] = 0;
    sif_message.header.reserved[1] = 0;

    sif_tag.control = kTagRefe | kPacketQuads;
    sif_tag.address = reinterpret_cast<uintptr_t>(&sif_message);
    sif_tag.reserved[0] = 0;
    sif_tag.reserved[1] = 0;

    writeWord(kDmaSif1 + kTadr, reinterpret_cast<uintptr_t>(&sif_tag));
    writeWord(kDmaSif1 + kChcr, kDmaChain);      // start
    waitForChannel(kDmaSif1);

    writeWord(kSifMsflg, kRequestBit);           // our write sets: it is there
    waitUntilSet(kSifSmflg, kReplyBit);
    writeWord(kSifSmflg, kReplyBit);             // our write to SMFLG clears

    // The channel was armed before the request went out; all that is left is to
    // let it finish. The tag the IOP put in front of the data is what says where
    // it lands and how much of it there is.
    waitForChannel(kDmaSif0);
    return destination;
}

}  // extern "C"
