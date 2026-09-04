// SIFMAN: the bus itself -- the two DMA channels, the flag registers, and the
// queue of transfers a module hands the EE.
//
// docs/spec/03-boot-chain.md BOOT-11 and docs/analysis/11 §"SIFMAN". The
// reference splits the EE-facing stack in three: this module moves bytes,
// `SIFCMD` frames them as packets and layers RPC on top, and `EESYNC` is the
// last thing on the boot list, whose whole function is to tell the EE that
// the IOP has finished coming up. They are three modules and not one because
// a title's own `IOPRP` image supersedes them by name (spec/02 IRX-11): a
// disc that carries a newer `SIFCMD` must be able to take that one and leave
// this one alone.
//
// BOOT-10c is the part worth being careful about: a write from this side
// *clears* bits in MSFLG and *sets* them in SMFLG, and the EE's writes do the
// reverse. Registers that merely stored would livelock rather than fail.

#include "module.hpp"
#include "sif.hpp"

#include <stdint.h>

namespace {

using ps2::sif::Transfer;

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
constexpr uintptr_t kBcr = 0x4;
constexpr uintptr_t kChcr = 0x8;
constexpr uintptr_t kTadr = 0xC;

// BOOT-11e: starting a channel takes more than the busy bit. Each carries its
// direction and sync mode, and the block size the SIF moves in has to be in
// BCR -- values read off the reference's own driver, because an emulator
// enforces them where a lenient simulator does not.
constexpr uint32_t kDmaSendChcr = 0x01000701;   // ch9:  from memory, started
constexpr uint32_t kDmaRecvChcr = 0x41000300;   // ch10: to memory, started
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
// EE's channel pops to learn where the bytes go. A run of send blocks is one
// transfer to the EE's channel, which stops on the last block's tag.
constexpr uint32_t kTagEnd = 0x80000000;        // last send block of this run
constexpr uint32_t kTagDest = 0x10000000;       // an EE destination tag: `cnt`
constexpr uint32_t kTagDestEnd = 0x90000000;    // `cnt` with the interrupt bit,
                                                // which is what the reference
                                                // writes on a packet

constexpr uint32_t kSif0Irq = 0x2A;             // IOP_IRQ_DMA_SIF0 [header]

// What our outgoing channel reads at TADR: where the bytes are and how many,
// then the tag the EE's channel pops.
struct SendBlock {
    uint32_t address;       // | kTagEnd on the last of a run
    uint32_t words;
    uint32_t tag;           // kTagDest[End] | quadwords
    uint32_t destination;
};

// BOOT-12g: the sender is a ring of runs, not one slot. The reference chains
// a run of transfers, and a client that sends a burst depends on it -- a
// title's own SIF bridge registers well over a hundred channels back to back,
// and a sender with one slot refuses all but the first. Refusing is correct
// only when there is genuinely no room: BOOT-12g's `0` means "not queued",
// and a client loops on it.
//
// A group is one run: its blocks go out back to back, and its completion
// callback fires when the last of them has. The blocks are the DMA's own tag
// list, so a group's storage is what `TADR` points at and must not move while
// the channel is on it -- which is why the ring holds them rather than one
// shared pair.
constexpr uint32_t kTransfersMax = 2;
constexpr uint32_t kSendGroups = 32;

struct alignas(16) SendGroup {
    SendBlock blocks[kTransfersMax];
    uint32_t count;
    void (*done)(void *);
    void *arg;
    uint32_t reserved;                     // keeps `blocks` quadword-aligned
};

SendGroup send_queue[kSendGroups];
volatile uint32_t send_head;                // the group the channel is on
volatile uint32_t send_queued;              // queued groups, the running one included

uint32_t ee_area;                           // what the EE published at the handshake
uint32_t sif_initialised;                   // BOOT-12h: the latch ordinal 29 reads

extern "C" {
int _import_intrman_register(uint32_t irq, uint32_t mode, int (*handler)(void *), void *arg);
int _import_intrman_enable(uint32_t irq);
int _import_intrman_suspend(uint32_t *state);
int _import_intrman_resume(uint32_t state);
}

[[nodiscard]] uint32_t readWord(uintptr_t address) {
    return *reinterpret_cast<volatile uint32_t *>(address);
}

void writeWord(uintptr_t address, uint32_t value) {
    *reinterpret_cast<volatile uint32_t *>(address) = value;
}

void barrier() {
    asm volatile("" ::: "memory");
}

void waitUntilSet(uintptr_t address, uint32_t bits) {
    while ((readWord(address) & bits) == 0) {
    }
}

[[nodiscard]] uint32_t quadwords(uint32_t words) {
    return (words + 3) / 4;
}

// One send block: `words` of `from` to `destination` in EE memory, tagged so
// the EE's channel stops there or goes on. BOOT-11k: the EE's client names
// its buffers through an uncached alias, and what goes into the tag has to be
// the physical address. The bus moves quadwords and the EE's channel counts
// them, so a short run is padded up to one; every source here has room.
void describe(SendBlock &block, uint32_t from, uint32_t words,
              uint32_t destination, bool last, bool ends_ee_channel) {
    const uint32_t quads = quadwords(words);
    block.address = from | (last ? kTagEnd : 0);
    block.words = quads * 4;
    block.tag = (last && ends_ee_channel ? kTagDestEnd : kTagDest) | quads;
    block.destination = destination & 0x1FFFFFFF;
}

// Start the run at the head of the ring: BOOT-11c, then the channel.
void startHead() {
    writeWord(ps2::sif::kCtrl, kCtrlSif0Path);   // BOOT-11f, for this path
    barrier();
    writeWord(kDmaSif0 + kTadr,
              reinterpret_cast<uintptr_t>(send_queue[send_head].blocks));
    writeWord(kDmaSif0 + kBcr, kDmaBlock);
    writeWord(kDmaSif0 + kChcr, kDmaSendChcr);   // start
}

// Queue one run, and start it when the channel is idle. Answers 0 only when
// the ring is full, which is BOOT-12g's "not queued". The caller must have
// interrupts closed: the sending channel's own handler walks the same ring.
[[nodiscard]] int enqueue(const SendBlock *blocks, uint32_t count,
                          void (*done)(void *), void *arg) {
    if (count == 0 || count > kTransfersMax || send_queued == kSendGroups) {
        return 0;
    }
    SendGroup &group = send_queue[(send_head + send_queued) % kSendGroups];
    for (uint32_t k = 0; k < count; k++) {
        group.blocks[k] = blocks[k];
    }
    group.count = count;
    group.done = done;
    group.arg = arg;
    const bool idle = send_queued == 0;
    send_queued++;
    if (idle) {
        startHead();
    }
    return 1;
}

// The sending channel's interrupt: the head group has gone. Its callback runs
// after the next run is started, so a callback that queues another send finds
// the channel already busy rather than racing it.
int sendFinished(void *) {
    if (send_queued == 0) {
        return 1;                                // not one of ours
    }
    SendGroup &group = send_queue[send_head];
    void (*const callback)(void *) = group.done;
    void *const arg = group.arg;
    group.done = nullptr;
    send_head = (send_head + 1) % kSendGroups;
    send_queued--;
    if (send_queued != 0) {
        startHead();
    }
    if (callback != nullptr) {
        callback(arg);
    }
    return 1;
}

// Ordinals 7 and 32. Each descriptor's `attr` decides whether its block ends
// the EE's channel; only the last block of a run can, since the tag bit that
// says so is the one that also stops the run.
[[nodiscard]] int queueTransfers(const Transfer *list, uint32_t count,
                                 void (*function)(void *), void *arg) {
    if (count == 0 || count > kTransfersMax) {
        return 0;                                // BOOT-12g: not queued
    }
    SendBlock run[kTransfersMax];
    for (uint32_t k = 0; k < count; k++) {
        describe(run[k], list[k].src, (list[k].size + 3) / 4, list[k].dest,
                 k + 1 == count, (list[k].attr & ps2::sif::kAttrEndEe) != 0);
    }
    uint32_t state;
    _import_intrman_suspend(&state);
    const int answer = enqueue(run, count, function, arg);
    _import_intrman_resume(state);
    return answer;
}

int sifSetDma(const Transfer *list, uint32_t count) {
    return queueTransfers(list, count, nullptr, nullptr);
}

// sifman 32: ordinal 7 with a completion callback, called once from the
// sending channel's interrupt after the whole run has gone. A null callback
// makes it ordinal 7 exactly.
int sifSetDmaIntr(const Transfer *list, uint32_t count, void (*function)(void *),
                  void *arg) {
    return queueTransfers(list, count, function, arg);
}

int sifDmaStat(uint32_t) {
    return send_queued != 0 ? 0 : -1;
}

// sifman ordinal 6, `sceSifSetDChain`: arm the receiving channel. `SIFCMD`
// calls it once its buffer is ready for a packet, and `REBOOT` needs it
// because a reboot discards the channel's in-flight state (docs/analysis/45
// §1's outside lead, and the same thing an emulator does on the reset
// command).
int sifSetDChain() {
    writeWord(ps2::sif::kCtrl, kCtrlSif1Path);   // BOOT-11f, for this path
    writeWord(kDmaSif1 + kBcr, kDmaBlock);
    writeWord(kDmaSif1 + kChcr, kDmaRecvChcr);
    return 0;
}

// sifman ordinals 5 and 29 (BOOT-12h). BOOT-10: the IOP boot does not end at
// the last module of the list; it ends waiting for the EE. The EE publishes
// an address and raises a bit in MSFLG; this answers with a bit in SMFLG --
// over the address `SIFCMD` has already put in SMCOM -- then clears what the
// EE raised. The reference's `sceSifInit` is idempotent through a latch and
// `sceSifCheckInit` reads that same latch; every client is written as
// `if (!CheckInit()) Init()`.
int sifInit() {
    if (sif_initialised != 0) {
        return 0;
    }
    waitUntilSet(ps2::sif::kMsflg, ps2::sif::kFlagSifInit);

    // BOOT-11f: open both data paths first, one bit per write.
    writeWord(ps2::sif::kCtrl, kCtrlSif0Path);
    writeWord(ps2::sif::kCtrl, kCtrlSif1Path);

    // BOOT-10b: answer with a bit of our own, and record what the EE
    // published. **The EE's bit is left standing.** A write to MSFLG from
    // this side would clear it, and a soft reboot could then never finish:
    // the EE raises that bit once, at its own initialisation, and
    // `sceSifIopReset` clears only SMFLG's three (read out of the SDK's own
    // `SifIopReset`). A rebooted kernel that waited for it again would wait
    // for a client that is itself waiting for `SIF_STAT_BOOTEND`.
    writeWord(ps2::sif::kSmflg, ps2::sif::kFlagSifInit);   // our write sets
    ee_area = readWord(ps2::sif::kMscom);

    sif_initialised = 1;
    return 0;
}

int sifCheckInit() {
    return static_cast<int>(sif_initialised);
}

// Ordinal 22 writes MSFLG and ordinal 24 SMFLG (docs/analysis/34 §0, against
// the two register addresses). From this side a write to SMFLG only ever
// *sets* (BOOT-10c), which is what makes ordinal 24 safe to call after a
// reboot; the same write to MSFLG *clears*, so ordinal 22 takes the EE's
// flags down rather than putting them up.
int sifSetMsFlag(uint32_t bits) {
    writeWord(ps2::sif::kMsflg, bits);
    return 0;
}

int sifSetSmFlag(uint32_t bits) {
    writeWord(ps2::sif::kSmflg, bits);
    return 0;
}

[[gnu::used]] ps2::module::ExportTable<36> sifman_exports = {
    ps2::module::kExportMagic,
    0,
    0x0101,
    0,
    {'s', 'i', 'f', 'm', 'a', 'n', 0, 0},
    {
        ps2::module::slot(ps2::module::reservedHook),   // 0
        ps2::module::slot(ps2::module::reservedHook),   // 1
        ps2::module::slot(ps2::module::reservedHook),   // 2
        ps2::module::slot(ps2::module::reservedHook),   // 3
        ps2::module::slot(ps2::module::reservedHook),   // 4
        ps2::module::slot(sifInit),                     // 5  sceSifInit
        ps2::module::slot(sifSetDChain),                // 6  sceSifSetDChain
        ps2::module::slot(sifSetDma),                   // 7  sceSifSetDma
        ps2::module::slot(sifDmaStat),                  // 8  sceSifDmaStat
        ps2::module::slot(ps2::module::reservedHook),   // 9
        ps2::module::slot(ps2::module::reservedHook),   // 10
        ps2::module::slot(ps2::module::reservedHook),   // 11
        ps2::module::slot(ps2::module::reservedHook),   // 12
        ps2::module::slot(ps2::module::reservedHook),   // 13
        ps2::module::slot(ps2::module::reservedHook),   // 14
        ps2::module::slot(ps2::module::reservedHook),   // 15
        ps2::module::slot(ps2::module::reservedHook),   // 16
        ps2::module::slot(ps2::module::reservedHook),   // 17
        ps2::module::slot(ps2::module::reservedHook),   // 18
        ps2::module::slot(ps2::module::reservedHook),   // 19
        ps2::module::slot(ps2::module::reservedHook),   // 20
        ps2::module::slot(ps2::module::reservedHook),   // 21
        ps2::module::slot(sifSetMsFlag),                // 22 writes MSFLG
        ps2::module::slot(ps2::module::reservedHook),   // 23
        ps2::module::slot(sifSetSmFlag),                // 24 writes SMFLG
        ps2::module::slot(ps2::module::reservedHook),   // 25
        ps2::module::slot(ps2::module::reservedHook),   // 26
        ps2::module::slot(ps2::module::reservedHook),   // 27
        ps2::module::slot(ps2::module::reservedHook),   // 28
        ps2::module::slot(sifCheckInit),                // 29 sceSifCheckInit
        ps2::module::slot(ps2::module::reservedHook),   // 30
        ps2::module::slot(ps2::module::reservedHook),   // 31
        ps2::module::slot(sifSetDmaIntr),               // 32 sceSifSetDmaIntr
        ps2::module::slot(ps2::module::reservedHook),   // 33
        ps2::module::slot(ps2::module::reservedHook),   // 34
        ps2::module::slot(ps2::module::reservedHook),   // 35
        nullptr,
    },
};

}  // namespace

PS2_IMPORTS_BEGIN("intrman\0", 0x0102)
PS2_IMPORT(_import_intrman_register, 4)
PS2_IMPORT(_import_intrman_enable, 6)
PS2_IMPORT(_import_intrman_suspend, 17)
PS2_IMPORT(_import_intrman_resume, 18)
PS2_IMPORTS_END()

extern "C" {

// The module's entry, called by the loader as entry(argc, argv, 0, record)
// (spec/02 IRX-10). Nothing here waits for the EE: the handshake is ordinal
// 5, so that the module that owns the command buffer decides when it happens.
int _module_start(int, char **) {
    // BOOT-11j: enable the second bank's channels before anything uses them.
    writeWord(kDmaDpcr2, kDmaDpcr2All);
    writeWord(kDmaDmacen, 1);

    _import_intrman_register(kSif0Irq, 1, sendFinished, nullptr);
    _import_intrman_enable(kSif0Irq);

    return 0;                                    // resident
}

}  // extern "C"
