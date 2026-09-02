// TIMRMAN: the IOP's six hardware timers, handed out and programmed by
// register address.
//
// docs/spec/06-iop-kernel.md IOP-7, from docs/analysis/44. A timer id is its
// register base shifted right by two, so every accessor recovers the
// register from the id with one shift and no table; the table exists only
// for the allocator, which walks it in the reference's order -- RTC2, RTC5,
// RTC4, RTC3, RTC0, RTC1 -- and claims the first free timer whose source
// mask, size and prescale satisfy the request, prescale by `>=`, not equality
// (IOP-7b). That order is why THREADMAN's request for a 32-bit SYSCLOCK
// timer at prescale 1 gets RTC5 (IOP-7c).

#include "module.hpp"

#include <stdint.h>

// IOP-7f/7g: the ordinals past 16 own an interrupt handler, so this module
// binds `intrman` where it used to bind nothing.
extern "C" {
int _import_intrman_register(uint32_t irq, uint32_t mode, int (*handler)(void *), void *arg);
int _import_intrman_enable(uint32_t irq);
int _import_intrman_disable(uint32_t irq, uint32_t *was_pending);
int _import_intrman_suspend(uint32_t *state);
int _import_intrman_resume(uint32_t state);
int _import_intrman_query_context();
}

namespace {

using ps2::module::ExportTable;
using ps2::module::reservedHook;
using ps2::module::slot;

constexpr uint32_t kTimers = 6;
constexpr uintptr_t kWideFrom = 0xBF801480;      // RTC3..5 count in 32 bits
constexpr uintptr_t kHoldBase = 0xBF8014B0;      // IOP-7e: the hold registers
constexpr uintptr_t kHoldModeBase = 0xBF8014C0;

// IOP-7f: what the four ordinals past 16 answer. -150 is FreeHardTimer's own.
constexpr int kNotOwned = -150;                  // FreeHardTimer of a timer not held
constexpr int kBadTimerId = -151;
constexpr int kBadSource = -152;
constexpr int kBadPrescale = -153;
constexpr int kRunning = -154;                   // the call needs a stopped timer
constexpr int kNotSetUp = -155;                  // StartHardTimer before SetupHardTimer
constexpr int kNotRunning = -156;                // StopHardTimer of a stopped timer
constexpr int kIllegalContext = -100;
constexpr int kIllegalMode = -405;

// IOP-7g: the MODE the compare interrupt needs -- reset on compare, raise the
// interrupt, and keep raising it.
constexpr uint32_t kCompareMode = 0x58;
// IOP-7d: the flags the MODE register reports, and clears as it is read.
constexpr uint32_t kFlagCompare = 0x800;
constexpr uint32_t kFlagOverflow = 0x1000;
constexpr uint32_t kModeCount = 8;               // IOP-7f: `mode` is 0..7

// The source bits [header]: which clock a timer may count.
constexpr uint32_t kSysclock = 1;
constexpr uint32_t kPixel = 2;
constexpr uint32_t kHline = 4;
constexpr uint32_t kHold = 8;

struct Descriptor {
    uintptr_t base;
    uint32_t sources;
    uint32_t size;
    uint32_t prescale;
    uint32_t irq;
};

// IOP-7a: the six timers in the allocator's scan order.
constexpr Descriptor kTable[kTimers] = {
    {0xBF801120, kSysclock, 16, 8, 6},                          // RTC2
    {0xBF8014A0, kSysclock, 32, 256, 16},                       // RTC5
    {0xBF801490, kSysclock, 32, 256, 15},                       // RTC4
    {0xBF801480, kSysclock | kHline, 32, 1, 14},                // RTC3
    {0xBF801100, kSysclock | kPixel | kHold, 16, 1, 4},         // RTC0
    {0xBF801110, kSysclock | kHline | kHold, 16, 1, 5},         // RTC1
};

struct Critical {
    uint32_t state;
    Critical() { _import_intrman_suspend(&state); }
    ~Critical() { _import_intrman_resume(state); }
};

uint8_t in_use[kTimers];                         // a count, not a flag (IOP-7b)

// IOP-7f/7g: what a timer carries once a driver programs it through the
// ordinals past 16. The reference's own state is a 44-byte record per timer
// holding all of this plus what `kTable` already has; ours splits it, since
// `kTable` is const and this half is not.
struct TimerState {
    uint32_t setup_mode;        // what SetupHardTimer settled, and that it ran
    bool set_up;
    bool running;
    bool handler_installed;     // the ISR is on this timer's IRQ
    uint32_t compare;
    uint32_t compare_mode;      // kCompareMode while a compare handler is in
    int (*compare_handler)(void *);
    void *compare_arg;
    uint32_t overflow_mode;
    int (*overflow_handler)(void *);
    void *overflow_arg;
};

TimerState states[kTimers];

[[nodiscard]] uintptr_t registerOf(uint32_t timer_id) {
    return static_cast<uintptr_t>(timer_id << 2);
}

[[nodiscard]] bool isWide(uintptr_t reg) {
    return reg >= kWideFrom;
}

[[nodiscard]] uint32_t readCounterLike(uintptr_t reg) {
    if (isWide(reg)) {
        return *reinterpret_cast<volatile uint32_t *>(reg);
    }
    return *reinterpret_cast<volatile uint16_t *>(reg);
}

void writeCounterLike(uintptr_t reg, uint32_t value) {
    if (isWide(reg)) {
        *reinterpret_cast<volatile uint32_t *>(reg) = value;
    } else {
        *reinterpret_cast<volatile uint16_t *>(reg) = static_cast<uint16_t>(value);
    }
}

// Ordinal 3: the descriptor table, for whoever wants to look.
const void *getTimersTable() {
    return kTable;
}

// IOP-7b: ordinals 4 and 5, the scan; only 4 claims.
[[nodiscard]] int findTimer(uint32_t source, uint32_t size, uint32_t prescale, bool claim) {
    for (uint32_t k = 0; k < kTimers; k++) {
        const Descriptor &timer = kTable[k];
        if (in_use[k] != 0 && claim) {
            continue;
        }
        if ((source & timer.sources) == 0 || timer.size != size || timer.prescale < prescale) {
            continue;
        }
        if (claim) {
            in_use[k]++;
        }
        return static_cast<int>(timer.base >> 2);
    }
    return -1;
}

int allocHardTimer(uint32_t source, uint32_t size, uint32_t prescale) {
    return findTimer(source, size, prescale, true);
}

int referHardTimer(uint32_t source, uint32_t size, uint32_t, uint32_t) {
    return findTimer(source, size, 1, false);
}

// Ordinal 6: give a timer back; one count per allocation.
int freeHardTimer(uint32_t timer_id) {
    const uintptr_t reg = registerOf(timer_id);
    for (uint32_t k = 0; k < kTimers; k++) {
        if (kTable[k].base == reg && in_use[k] != 0) {
            in_use[k]--;
            return 0;
        }
    }
    return kNotOwned;
}

// IOP-7d: count at +0, mode at +4 (always 16 bits), compare at +8.
void setTimerMode(uint32_t timer_id, uint32_t mode) {
    *reinterpret_cast<volatile uint16_t *>(registerOf(timer_id) + 4) = static_cast<uint16_t>(mode);
}

uint32_t getTimerStatus(uint32_t timer_id) {
    return *reinterpret_cast<volatile uint16_t *>(registerOf(timer_id) + 4);
}

void setTimerCounter(uint32_t timer_id, uint32_t count) {
    writeCounterLike(registerOf(timer_id), count);
}

uint32_t getTimerCounter(uint32_t timer_id) {
    return readCounterLike(registerOf(timer_id));
}

void setTimerCompare(uint32_t timer_id, uint32_t compare) {
    writeCounterLike(registerOf(timer_id) + 8, compare);
}

uint32_t getTimerCompare(uint32_t timer_id) {
    return readCounterLike(registerOf(timer_id) + 8);
}

// IOP-7e: the hold registers, a separate block nothing on the boot path
// programs.
void setHoldMode(uint32_t hold, uint32_t mode) {
    *reinterpret_cast<volatile uint32_t *>(kHoldModeBase + hold * 4) = mode;
}

uint32_t getHoldMode(uint32_t hold) {
    return *reinterpret_cast<volatile uint32_t *>(kHoldModeBase + hold * 4);
}

uint32_t getHoldReg(uint32_t hold) {
    return *reinterpret_cast<volatile uint32_t *>(kHoldBase + hold * 4);
}

// Ordinal 16: the interrupt a timer raises, from its register base.
int getHardTimerIntrCode(uint32_t timer_id) {
    const uintptr_t reg = registerOf(timer_id);
    for (const Descriptor &timer : kTable) {
        if (timer.base == reg) {
            return static_cast<int>(timer.irq);
        }
    }
    return -1;
}

// --- IOP-7f: the ordinals a title's driver imports past 16 -------------------
//
// IOP-7h: the id stays the one ordinal 4 hands out. The reference's v1.03
// packs a table index into its top nibble and these four read it back; ours
// keeps the one encoding the rest of the library already uses and finds the
// index by the register, which no caller can tell apart.

[[nodiscard]] uint32_t indexOf(uint32_t timer_id) {
    const uintptr_t reg = registerOf(timer_id);
    for (uint32_t k = 0; k < kTimers; k++) {
        if (kTable[k].base == reg) {
            return k;
        }
    }
    return kTimers;
}

// The library's own handler, on every timer a driver sets up. Reading MODE is
// what acknowledges the interrupt (IOP-7d), so it is read once and both halves
// are decided from that one value.
int timerInterrupt(void *arg) {
    auto &state = *static_cast<TimerState *>(arg);
    const uint32_t index = static_cast<uint32_t>(&state - states);
    const uint32_t mode =
        *reinterpret_cast<volatile uint16_t *>(kTable[index].base + 4);
    if ((mode & kFlagOverflow) != 0 && state.overflow_mode != 0
        && state.overflow_handler != nullptr) {
        (void)state.overflow_handler(state.overflow_arg);
    }
    if ((mode & kFlagCompare) != 0 && state.compare_mode != 0
        && state.compare_handler != nullptr) {
        (void)state.compare_handler(state.compare_arg);
    }
    return 1;
}

// Ordinal 20: record the compare handler and the value it fires at. A null
// handler takes the compare interrupt back out.
int setTimerHandler(uint32_t timer_id, uint32_t compare, int (*handler)(void *),
                    void *arg) {
    const uint32_t index = indexOf(timer_id);
    if (index == kTimers) {
        return kBadTimerId;
    }
    Critical critical;
    TimerState &state = states[index];
    if (state.running) {
        return kRunning;
    }
    state.compare = compare;
    state.compare_handler = handler;
    state.compare_arg = arg;
    state.compare_mode = handler != nullptr ? kCompareMode : 0;
    return 0;
}

// Ordinal 22: settle the source, mode and prescale, and put the library's own
// handler on this timer's line -- once per timer, not once per call.
int setupHardTimer(uint32_t timer_id, uint32_t source, uint32_t mode,
                   uint32_t prescale) {
    if (_import_intrman_query_context()) {
        return kIllegalContext;
    }
    const uint32_t index = indexOf(timer_id);
    if (index == kTimers) {
        return kBadTimerId;
    }
    Critical critical;
    TimerState &state = states[index];
    if (state.running) {
        return kRunning;
    }
    if (!state.handler_installed) {
        const int registered = _import_intrman_register(
            kTable[index].irq, 1, timerInterrupt, &state);
        if (registered < 0) {
            return registered;
        }
        state.handler_installed = true;
    }
    if (mode >= kModeCount) {
        return kIllegalMode;
    }
    if ((source & kTable[index].sources) == 0) {
        return kBadSource;
    }
    if (prescale > kTable[index].prescale) {
        return kBadPrescale;
    }
    state.setup_mode = mode;
    state.set_up = true;
    return 0;
}

// Ordinal 23: MODE zero first, so the hardware is quiet while the compare is
// written; then the MODE that starts it (IOP-7g).
int startHardTimer(uint32_t timer_id) {
    const uint32_t index = indexOf(timer_id);
    if (index == kTimers) {
        return kBadTimerId;
    }
    Critical critical;
    TimerState &state = states[index];
    if (state.running) {
        return kRunning;
    }
    if (!state.set_up) {
        return kNotSetUp;
    }
    const uintptr_t reg = kTable[index].base;
    *reinterpret_cast<volatile uint16_t *>(reg + 4) = 0;
    writeCounterLike(reg, 0);
    writeCounterLike(reg + 8, state.compare);
    *reinterpret_cast<volatile uint16_t *>(reg + 4) =
        static_cast<uint16_t>(state.compare_mode | state.overflow_mode);
    _import_intrman_enable(kTable[index].irq);
    state.running = true;
    return 0;
}

// Ordinal 24: the inverse. The line goes quiet with the timer.
int stopHardTimer(uint32_t timer_id) {
    const uint32_t index = indexOf(timer_id);
    if (index == kTimers) {
        return kBadTimerId;
    }
    Critical critical;
    TimerState &state = states[index];
    if (!state.running) {
        return kNotRunning;
    }
    *reinterpret_cast<volatile uint16_t *>(kTable[index].base + 4) = 0;
    uint32_t was_pending = 0;
    _import_intrman_disable(kTable[index].irq, &was_pending);
    state.running = false;
    return 0;
}

[[gnu::used]] ExportTable<25> timrman_exports = {
    ps2::module::kExportMagic,
    0,
    0x0101,
    0,
    {'t', 'i', 'm', 'r', 'm', 'a', 'n', 0},
    {
        slot(reservedHook),             // 0
        slot(reservedHook),             // 1
        slot(reservedHook),             // 2
        slot(getTimersTable),           // 3
        slot(allocHardTimer),           // 4
        slot(referHardTimer),           // 5
        slot(freeHardTimer),            // 6
        slot(setTimerMode),             // 7
        slot(getTimerStatus),           // 8
        slot(setTimerCounter),          // 9
        slot(getTimerCounter),          // 10
        slot(setTimerCompare),          // 11
        slot(getTimerCompare),          // 12
        slot(setHoldMode),              // 13
        slot(getHoldMode),              // 14
        slot(getHoldReg),               // 15
        slot(getHardTimerIntrCode),     // 16
        // IOP-7f: the table reaches 24 because a title's own driver imports
        // 20, 22, 23 and 24 from the `timrman` its IOPRP carries, and an
        // ordinal past the end binds to `jr $ra` with no diagnostic. 17, 18
        // and 21 are unread (docs/analysis/49) and stay reserved hooks.
        slot(reservedHook),             // 17 GetTimerMode [header], unread
        slot(reservedHook),             // 18 unread
        slot(reservedHook),             // 19 reserved in the reference too
        slot(setTimerHandler),          // 20
        slot(reservedHook),             // 21 the overflow twin of 20, unread
        slot(setupHardTimer),           // 22
        slot(startHardTimer),           // 23
        slot(stopHardTimer),            // 24
        nullptr,
    },
};

}  // namespace

PS2_IMPORTS_BEGIN("intrman\0", 0x0102)
PS2_IMPORT(_import_intrman_register, 4)
PS2_IMPORT(_import_intrman_enable, 6)
PS2_IMPORT(_import_intrman_disable, 7)
PS2_IMPORT(_import_intrman_suspend, 17)
PS2_IMPORT(_import_intrman_resume, 18)
PS2_IMPORT(_import_intrman_query_context, 23)
PS2_IMPORTS_END()

extern "C" {

int _module_start(int, char **) {
    for (auto &count : in_use) {
        count = 0;
    }
    for (auto &state : states) {
        state.setup_mode = 0;
        state.set_up = false;
        state.running = false;
        state.handler_installed = false;
        state.compare = 0;
        state.compare_mode = 0;
        state.compare_handler = nullptr;
        state.compare_arg = nullptr;
        state.overflow_mode = 0;
        state.overflow_handler = nullptr;
        state.overflow_arg = nullptr;
    }
    return 0;                           // resident
}

}  // extern "C"
