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
constexpr uint32_t kStatusInterrupts = 0x401;   // IEc and Im2, as running code sees them

// kerr.h [header]
constexpr int kOk = 0;
constexpr int kErrorIllegalIrq = -0x65;         // KE_ILLEGAL_INTRCODE
constexpr int kErrorAlreadyDisabled = -0x66;    // KE_CPUDI
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
uint32_t dispatch_mask;                         // IOP-2f: ordinals 15/16
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

int cpuSuspendIntr(uint32_t *state) {
    const uint32_t status = readStatus();
    if (state != nullptr) {
        *state = status & kStatusInterrupts;
    }
    writeStatus(status & ~kStatusInterrupts);
    return (status & 1) != 0 ? kOk : kErrorAlreadyDisabled;
}

int cpuResumeIntr(uint32_t state) {
    writeStatus((readStatus() & ~kStatusInterrupts) | (state & kStatusInterrupts));
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
// both, and I_MASK's DMA line must be open for either to reach the CPU.
int enableIntr(uint32_t irq) {
    uint32_t state;
    cpuSuspendIntr(&state);
    if (irq < kDmaFirst) {
        writeWord(kIntMask, readWord(kIntMask) | (1u << irq));
    } else if (irq < kDmaSecondBank - 1) {
        const uint32_t channel = irq - kDmaFirst;
        writeWord(kDicr, (readWord(kDicr) & ~kDicrFlags) | (1u << (16 + channel)) | kDmaMaster);
        writeWord(kIntMask, readWord(kIntMask) | (1u << kDmaLine));
    } else if (irq >= kDmaSecondBank && irq < kLines) {
        const uint32_t channel = irq - kDmaSecondBank;
        // DICR's bit 23 is the master for both banks (docs/analysis/37 §2.3).
        writeWord(kDicr2, (readWord(kDicr2) & ~kDicrFlags) | (1u << (16 + channel)));
        writeWord(kDicr, (readWord(kDicr) & ~kDicrFlags) | kDmaMaster);
        writeWord(kIntMask, readWord(kIntMask) | (1u << kDmaLine));
    }
    cpuResumeIntr(state);
    return kOk;
}

int disableIntr(uint32_t irq, uint32_t *was_pending) {
    uint32_t state;
    cpuSuspendIntr(&state);
    int result = kOk;
    uint32_t pending = 0;
    if (irq < kDmaFirst) {
        pending = (readWord(kIntMask) >> irq) & 1;
        writeWord(kIntMask, readWord(kIntMask) & ~(1u << irq));
    } else if (irq < kDmaSecondBank - 1) {
        const uint32_t channel = irq - kDmaFirst;
        const uint32_t dicr = readWord(kDicr);
        pending = (dicr >> (24 + channel)) & 1;
        writeWord(kDicr, (dicr & ~kDicrFlags) & ~(1u << (16 + channel)));
    } else if (irq >= kDmaSecondBank && irq < kLines) {
        const uint32_t channel = irq - kDmaSecondBank;
        const uint32_t dicr2 = readWord(kDicr2);
        pending = (dicr2 >> (24 + channel)) & 1;
        writeWord(kDicr2, (dicr2 & ~kDicrFlags) & ~(1u << (16 + channel)));
    } else {
        result = kErrorIllegalIrq;
    }
    if (was_pending != nullptr) {
        *was_pending = pending;
    }
    cpuResumeIntr(state);
    return result;
}

int disableDispatchIntr(uint32_t irq) {
    if (irq < 32) {
        dispatch_mask |= 1u << irq;
    }
    return kOk;
}

int enableDispatchIntr(uint32_t irq) {
    if (irq < 32) {
        dispatch_mask &= ~(1u << irq);
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

// For each channel whose flag and enable are both set: clear the flag --
// writing 1 to a flag bit clears it, and the others are written 0 so they
// stay -- then call its handler through the same table as every other line.
void serveBank(uintptr_t dicr, uint32_t first_irq) {
    const uint32_t value = readWord(dicr);
    const uint32_t enables = (value >> 16) & 0x7F;
    const uint32_t flags = (value >> 24) & 0x7F;
    for (uint32_t channel = 0; channel < 7; channel++) {
        const uint32_t bit = 1u << channel;
        if ((flags & enables & bit) == 0) {
            continue;
        }
        writeWord(dicr, (value & ~kDicrFlags) | (bit << 24));
        (void)callRegistered(first_irq + channel);
    }
}

int dmaDispatch(void *) {
    serveBank(kDicr, kDmaFirst);
    serveBank(kDicr2, kDmaSecondBank);
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
    // I_CTRL reads as its value and clears itself on the read; the gate is
    // put back as it was on the way out, as the reference's dispatcher does.
    const uint32_t gate = readWord(kIntCtrl);
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
    uint32_t *resume = nested ? frame : reschedule(frame);
    writeWord(kIntCtrl, gate);
    return resume;
}

// IOP-3h: the reschedule syscall. Its code field is THREADMAN's number;
// `0xc` is `CpuInvokeInKmode`, and any other just returns past the
// instruction.
//
// `CpuInvokeInKmode` calls $a0 with $a1..$a3 and answers in $v0. What it is
// for is a caller that needs to run with the exception's own privileges and
// its own stack -- `MODLOAD`'s reboot core, which never comes back, is the
// only user in this image (docs/analysis/45 §2).
//
// **The target runs with this module's `$gp`, not its own.** That is fine
// for a target that reaches nothing through it, which the reboot core does
// not; a target that does would need its `$gp` captured at the call and
// installed here, the way `src/iop/loader.hpp`'s `callEntry` does.
uint32_t *_intrman_syscall(uint32_t *frame, uint32_t instruction) {
    using ps2::context::slotOf;
    const uint32_t code = (instruction >> 6) & 0xFFFFF;
    if (code == 0x20) {
        return reschedule(frame);
    }
    if (code == 0xC) {
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
    new_context = nullptr;
    should_preempt = nullptr;

    _import_excepman_register(0, _intrman_interrupt_handler);
    _import_excepman_register(8, _intrman_syscall_handler);
    registerIntrHandler(kDmaLine, 1, dmaDispatch, nullptr);
    return 0;                                   // resident
}

}  // extern "C"
