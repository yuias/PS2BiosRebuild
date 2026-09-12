// The machine's configuration record, as an EE program reads it.
//
// docs/spec/06-iop-kernel.md IOP-13d/13e get the two blocks across the SIF and
// IOP-13g says what is in them. This is the reading half: the session, the
// retry the status word asks for, and block 1 taken apart into the fields
// IOP-13g names.
//
// Header over a real translation unit, for the reason `sifclient.hpp` gives:
// only EE programs use it, and each links its own copy.

#pragma once

#include <stdint.h>

namespace ps2::config {

// IOP-13g: two blocks of fifteen bytes, and only the second means anything.
inline constexpr uint32_t kBlockBytes = 15;
inline constexpr uint32_t kBlocks = 2;

// IOP-13g1's eight, in the order the index selects them.
inline constexpr uint32_t kLanguages = 8;
extern const char *const kLanguageNames[kLanguages];

struct Record {
    // False when the session never completed: no service, or the read gave
    // back fewer blocks than were asked for. Every field below is then zero,
    // which is not the same as an unconfigured machine and must not be shown
    // as one.
    bool read;

    // IOP-13g2. The reference's decoder returns this inverted; here it is the
    // bit as stored, so `true` means the machine has been through setup.
    bool configured;

    // IOP-13g1: false when byte +0's top three bits are zero, which is the
    // older record whose language is one bit rather than five.
    bool wide_language;
    uint32_t language;                  // an index, and it may exceed kLanguages
    const char *language_name;          // nullptr when it does

    int32_t timezone_minutes;           // IOP-13g, the hour of +2 bit 3 folded in
    bool clock_12_hour;
    // Decoded and carried, but nothing reads either yet: `kernel_bit` is the
    // argument EE syscall `0x4f` takes (IOP-13g), and no part of this program
    // calls that slot; the nine-bit field has no name in the analysis at all.
    // They are here so that the decode is complete rather than partial.
    bool kernel_bit;
    uint32_t unsettled;

    uint8_t raw[kBlocks * kBlockBytes];
};

// Run the session and decode what it brings back. Binds the service itself,
// so the caller needs only `initRpc` to have run.
[[nodiscard]] Record read();

}  // namespace ps2::config
