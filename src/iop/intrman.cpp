// INTRMAN: the IOP's interrupt manager -- who gets told, and how.
//
// docs/spec/06-iop-kernel.md IOP-2. A driver registers a handler for one of
// the 0x2E interrupt sources (the I_STAT lines, then the two DMA controller
// banks' channels through DICR and DICR2) or the two software lines, and
// enables it; the exception handler in intrman.S lands in `dispatch` here,
// which finds the lowest pending source, acknowledges it before calling the
// handler, and restores or withholds its mask by what the handler answers.
// At the tail of a non-nested interrupt the two hooks THREADMAN installs
// decide whether another thread's frame is what returns (IOP-2j, IOP-3h).
//
// Two things are ours rather than the reference's (docs/implementation.md):
// the interrupt-enable calls edit Status directly instead of trapping into
// the syscall handler, and one module serves the machine instead of the
// reference's P/I pair -- this is the second-bank hardware the I variant is
// for.

#include "context.hpp"
#include "module.hpp"

#include <stdint.h>

namespace {

using ps2::module::ExportTable;
using ps2::module::reservedHook;
using ps2::module::slot;

constexpr uintptr_t kIntStat = 0xBF801070;
constexpr uintptr_t kIntMask = 0xBF801074;
constexpr uintptr_t kIntCtrl = 0xBF801078;      // the second gate: 1 lets I_STAT through
constexpr uintptr_t kDicr = 0xBF8010F4;
constexpr uintptr_t kDicr2 = 0xBF801574;

constexpr uint32_t kLines = 0x2E;               // IOP-2b: irq 0..0x2D
constexpr uint32_t kSoftwareFirst = 0x3E;       // and 0x3E, 0x3F
constexpr uint32_t kDmaLine = 3;                // IOP_IRQ_DMA [header]
constexpr uint32_t kDmaFirst = 0x20;            // bank 1: 0x20..0x26
constexpr uint32_t kDmaSecondBank = 0x28;       // bank 2: 0x28..0x2D
constexpr uint32_t kDmaMaster = 1u << 23;       // DICR's master enable
constexpr uint32_t kDicrFlags = 0x7F000000;     // write 1 to clear
// IOP-2h1: the force condition, and the table slot in the gap between the two
// banks' ranges that the dispatcher calls for it.
constexpr uint32_t kDmaForce = 1u << 15;
constexpr uint32_t kDmaForceIrq = 0x27;

// IOP-2c: `EnableIntr`/`DisableIntr` take the line in the **low byte** and
// read the rest as flags. The reference masks with `0xFF` before it compares
// anything, so a caller may pass `line | 0x100 | 0x200`; an implementation
// that tests the whole word against the line ranges matches nothing and
// enables nothing, silently. `SIFCMD` 2.08 enables SIF1 as `0x22b`.
constexpr uint32_t kLineMask = 0xFF;
constexpr uint32_t kFlagDicr = 0x100;           // also set DICR's low bit for the channel
constexpr uint32_t kFlagDicr2 = 0x200;          // also set DICR2's

// Bits 24..31 of both DICR registers are acknowledge-on-write, so a write
// that is configuring rather than acknowledging leaves them zero. This is the
// reference's own mask, wider than `kDicrFlags` by the master flag at 31.
constexpr uint32_t kDicrWritable = 0x00FFFFFF;
constexpr uint32_t kStatusInterrupts = 0x401;   // IEc and Im2, as running code sees them

// IOP-2k: the enable state as it travels *between* modules. A frame holds it
// one level down -- IEp, IEo and Im2 -- because the exception that built the
// frame pushed Status's stack; running code holds the same three as IEc, IEp
// and Im2. The saved-frame places are the ABI, because the value does not
// stay in this module: a thread manager takes it from ordinal 17 and hands it
// to the reschedule syscall as `$a2` (IOP-2k2), which merges it straight into
// a saved frame's Status.
constexpr uint32_t kStateSaved = 0x414;         // IEp, IEo, Im2
constexpr uint32_t kStateLive = 0x405;          // the same three, one level up

// kerr.h [header]
constexpr int kOk = 0;
constexpr int kErrorIllegalIrq = -0x65;         // KE_ILLEGAL_INTRCODE
constexpr int kErrorAlreadyDisabled = -0x66;    // KE_CPUDI
constexpr int kErrorNotEnabled = -0x67;         // KE_INTR_NOT_ENABLED
constexpr int kErrorInUse = -0x68;              // KE_FOUND_HANDLER
constexpr int kErrorSoftwareInUse = -0x69;

using Handler = int (*)(void *arg);
using NewContext = uint32_t *(*)(uint32_t *frame);
using ShouldPreempt = int (*)();

struct Registration {
    Handler handler;                            // IOP-2b: null when free
    void *arg;
    uint32_t mode;
};

Registration table[kLines];
Registration software[2];
uint32_t dispatch_mask;                         // IOP-2f: ordinals 15/16, irq < 0x20
// IOP-2f/2h: the same software gate for the two DMA banks, as the reference
// keeps it -- a word ANDed into the DICR/DICR2 read, a channel's flag at bit
// 24 + channel, all ones until DisableDispatchIntr clears one.
uint32_t bank_dispatch_mask[2];
NewContext new_context;
ShouldPreempt should_preempt;

[[nodiscard]] uint32_t readWord(uintptr_t address) {
    return *reinterpret_cast<volatile uint32_t *>(address);
}

void writeWord(uintptr_t address, uint32_t value) {
    *reinterpret_cast<volatile uint32_t *>(address) = value;
}

[[nodiscard]] uint32_t readStatus() {
    uint32_t value;
    asm volatile("mfc0 %0, $12\n\tnop" : "=r"(value));
    return value;
}

void writeStatus(uint32_t value) {
    asm volatile("mtc0 %0, $12\n\tnop" : : "r"(value) : "memory");
}

[[nodiscard]] uint32_t readCause() {
    uint32_t value;
    asm volatile("mfc0 %0, $13\n\tnop" : "=r"(value));
    return value;
}

void writeCause(uint32_t value) {
    asm volatile("mtc0 %0, $13\n\tnop" : : "r"(value) : "memory");
}

extern "C" uint8_t _intrman_stack[];
extern "C" uint8_t _intrman_stack_top[];

// IOP-2e: "in interrupt context" is a stack-pointer range, not a flag.
[[nodiscard]] int queryIntrStack(uint32_t sp) {
    return sp >= reinterpret_cast<uintptr_t>(_intrman_stack)
           && sp < reinterpret_cast<uintptr_t>(_intrman_stack_top);
}

[[nodiscard]] int queryIntrContext() {
    uint32_t sp;
    asm volatile("move %0, $sp" : "=r"(sp));
    return queryIntrStack(sp);
}

[[nodiscard]] Registration *registrationOf(uint32_t irq) {
    if (irq < kLines) {
        return &table[irq];
    }
    if (irq == kSoftwareFirst || irq == kSoftwareFirst + 1) {
        return &software[irq - kSoftwareFirst];
    }
    return nullptr;
}

// --- IOP-2k: the enable state, edited in place ---------------------------

// The reference's own 17 and 18 are the bodies of `syscall 0x10` and `0x14`,
// so they read and write a *frame's* Status and report `Status & 0x414`
// directly. These are plain functions reading the live Status, so the same
// three bits sit one level up and are shifted into the saved places on the
// way out and back down on the way in. Reporting this module's own view
// instead would be invisible until a thread manager from another image
// passed the value to `$a2` and had it land in the wrong Status bits.
[[nodiscard]] uint32_t savedFormOf(uint32_t status) {
    return ((status & 0x5) << 2) | (status & 0x400);
}

[[nodiscard]] uint32_t liveFormOf(uint32_t state) {
    return ((state & 0x14) >> 2) | (state & 0x400);
}

int cpuSuspendIntr(uint32_t *state) {
    const uint32_t status = readStatus();
    if (state != nullptr) {
        *state = savedFormOf(status);
    }
    writeStatus(status & ~kStatusInterrupts);
    return (status & 1) != 0 ? kOk : kErrorAlreadyDisabled;
}

int cpuResumeIntr(uint32_t state) {
    writeStatus((readStatus() & ~kStateLive) | liveFormOf(state));
    return kOk;
}

// IOP-2k: besides Status, the controller's own gate is opened here and
// never closed by the disabling calls -- the reference's asymmetry too.
int cpuEnableIntr() {
    writeWord(kIntCtrl, 1);
    writeStatus(readStatus() | kStatusInterrupts);
    return kOk;
}

int cpuDisableIntr() {
    writeStatus(readStatus() & ~kStatusInterrupts);
    return kOk;
}

// --- IOP-2b: registration ------------------------------------------------

int registerIntrHandler(uint32_t irq, uint32_t mode, Handler handler, void *arg) {
    if (queryIntrContext()) {
        return kOk;                             // IOP-2b: silently ignored
    }
    Registration *slot_for = registrationOf(irq);
    if (slot_for == nullptr) {
        return kErrorIllegalIrq;
    }
    uint32_t state;
    cpuSuspendIntr(&state);
    int result = kOk;
    if (slot_for->handler != nullptr) {
        result = irq < kLines ? kErrorInUse : kErrorSoftwareInUse;
    } else {
        slot_for->handler = handler;
        slot_for->arg = arg;
        slot_for->mode = irq < kDmaFirst ? (mode & 3) : 0;
    }
    cpuResumeIntr(state);
    return result;
}

int releaseIntrHandler(uint32_t irq) {
    if (queryIntrContext()) {
        return kOk;
    }
    Registration *slot_for = registrationOf(irq);
    if (slot_for == nullptr) {
        return kErrorIllegalIrq;
    }
    uint32_t state;
    cpuSuspendIntr(&state);
    int result = kOk;
    if (slot_for->handler == nullptr) {
        result = kErrorIllegalIrq;
    } else {
        slot_for->handler = nullptr;
        slot_for->arg = nullptr;
    }
    cpuResumeIntr(state);
    return result;
}

// --- IOP-2c: the mask registers -------------------------------------------

// The two banks' channel bits: DICR's bits 16..22 enable and 24..30 flag
// channels 0..6; DICR2's the same for 7..13. DICR's bit 23 is the master for
// both, and I_MASK's DMA line must be open for either to reach the CPU. Both
// registers also carry a low bit per channel -- DICR's at the channel index,
// DICR2's at index + 7 for the second bank -- which the argument's `0x100`
// and `0x200` flags select; the reference sets them here and reports them
// back out of `DisableIntr`.
int enableIntr(uint32_t line_and_flags) {
    const uint32_t line = line_and_flags & kLineMask;
    const uint32_t flags = line_and_flags & ~kLineMask;
    uint32_t state;
    cpuSuspendIntr(&state);
    int result = kOk;
    if (line < kDmaFirst) {
        writeWord(kIntMask, readWord(kIntMask) | (1u << line));
    } else if (line < kDmaSecondBank - 1) {
        const uint32_t channel = line - kDmaFirst;
        const uint32_t keep = kDicrWritable & ~(1u << channel);
        uint32_t bits = (1u << (16 + channel)) | kDmaMaster;
        if ((flags & kFlagDicr) != 0) {
            bits |= 1u << channel;
        }
        writeWord(kDicr, (readWord(kDicr) & keep) | bits);
        // The reference writes DICR2 here too: its own bits 0..23 back, plus
        // the channel's low bit when `0x200` is passed.
        const uint32_t keep2 = kDicrWritable & ~(1u << channel);
        const uint32_t bits2 = (flags & kFlagDicr2) != 0 ? (1u << channel) : 0;
        writeWord(kDicr2, (readWord(kDicr2) & keep2) | bits2);
        writeWord(kIntMask, readWord(kIntMask) | (1u << kDmaLine));
    } else if (line >= kDmaSecondBank && line < kLines) {
        const uint32_t channel = line - kDmaSecondBank;
        const uint32_t low = channel + 7;       // DICR2's low bit for this channel
        const uint32_t keep = kDicrWritable & ~(1u << low);
        uint32_t bits = 1u << (16 + channel);
        if ((flags & kFlagDicr2) != 0) {
            bits |= 1u << low;
        }
        writeWord(kDicr2, (readWord(kDicr2) & keep) | bits);
        // DICR's bit 23 is the master for both banks (docs/analysis/37 §2.3).
        writeWord(kDicr, (readWord(kDicr) & kDicrWritable) | kDmaMaster);
        writeWord(kIntMask, readWord(kIntMask) | (1u << kDmaLine));
    } else {
        result = kErrorIllegalIrq;
    }
    cpuResumeIntr(state);
    return result;
}

// The out-parameter is **not** a pending flag: the reference reconstructs the
// argument that would re-enable this line -- the line plus whichever of the
// two low bits it found set -- and leaves `-0x67` there when it refuses. A
// line that was not enabled is `-0x67`, not success.
int disableIntr(uint32_t line_and_flags, uint32_t *previous) {
    const uint32_t line = line_and_flags & kLineMask;
    uint32_t state;
    cpuSuspendIntr(&state);
    int result = kOk;
    auto restore = static_cast<uint32_t>(kErrorNotEnabled);
    if (line < kDmaFirst) {
        const uint32_t bit = 1u << line;
        const uint32_t mask = readWord(kIntMask);
        writeWord(kIntMask, mask & ~bit);
        if ((mask & bit) == 0) {
            result = kErrorNotEnabled;
        } else {
            restore = line;
        }
    } else if (line < kDmaSecondBank - 1) {
        const uint32_t channel = line - kDmaFirst;
        const uint32_t enable = 1u << (16 + channel);
        const uint32_t value = readWord(kDicr) & kDicrWritable;
        if ((value & enable) == 0) {
            result = kErrorNotEnabled;
        } else {
            restore = line;
            if (((value >> channel) & 1) != 0) {
                restore |= kFlagDicr;
            }
            if ((readWord(kDicr2) & (1u << channel)) != 0) {
                restore |= kFlagDicr2;
            }
            writeWord(kDicr, value & ~enable);
        }
    } else if (line >= kDmaSecondBank && line < kLines) {
        const uint32_t channel = line - kDmaSecondBank;
        const uint32_t enable = 1u << (16 + channel);
        const uint32_t value = readWord(kDicr2) & kDicrWritable;
        if ((value & enable) == 0) {
            result = kErrorNotEnabled;
        } else {
            restore = line;
            if (((value >> (channel + 7)) & 1) != 0) {
                restore |= kFlagDicr2;
            }
            writeWord(kDicr2, value & ~enable);
        }
    } else {
        result = kErrorIllegalIrq;
    }
    if (previous != nullptr) {
        *previous = restore;
    }
    cpuResumeIntr(state);
    return result;
}

// The reference answers nothing readable for an irq past the second bank
// (the spec leaves it open); this keeps the earlier behaviour of doing
// nothing and reporting success.
int disableDispatchIntr(uint32_t irq) {
    if (irq < kDmaFirst) {
        dispatch_mask |= 1u << irq;
    } else if (irq < kDmaSecondBank) {
        bank_dispatch_mask[0] &= ~(1u << (24 + (irq - kDmaFirst)));
    } else if (irq < kLines) {
        bank_dispatch_mask[1] &= ~(1u << (24 + (irq - kDmaSecondBank)));
    }
    return kOk;
}

int enableDispatchIntr(uint32_t irq) {
    if (irq < kDmaFirst) {
        dispatch_mask &= ~(1u << irq);
    } else if (irq < kDmaSecondBank) {
        bank_dispatch_mask[0] |= 1u << (24 + (irq - kDmaFirst));
    } else if (irq < kLines) {
        bank_dispatch_mask[1] |= 1u << (24 + (irq - kDmaSecondBank));
    }
    return kOk;
}

// --- IOP-2h: the DMA line's own handler, one per bank -----------------------

[[nodiscard]] int callRegistered(uint32_t irq) {
    const Registration &registration = table[irq];
    if (registration.handler == nullptr) {
        return 1;
    }
    return registration.handler(registration.arg);
}

// For each channel whose flag is set -- the flag alone, as IOP-2h reads the
// reference: a channel whose enable is off still gets its handler when some
// other channel's interrupt brings the dispatcher here. The disc's CDVDMAN
// depends on exactly that, having disabled channel 3 and then kicked the
// last chunk of a read; SPU2's channel 4 completing is what delivers it.
// Clear the flag -- writing 1 to a flag bit clears it, and the others are
// written 0 so they stay -- then call its handler through the same table as
// every other line.
// The bank's software gate (ordinals 15/16) is applied to the flags read,
// not to the hardware: a gated channel's flag stays set and is served once
// the gate opens again, which is what the reference's AND-then-shift does.
// Bank 1 has seven channels, bank 2 six (IOP-2i).
[[nodiscard]] uint32_t bankFlags(uintptr_t dicr, uint32_t channels, uint32_t gate) {
    return ((readWord(dicr) & gate) >> 24) & ((1u << channels) - 1);
}

void serveBank(uintptr_t dicr, uint32_t first_irq, uint32_t channels, uint32_t gate) {
    const uint32_t flags = bankFlags(dicr, channels, gate);
    for (uint32_t channel = 0; channel < channels; channel++) {
        const uint32_t bit = 1u << channel;
        if ((flags & bit) == 0) {
            continue;
        }
        // Read again for the ack: a handler served earlier in this walk may
        // have changed the enables, and a stale copy would put them back.
        writeWord(dicr, (readWord(dicr) & ~kDicrFlags) | (bit << 24));
        (void)callRegistered(first_irq + channel);
    }
}

// IOP-2h2: round again until neither bank has a masked flag and the force bit
// is clear, so a channel that re-asserts while a sibling's handler runs is
// served here rather than at the next hardware edge. Only flags the shadow
// mask lets through count as pending, which is what keeps a gated channel
// from holding the loop for ever.
int dmaDispatch(void *) {
    for (;;) {
        const uint32_t bank1_word = readWord(kDicr) & bank_dispatch_mask[0];
        const uint32_t bank1 = (bank1_word >> 24) & 0x7F;
        const uint32_t bank2 = bankFlags(kDicr2, 6, bank_dispatch_mask[1]);
        const uint32_t force = bank1_word & kDmaForce;
        if ((bank1 | bank2 | force) == 0) {
            break;
        }
        if (force != 0) {
            // IOP-2h1: clear bit 15 with zeroes in the acknowledge bits, so
            // no channel's flag is lost with it, then call the slot between
            // the banks. Nothing here registers one, and the reference's own
            // boot leaves it null too (docs/analysis/37 §3.4.1).
            writeWord(kDicr, readWord(kDicr) & kDicrWritable & ~kDmaForce);
            (void)callRegistered(kDmaForceIrq);
        }
        serveBank(kDicr, kDmaFirst, 7, bank_dispatch_mask[0]);
        serveBank(kDicr2, kDmaSecondBank, 6, bank_dispatch_mask[1]);
    }

    // IOP-2h3: pulse the master enable. Every acknowledge above writes the
    // whole register, and a master enable one of them dropped would stop
    // every later DMA interrupt with nothing else to show for it. The wait is
    // for this handler's own clear to read back, not for a flag to drain.
    writeWord(kDicr, readWord(kDicr) & kDicrWritable & ~kDmaMaster);
    while ((readWord(kDicr) & kDmaMaster) != 0) {
    }
    writeWord(kDicr, (readWord(kDicr) & kDicrWritable) | kDmaMaster);
    return 1;                                   // IOP-2h: the line stays open
}

// --- the hooks (IOP-2j) --------------------------------------------------

void setNewCtxCb(NewContext callback) {
    new_context = callback;
}

void resetNewCtxCb() {
    new_context = nullptr;
}

void setShouldPreemptCb(ShouldPreempt callback) {
    should_preempt = callback;
}

void resetShouldPreemptCb() {
    should_preempt = nullptr;
}

[[nodiscard]] uint32_t *reschedule(uint32_t *frame) {
    if (should_preempt != nullptr && new_context != nullptr && should_preempt()) {
        return new_context(frame);
    }
    return frame;
}

int unimplemented() {
    return -1;
}

// intrman.S: ordinal 14's body, which is one `syscall` and a return.
extern "C" void _intrman_invoke_in_kmode();

// IRX-4: the export table, IOP-2a's ordinals.
PS2_EXPORT_TABLE ExportTable<32> intrman_exports = {
    ps2::module::kExportMagic,
    0,
    0x0102,
    0,
    {'i', 'n', 't', 'r', 'm', 'a', 'n', 0},
    {
        slot(reservedHook),             // 0
        slot(reservedHook),             // 1
        slot(reservedHook),             // 2
        slot(unimplemented),            // 3  GetIntrmanInternalData
        slot(registerIntrHandler),      // 4
        slot(releaseIntrHandler),       // 5
        slot(enableIntr),               // 6
        slot(disableIntr),              // 7
        slot(cpuDisableIntr),           // 8
        slot(cpuEnableIntr),            // 9
        slot(cpuDisableIntr),           // 10 the bare forms
        slot(cpuEnableIntr),            // 11
        slot(unimplemented),            // 12
        slot(unimplemented),            // 13
        slot(_intrman_invoke_in_kmode), // 14 CpuInvokeInKmode
        slot(disableDispatchIntr),      // 15
        slot(enableDispatchIntr),       // 16
        slot(cpuSuspendIntr),           // 17
        slot(cpuResumeIntr),            // 18
        slot(cpuSuspendIntr),           // 19 aliases of 17, 18 (IRX-6b)
        slot(cpuResumeIntr),            // 20
        slot(cpuSuspendIntr),           // 21 the bare forms
        slot(cpuResumeIntr),            // 22
        slot(queryIntrContext),         // 23
        slot(queryIntrStack),           // 24
        slot(unimplemented),            // 25 iCatchMultiIntr
        slot(reservedHook),             // 26
        slot(unimplemented),            // 27
        slot(setNewCtxCb),              // 28
        slot(resetNewCtxCb),            // 29
        slot(setShouldPreemptCb),       // 30
        slot(resetShouldPreemptCb),     // 31
        nullptr,                        // IRX-5b
    },
};

}  // namespace

PS2_IMPORTS_BEGIN("excepman", 0x0101)
PS2_IMPORT(_import_excepman_register, 4)
PS2_IMPORT(_import_excepman_register_priority, 5)
PS2_IMPORTS_END()

PS2_IMPORTS_BEGIN("loadcore", 0x0101)
PS2_IMPORT(_import_loadcore_register, 6)
PS2_IMPORTS_END()

extern "C" {

int _import_excepman_register(uint32_t cause, void *handler);
int _import_excepman_register_priority(uint32_t cause, uint32_t priority, void *handler);
int _import_loadcore_register(void *table);

// intrman.S: the two handler structures.
extern uint32_t _intrman_interrupt_handler[];
extern uint32_t _intrman_syscall_handler[];

// IOP-2f/2g: one interrupt per exception. The lowest pending line that is
// enabled and not held back by the software mask is acknowledged -- I_STAT
// clears where a 0 is written -- and masked while its handler runs; the
// handler's answer decides whether the mask comes back. The software lines
// are Cause's own two bits, cleared there.
uint32_t *_intrman_dispatch(uint32_t *frame, uint32_t cause) {
    const bool nested = queryIntrStack(frame[ps2::context::slotOf(29)]);
    // The I_CTRL gate is the frame's, not this dispatcher's: intrman.S saved
    // it on entry and the return installs the *resumed* frame's copy, which
    // is the reference's rule and the only one a context switch can honour.
    if (cause & 0x300) {
        const uint32_t line = (cause & 0x100) != 0 ? 0 : 1;
        writeCause(readCause() & ~(0x100u << line));
        const Registration &registration = software[line];
        if (registration.handler != nullptr) {
            registration.handler(registration.arg);
        }
    } else {
        const uint32_t pending = readWord(kIntStat) & readWord(kIntMask) & ~dispatch_mask;
        if (pending != 0) {
            uint32_t irq = 0;
            while ((pending & (1u << irq)) == 0) {
                irq++;
            }
            const uint32_t bit = 1u << irq;
            writeWord(kIntStat, ~bit);
            const uint32_t mask = readWord(kIntMask);
            writeWord(kIntMask, mask & ~bit);
            int keep = 1;
            if (table[irq].handler != nullptr) {
                keep = table[irq].handler(table[irq].arg);
            }
            if (keep != 0) {
                writeWord(kIntMask, readWord(kIntMask) | bit);
            }
        }
    }
    return nested ? frame : reschedule(frame);
}

// IOP-3h: the kernel's own syscalls. **The number is in `$v0`, not in the
// instruction's code field** -- every reference caller emits a bare `syscall`
// after loading `$v0`, which is what `intrman` ordinal 14 is in the reference
// (`addiu $v0, $zero, 0xc` then `syscall`) and what the reference's thread
// manager traps with to reschedule (`$v0 = 0x20`). A handler that reads the
// code field instead sees `0` from all of them and returns without doing
// anything, silently: that is exactly what the merged kernel's `THREADMAN`
// 2.03 ran into, leaving its dispatcher never entered and interrupts off for
// the rest of the boot.
//
// `0x20` reschedules. `0xc` is `CpuInvokeInKmode`: it calls `$a0` with
// `$a1..$a3` and answers in `$v0`, for a caller that needs the exception's
// own privileges and stack -- `MODLOAD`'s reboot core, which never comes
// back, is the only user in this image (docs/analysis/45 §2).
//
// **The target runs with this module's `$gp`, not its own.** That is fine
// for a target that reaches nothing through it, which the reboot core does
// not; a target that does would need its `$gp` captured at the call and
// installed here, the way `src/iop/loader.hpp`'s `callEntry` does.
uint32_t *_intrman_syscall(uint32_t *frame) {
    using ps2::context::slotOf;
    const uint32_t number = frame[slotOf(2)];
    if (number == 0x20) {
        // IOP-2k2: a voluntary switch carries three arguments. `$a0` and
        // `$a1` are what the blocked call answers with once its thread is
        // resumed, and `$a2` the interrupt state to resume under -- the same
        // word ordinal 17 reported. A handler that drops `$a2` leaves the
        // caller under the trap's own state, which is interrupts off, for the
        // rest of its life; that is what stopped the merged kernel, whose
        // `THREADMAN` 2.03 blocks this way and nothing else.
        //
        // The mask on `$a2` is a deviation and docs/implementation.md records
        // it: the reference ORs the word in whole.
        frame[slotOf(2)] = frame[slotOf(4)];
        frame[slotOf(3)] = frame[slotOf(5)];
        frame[ps2::context::kStatus] =
            (frame[ps2::context::kStatus] & ~kStateSaved) | (frame[slotOf(6)] & kStateSaved);
        return reschedule(frame);
    }
    if (number == 0xC) {
        const auto function = reinterpret_cast<uint32_t (*)(uint32_t, uint32_t, uint32_t)>(
            frame[slotOf(4)]);
        if (function != nullptr) {
            frame[slotOf(2)] = function(frame[slotOf(5)], frame[slotOf(6)],
                                        frame[slotOf(7)]);
        }
        return frame;
    }
    return frame;
}

// IRX-12: the entry. IOP-2d: the interrupt and syscall handlers go to
// EXCEPMAN, the DMA line's handler to this module's own table, and every
// mask starts closed.
int _module_start(int, char **) {
    if (_import_loadcore_register(&intrman_exports) < 0) {
        return 1;
    }
    writeWord(kIntMask, 0);
    writeWord(kDicr, 0);
    writeWord(kDicr2, 0);
    writeWord(kIntStat, 0);
    for (Registration &registration : table) {
        registration.handler = nullptr;
    }
    software[0].handler = nullptr;
    software[1].handler = nullptr;
    dispatch_mask = 0;
    bank_dispatch_mask[0] = bank_dispatch_mask[1] = ~0u;
    new_context = nullptr;
    should_preempt = nullptr;

    _import_excepman_register(0, _intrman_interrupt_handler);
    _import_excepman_register(8, _intrman_syscall_handler);
    registerIntrHandler(kDmaLine, 1, dmaDispatch, nullptr);
    return 0;                                   // resident
}

}  // extern "C"
