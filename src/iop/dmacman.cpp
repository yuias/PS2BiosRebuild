// DMACMAN: the IOP's DMA controller, register by register.
//
// docs/analysis/37 §3.6 reads this module as accessors over the two banks'
// channel registers and the three priority registers, with five operations
// on top [header dmacman.h]: set a channel's priority, enable or disable it
// in its DPCR nibble, describe a slice transfer, start it. SIO2MAN's start
// uses the priority and enable calls for channels 11 and 12 (spec/06
// IOP-6a); the rest are here so any module's import of the library binds.

#include "module.hpp"

#include <stdint.h>

namespace {

using ps2::module::ExportTable;
using ps2::module::reservedHook;
using ps2::module::slot;

constexpr uintptr_t kBank1 = 0xBF801080;        // channels 0..6, 0x10 apart
constexpr uintptr_t kBank2 = 0xBF801500;        // channels 7..13
constexpr uintptr_t kDpcr = 0xBF8010F0;
constexpr uintptr_t kDpcr2 = 0xBF801570;
constexpr uintptr_t kDicr = 0xBF8010F4;
constexpr uintptr_t kDicr2 = 0xBF801574;
constexpr uintptr_t kUnnamed1578 = 0xBF801578;
constexpr uintptr_t kUnnamed157C = 0xBF80157C;
constexpr uintptr_t kDpcr3 = 0xBF8015F0;
constexpr uintptr_t kUnnamed4_9_a = 0xBF801450; // dmac_set_4_9_a [header]
constexpr uint32_t kChannels = 14;
constexpr uint32_t kChcrStart = 0x01000000;
constexpr uint32_t kChcrSlice = 0x00000200;

[[nodiscard]] uint32_t readWord(uintptr_t address) {
    return *reinterpret_cast<volatile uint32_t *>(address);
}

void writeWord(uintptr_t address, uint32_t value) {
    *reinterpret_cast<volatile uint32_t *>(address) = value;
}

[[nodiscard]] uintptr_t channelBase(uint32_t channel) {
    return channel < 7 ? kBank1 + channel * 0x10 : kBank2 + (channel - 7) * 0x10;
}

// A channel's nibble in DPCR (0..6), DPCR2 (7..13) or DPCR3 (14..).
[[nodiscard]] uintptr_t priorityRegister(uint32_t channel, uint32_t &shift) {
    if (channel < 7) {
        shift = channel * 4;
        return kDpcr;
    }
    if (channel < 14) {
        shift = (channel - 7) * 4;
        return kDpcr2;
    }
    shift = (channel - 14) * 4;
    return kDpcr3;
}

void setMadr(uint32_t channel, uint32_t value) { writeWord(channelBase(channel), value); }
uint32_t getMadr(uint32_t channel) { return readWord(channelBase(channel)); }
void setBcr(uint32_t channel, uint32_t value) { writeWord(channelBase(channel) + 4, value); }
uint32_t getBcr(uint32_t channel) { return readWord(channelBase(channel) + 4); }
void setChcr(uint32_t channel, uint32_t value) { writeWord(channelBase(channel) + 8, value); }
uint32_t getChcr(uint32_t channel) { return readWord(channelBase(channel) + 8); }
void setTadr(uint32_t channel, uint32_t value) { writeWord(channelBase(channel) + 0xC, value); }
uint32_t getTadr(uint32_t channel) { return readWord(channelBase(channel) + 0xC); }
void set49a(uint32_t value) { writeWord(kUnnamed4_9_a, value); }
uint32_t get49a() { return readWord(kUnnamed4_9_a); }
void setDpcr(uint32_t value) { writeWord(kDpcr, value); }
uint32_t getDpcr() { return readWord(kDpcr); }
void setDpcr2(uint32_t value) { writeWord(kDpcr2, value); }
uint32_t getDpcr2() { return readWord(kDpcr2); }
void setDpcr3(uint32_t value) { writeWord(kDpcr3, value); }
uint32_t getDpcr3() { return readWord(kDpcr3); }
void setDicr(uint32_t value) { writeWord(kDicr, value); }
uint32_t getDicr() { return readWord(kDicr); }
void setDicr2(uint32_t value) { writeWord(kDicr2, value); }
uint32_t getDicr2() { return readWord(kDicr2); }
void set157C(uint32_t value) { writeWord(kUnnamed157C, value); }
uint32_t get157C() { return readWord(kUnnamed157C); }
void set1578(uint32_t value) { writeWord(kUnnamed1578, value); }
uint32_t get1578() { return readWord(kUnnamed1578); }

// sceSetSliceDMA(channel, address, size, count, direction) [header]: a
// block transfer of `count` slices of `size` words, not yet started.
int setSliceDma(uint32_t channel, uint32_t address, uint32_t size, uint32_t count, uint32_t direction) {
    if (channel >= kChannels) {
        return 0;
    }
    setMadr(channel, address & 0x00FFFFFF);
    setBcr(channel, (count << 16) | (size & 0xFFFF));
    setChcr(channel, kChcrSlice | (direction & 1));
    return 1;
}

void startDma(uint32_t channel) {
    if (channel < kChannels) {
        setChcr(channel, getChcr(channel) | kChcrStart);
    }
}

void setDmaPriority(uint32_t channel, uint32_t priority) {
    uint32_t shift;
    const uintptr_t reg = priorityRegister(channel, shift);
    writeWord(reg, (readWord(reg) & ~(7u << shift)) | ((priority & 7) << shift));
}

void enableDmaChannel(uint32_t channel) {
    uint32_t shift;
    const uintptr_t reg = priorityRegister(channel, shift);
    writeWord(reg, readWord(reg) | (8u << shift));
}

void disableDmaChannel(uint32_t channel) {
    uint32_t shift;
    const uintptr_t reg = priorityRegister(channel, shift);
    writeWord(reg, readWord(reg) & ~(8u << shift));
}

int unimplemented() {
    return -1;
}

[[gnu::used]] ExportTable<36> dmacman_exports = {
    ps2::module::kExportMagic,
    0,
    0x0102,
    0,
    {'d', 'm', 'a', 'c', 'm', 'a', 'n', 0},
    {
        slot(reservedHook),             // 0
        slot(reservedHook),             // 1
        slot(reservedHook),             // 2
        slot(reservedHook),             // 3
        slot(setMadr),                  // 4
        slot(getMadr),                  // 5
        slot(setBcr),                   // 6
        slot(getBcr),                   // 7
        slot(setChcr),                  // 8
        slot(getChcr),                  // 9
        slot(setTadr),                  // 10
        slot(getTadr),                  // 11
        slot(set49a),                   // 12
        slot(get49a),                   // 13
        slot(setDpcr),                  // 14
        slot(getDpcr),                  // 15
        slot(setDpcr2),                 // 16
        slot(getDpcr2),                 // 17
        slot(setDpcr3),                 // 18
        slot(getDpcr3),                 // 19
        slot(setDicr),                  // 20
        slot(getDicr),                  // 21
        slot(setDicr2),                 // 22
        slot(getDicr2),                 // 23
        slot(set157C),                  // 24
        slot(get157C),                  // 25
        slot(set1578),                  // 26
        slot(get1578),                  // 27
        slot(setSliceDma),              // 28 sceSetSliceDMA
        slot(unimplemented),            // 29 dmac_set_dma_chained_spu_sif0
        slot(unimplemented),            // 30 dmac_set_dma_sif0
        slot(unimplemented),            // 31 dmac_set_dma_sif1
        slot(startDma),                 // 32 sceStartDMA
        slot(setDmaPriority),           // 33 sceSetDMAPriority
        slot(enableDmaChannel),         // 34 sceEnableDMAChannel
        slot(disableDmaChannel),        // 35 sceDisableDMAChannel
        nullptr,
    },
};

}  // namespace

extern "C" {

// What the reference's entry leaves in the priority registers and the
// second bank's enable (docs/analysis/37 §3.6).
int _module_start(int, char **) {
    writeWord(kDpcr, 0x07777777);
    writeWord(kDpcr2, 0x07777777);
    writeWord(kDpcr3, 0x00000777);
    writeWord(kUnnamed1578, 1);
    return 0;                           // resident
}

}  // extern "C"
