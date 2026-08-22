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
// this module published. The reference serves them from `SIFCMD` under a DMA
// interrupt; here the service polls the packet's own size byte, which the
// channel writes as the packet lands and the service zeroes when it has read
// it -- the same mark the EE's client uses on its side. Two kinds of command
// are answered: the system commands the SDK's client sends to bring its RPC
// layer up (BOOT-12c), and a command of our own by which the kernel asks for a
// file out of the archive (docs/implementation.md) -- the reference serves
// `rom0:` through ROMDRV over SIFCMD's RPC, which does not exist here yet.

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
// EE's channel pops to learn where the bytes go.
constexpr uint32_t kTagEnd = 0x80000000;        // last packet of this channel's run
constexpr uint32_t kTagDestEnd = 0x90000000;    // an EE destination tag: `cnt`
                                                // with the interrupt bit, which
                                                // is what the reference writes

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
constexpr uint32_t kCidFile = 0x10;

// BOOT-12a: the sixteen bytes every packet begins with. `size` is `psize` in
// its low byte and `dsize` above it.
struct CommandHeader {
    uint32_t size;
    uint32_t dest;
    uint32_t cid;
    uint32_t opt;
};
static_assert(sizeof(CommandHeader) == 16);

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
    uint32_t address;       // | kTagEnd
    uint32_t words;
    uint32_t tag;           // kTagDestEnd | quadwords
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

alignas(16) uint8_t receive[kPacketBytes];  // published in SMCOM: packets land here
alignas(16) SendBlock send_block;
alignas(16) uint32_t payload[kPayloadWords];
uint32_t ee_area;                          // what the EE published at the handshake
uint32_t ee_packet_buffer;                 // BOOT-12c: where the EE's client listens

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

// Send `words` of the payload to `destination` in EE memory: BOOT-11c's send
// block, then the channel. BOOT-11k: the EE's client names its buffer through
// an uncached alias, and what goes into the tag has to be the physical address.
void send(uint32_t words, uint32_t destination) {
    writeWord(kSifCtrl, kCtrlSif0Path);          // BOOT-11f, for this path

    // The bus moves quadwords and the EE's channel counts them, so a short
    // answer is padded up to one; the payload has room for the padding.
    const uint32_t quads = (words + 3) / 4;
    send_block.address = reinterpret_cast<uintptr_t>(payload) | kTagEnd;
    send_block.words = quads * 4;
    send_block.tag = kTagDestEnd | quads;
    send_block.destination = destination & 0x1FFFFFFF;
    barrier();

    writeWord(kDmaSif0 + kTadr, reinterpret_cast<uintptr_t>(&send_block));
    writeWord(kDmaSif0 + kBcr, kDmaBlock);
    writeWord(kDmaSif0 + kChcr, kDmaSendChcr);   // start
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
    send(6, ee_packet_buffer);
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
    send(words, request.destination);
}

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

// The size byte is in the packet's first word and lands first; the rest of
// the packet follows it. The channel's busy bit says when it has all landed
// -- where that bit works (spec/03 BOOT-11h: on one target it never clears),
// so the wait for it is bounded rather than trusted.
void waitForPacket() {
    while ((readWord(reinterpret_cast<uintptr_t>(receive)) & 0xFF) == 0) {
    }
    for (uint32_t n = 0; n < 4096; n++) {
        if ((readWord(kDmaSif1 + kChcr) & kDmaBusy) == 0) {
            break;
        }
    }
    barrier();
}

[[noreturn]] void serve() {
    armReceive();
    writeWord(kSifSmflg, kCommandBit);           // BOOT-12b: listening
    for (;;) {
        waitForPacket();
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
        case kCidFile:
            serveFile(*reinterpret_cast<const Request *>(body));
            break;
        default:
            break;                               // nothing registered for it
        }
        armReceive();
    }
}

}  // namespace

extern "C" {

// The module's entry, called by the loader as entry(argc, argv, 0, record)
// (spec/02 IRX-10). It never returns: the IOP's boot ends here, serving.
[[noreturn]] int _module_start(int, char **) {
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

    serve();
}

}  // extern "C"
