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

namespace {

using ps2::module::ExportTable;
using ps2::module::reservedHook;
using ps2::module::slot;

constexpr uint32_t kTimers = 6;
constexpr uintptr_t kWideFrom = 0xBF801480;      // RTC3..5 count in 32 bits
constexpr uintptr_t kHoldBase = 0xBF8014B0;      // IOP-7e: the hold registers
constexpr uintptr_t kHoldModeBase = 0xBF8014C0;
constexpr int kNotOwned = -150;                  // FreeHardTimer of a timer not held

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

uint8_t in_use[kTimers];                         // a count, not a flag (IOP-7b)

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

[[gnu::used]] ExportTable<17> timrman_exports = {
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
        nullptr,
    },
};

}  // namespace

extern "C" {

int _module_start(int, char **) {
    for (auto &count : in_use) {
        count = 0;
    }
    return 0;                           // resident
}

}  // extern "C"
