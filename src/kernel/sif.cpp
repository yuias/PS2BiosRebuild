// The EE's half of the SIF: the meeting with the IOP, the command packets that
// cross afterwards, and the five slots a program drives the bus with.
//
// docs/spec/03-boot-chain.md BOOT-10 to BOOT-12 and docs/spec/05 SYS-13. The
// EE opens the bus, publishes an address in its own RAM and raises a bit; the
// IOP answers with the address of its command receive buffer and a bit of its
// own. From then on everything the EE sends is a BOOT-12a command packet --
// the SDK's client sends its own through slot 0x77, and the kernel's file
// fetches go the same way under a command id of ours -- and everything that
// comes back lands where the sender's tag says (BOOT-11c).
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

constexpr uint32_t kHandshakeBit = 0x00010000;  // BOOT-10: SIF_STAT_SIFINIT
constexpr uint32_t kCommandBit = 0x00020000;    // BOOT-12b: SIF_STAT_CMDINIT
constexpr uint32_t kCtrlOpen = 0x00000100;

// The EE's two SIF DMA channels, and the registers each carries.
constexpr uintptr_t kDmaSif0 = 0xB000C000;      // channel 5, from the IOP
constexpr uintptr_t kDmaSif1 = 0xB000C400;      // channel 6, to the IOP
constexpr uintptr_t kChcr = 0x00;
constexpr uintptr_t kQwc = 0x20;
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
constexpr uintptr_t kDmaStat = 0xB000E010;      // low half status, high half mask
constexpr uintptr_t kDmaEnableW = 0xB000F590;   // bit 16 holds every channel off
constexpr uint32_t kDmaEnabled = 0x00002000;    // what the reference leaves there
constexpr uint32_t kDmaStatClear = 0x0000E31F;
constexpr uint32_t kDmaStatSif0 = 1u << 5;      // channel 5's pending bit

// Source-chain tag ids (BOOT-11b): `ref` names data elsewhere and goes on,
// `refe` names data elsewhere and is the last of the list.
constexpr uint32_t kTagRef = 0x30000000;
constexpr uint32_t kTagRefe = 0x00000000;

// BOOT-11a: the header quadword's flag bits, above the IOP address.
constexpr uint32_t kHeaderEnd = 0x80000000;
constexpr uint32_t kHeaderInterrupt = 0x40000000;

// SYS-13c: the attribute bits of a transfer-list entry that reach the header.
// Values are the SDK's (`SIF_DMA_INT_O`, `SIF_DMA_ERT`), since the SDK's
// client is what fills the list.
constexpr uint32_t kAttrInterruptIop = 0x04;
constexpr uint32_t kAttrEnd = 0x40;

constexpr uint32_t kMaxTransfers = 8;           // the SDK sends at most two
constexpr uint32_t kSoftwareRegisters = 32;     // SYS-13a
constexpr uint32_t kSoftwareRegisterBit = 0x80000000;

constexpr uint32_t kNameLength = 10;            // spec/01 ARC-2

// The command id the kernel's own file requests travel under. Non-negative,
// so it is a "user" command in BOOT-12a's terms, and the only one the IOP's
// service knows; `src/iop/eesync.cpp` names the same number.
constexpr uint32_t kFileCommand = 0x10;

[[nodiscard]] uint32_t readWord(uintptr_t address) {
    return *reinterpret_cast<volatile uint32_t *>(address);
}

void writeWord(uintptr_t address, uint32_t value) {
    *reinterpret_cast<volatile uint32_t *>(address) = value;
}

// Every address handed to the DMAC has to be a physical one, and not merely
// because the controller does not walk the TLB: **bit 31 of MADR and TADR
// selects the scratchpad**. The kernel is linked in KSEG0, so a pointer taken
// here arrives with that bit set and the controller reads sixteen kilobytes of
// scratchpad instead of the buffer that was meant. A simulator that masks
// addresses on its way into memory cannot see the difference; the hardware can.
[[nodiscard]] uint32_t physical(const void *pointer) {
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pointer))
           & 0x1FFFFFFF;
}

[[nodiscard]] uint32_t physical(uint32_t address) {
    return address & 0x1FFFFFFF;
}

// What the controller reads has to be in memory, not in the data cache. The
// kernel's DMA-visible objects are written through KSEG1 so that they are.
template <typename T>
[[nodiscard]] T *uncached(T *pointer) {
    return reinterpret_cast<T *>(reinterpret_cast<uintptr_t>(pointer)
                                 | 0x20000000);
}

// Ordinary stores are not ordered against volatile ones by the language, so
// the requirement is stated rather than left to what the optimiser happens to
// do.
void barrier() {
    asm volatile("" ::: "memory");
}

void waitUntilSet(uintptr_t address, uint32_t bits) {
    while ((readWord(address) & bits) == 0) {
    }
}

// BOOT-11h: a transfer is not instantaneous, and treating one as done before
// the channel has drained reads a buffer the bytes have not reached yet.
void waitForChannel(uintptr_t channel) {
    while ((readWord(channel + kChcr) & kDmaStart) != 0) {
    }
}

[[nodiscard]] uint32_t quadwords(uint32_t bytes) {
    return (bytes + 15) / 16;
}

// BOOT-12a: the sixteen bytes every command packet begins with. `size` is
// `psize` in its low byte and `dsize` above it.
struct CommandHeader {
    uint32_t size;
    uint32_t dest;
    uint32_t cid;
    uint32_t opt;
};
static_assert(sizeof(CommandHeader) == 16);

// The body of the kernel's file request, as the IOP reads it: an entry name in
// its fixed-width field, what to do with it, where the answer is to land back
// here, and -- for a content request -- which window of the file. The IOP
// answers at most one window (16 KiB) per exchange, so a file larger than that
// is fetched by more than one.
struct Request {
    char name[kNameLength];
    uint16_t pad;
    uint32_t verb;
    uint32_t destination;
    uint32_t offset;
    uint32_t length;        // 0: as much as fits
    uint32_t reserved;
};
static_assert(sizeof(Request) == 32);

struct FilePacket {
    CommandHeader header;
    Request request;
};
static_assert(sizeof(FilePacket) == 48);

// BOOT-11a: the quadword the IOP's channel reads ahead of each packet -- the
// address it published to us with the flags above it, and how many words
// follow.
struct Packet {
    uint32_t header;
    uint32_t words;
    uint32_t reserved[2];
};

// TTE is clear, so the tag itself does not travel and only what it points at
// does.
struct SourceTag {
    uint32_t control;
    uint32_t address;
    uint32_t reserved[2];
};

// SYS-13c: one entry of the list slot 0x77 takes.
struct Transfer {
    uint32_t src;
    uint32_t dest;
    uint32_t size;
    uint32_t attr;
};

alignas(16) uint8_t sif_area[256];               // what we publish to the IOP
alignas(16) Packet sif_headers[kMaxTransfers];
alignas(16) SourceTag sif_tags[2 * kMaxTransfers];
alignas(16) FilePacket sif_file_packet;
uint32_t software_registers[kSoftwareRegisters];

// BOOT-11b over a transfer list: each entry becomes two tags, a `ref` to its
// header quadword here and a `ref` (or, for the last entry, `refe`) to its
// bytes where they are, so nothing is copied. Channel 6 is restarted on the
// list; returns the non-zero word SYS-13c describes, or 0 for a list that
// cannot be sent.
[[nodiscard]] uint32_t startChain(const Transfer *list, uint32_t count) {
    if (count == 0 || count > kMaxTransfers) {
        return 0;
    }
    // A chain still running is one the IOP has not drained yet. Rewriting TADR
    // under it would lose what it had left to push, so it is let finish.
    waitForChannel(kDmaSif1);

    Packet *headers = uncached(sif_headers);
    SourceTag *tags = uncached(sif_tags);
    for (uint32_t k = 0; k < count; k++) {
        const Transfer &entry = list[k];
        if (entry.size == 0) {
            return 0;
        }
        const uint32_t quads = quadwords(entry.size);
        uint32_t flags = 0;
        if (entry.attr & kAttrEnd) {
            flags |= kHeaderEnd;
        }
        if (entry.attr & kAttrInterruptIop) {
            flags |= kHeaderInterrupt;
        }
        headers[k].header = (entry.dest & ~(kHeaderEnd | kHeaderInterrupt)) | flags;
        headers[k].words = quads * 4;
        headers[k].reserved[0] = 0;
        headers[k].reserved[1] = 0;

        tags[2 * k].control = kTagRef | 1;
        tags[2 * k].address = physical(&sif_headers[k]);
        tags[2 * k].reserved[0] = 0;
        tags[2 * k].reserved[1] = 0;
        const bool last = k + 1 == count;
        tags[2 * k + 1].control = (last ? kTagRefe : kTagRef) | quads;
        tags[2 * k + 1].address = physical(entry.src);
        tags[2 * k + 1].reserved[0] = 0;
        tags[2 * k + 1].reserved[1] = 0;
    }
    barrier();

    writeWord(kDmaSif1 + kChcr, 0);
    writeWord(kDmaSif1 + kQwc, 0);
    writeWord(kDmaSif1 + kTadr, physical(sif_tags));
    writeWord(kDmaSif1 + kChcr, kDmaChain);      // start
    // The reference packs list positions into this word; what a caller does
    // with it is hand it back to 0x76, so the count is enough to be non-zero.
    return count;
}

// SYS-13b: channel 5 as a destination chain, ready for whatever the IOP's next
// send block addresses. The read-back is what the slot returns.
[[nodiscard]] uint32_t armReceive(bool start) {
    writeWord(kDmaSif0 + kChcr, 0);
    writeWord(kDmaSif0 + kQwc, 0);
    if (start) {
        writeWord(kDmaSif0 + kChcr, kDmaChain);
    }
    return readWord(kDmaSif0 + kChcr);
}

}  // namespace

extern "C" {

uint32_t sif_peer;                               // the IOP's receive buffer

// Returns once the IOP has answered, with its published address.
uint32_t sifHandshake() {
    // BOOT-11g: bring the DMA controller up before anything asks it to move.
    writeWord(kDmaStat, kDmaStatClear);          // acknowledge what reset left
    writeWord(kDmaEnableW, kDmaEnabled);         // release the hold on every one
    writeWord(kDmaCtrl, 1);

    writeWord(kSifCtrl, kCtrlOpen);
    writeWord(kSifMscom, physical(sif_area));    // BOOT-10a
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
// What goes out is a command packet headed for the IOP's receive buffer, and
// what comes back is headed by a tag naming an address in our memory -- which
// is why the destination travels in the request rather than into MADR.
void *sifExchange(const char *name, uint32_t verb, void *destination,
                  uint32_t offset, uint32_t length) {
    // BOOT-12b: the IOP says when its command service is listening.
    waitUntilSet(kSifSmflg, kCommandBit);

    // BOOT-11i: arm the receiving channel first. A transfer runs only when both
    // ends are ready, so a side that arms its receiver after asking has arranged
    // for neither end to move.
    (void)armReceive(true);

    FilePacket *packet = uncached(&sif_file_packet);
    packet->header.size = sizeof(FilePacket);    // psize; no out-of-band data
    packet->header.dest = 0;
    packet->header.cid = kFileCommand;
    packet->header.opt = 0;

    // A caller's string ends at its NUL and whatever follows it in memory is not
    // part of the name, so the copy stops there and the rest is filled in.
    uint32_t index = 0;
    for (; index < kNameLength && name[index] != '\0'; index++) {
        packet->request.name[index] = name[index];
    }
    for (; index < kNameLength; index++) {
        packet->request.name[index] = '\0';
    }
    packet->request.pad = 0;
    packet->request.verb = verb;
    packet->request.destination = physical(destination);
    packet->request.offset = offset;
    packet->request.length = length;
    packet->request.reserved = 0;

    const Transfer transfer = {physical(&sif_file_packet), sif_peer,
                               sizeof(FilePacket), kAttrEnd};
    (void)startChain(&transfer, 1);
    waitForChannel(kDmaSif1);

    // The channel was armed before the request went out; all that is left is to
    // let it finish. The tag the IOP put in front of the data is what says where
    // it lands and how much of it there is. The completion it leaves pending in
    // the controller is ours, not a program's, so it is taken back here rather
    // than left for the handler a program installs later (SYS-12b).
    waitForChannel(kDmaSif0);
    writeWord(kDmaStat, kDmaStatSif0);
    barrier();
    return destination;
}

// --- SYS-13: the slots ------------------------------------------------------

// Slot 0x78, SYS-13b: (-) -> CHCR read back. Slot 0x6B is the stop.
uint32_t sysSifSetDChain() asm("_sys_sif_set_dchain");
uint32_t sysSifSetDChain() {
    return armReceive(true);
}

uint32_t sysSifStopDChain() asm("_sys_sif_stop_dchain");
uint32_t sysSifStopDChain() {
    return armReceive(false);
}

// Slot 0x77, SYS-13c: (list, count) -> word | 0.
uint32_t sysSifSetDma(const Transfer *list, uint32_t count) asm("_sys_sif_set_dma");
uint32_t sysSifSetDma(const Transfer *list, uint32_t count) {
    return startChain(list, count);
}

// Slot 0x76, SYS-13d: (word) -> -1 once channel 6 has stopped, 0 while it runs.
// The reference reads the channel's position against the word's; reporting
// every transfer as still running until the channel idles answers the SDK's
// wait loop the same way, a little later (docs/implementation.md).
int32_t sysSifDmaStat(uint32_t word) asm("_sys_sif_dma_stat");
int32_t sysSifDmaStat(uint32_t word) {
    (void)word;
    if ((readWord(kDmaSif1 + kChcr) & kDmaStart) == 0) {
        return -1;
    }
    return 0;
}

// Slot 0x79, SYS-13a: (reg, value) -> value | 0. A write from here sets MSFLG
// bits and clears SMFLG bits (BOOT-10c); the registers the SDK keeps its
// addresses in are the software ones, numbered with bit 31 set.
uint32_t sysSifSetReg(uint32_t reg, uint32_t value) asm("_sys_sif_set_reg");
uint32_t sysSifSetReg(uint32_t reg, uint32_t value) {
    if (reg & kSoftwareRegisterBit) {
        const uint32_t index = reg & ~kSoftwareRegisterBit;
        if (index < kSoftwareRegisters) {
            software_registers[index] = value;
        }
        return value;
    }
    switch (reg) {
    case 1:
        writeWord(kSifMscom, value);
        return value;
    case 3:
        writeWord(kSifMsflg, value);
        return value;
    case 4:
        writeWord(kSifSmflg, value);
        return value;
    default:
        return 0;                                // SMCOM is the IOP's to write
    }
}

// Slot 0x7A, SYS-13a: (reg) -> the register.
uint32_t sysSifGetReg(uint32_t reg) asm("_sys_sif_get_reg");
uint32_t sysSifGetReg(uint32_t reg) {
    if (reg & kSoftwareRegisterBit) {
        const uint32_t index = reg & ~kSoftwareRegisterBit;
        return index < kSoftwareRegisters ? software_registers[index] : 0;
    }
    switch (reg) {
    case 1:
        return readWord(kSifMscom);
    case 2:
        return readWord(kSifSmcom);
    case 3:
        return readWord(kSifMsflg);
    case 4:
        return readWord(kSifSmflg);
    default:
        return 0;
    }
}

}  // extern "C"
