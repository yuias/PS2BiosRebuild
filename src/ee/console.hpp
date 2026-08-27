// The EE's serial console, for the programs the boot runs.
//
// The only output an image with no display has, and the only reason every
// stage of the boot can say where it got to. The kernel has its own copy in
// assembly (`_print`, reached from the vector page); this is the same two
// registers for programs, which cannot call into the kernel's.
//
// Header-only and `inline`: it is a handful of instructions per call site and
// each program links it on its own, the way `sifclient.hpp` describes.

#pragma once

#include <stdint.h>

namespace ps2::console {

inline constexpr uintptr_t kSioIsr = 0xB000F130;
inline constexpr uintptr_t kSioTx = 0xB000F180;
inline constexpr uint32_t kSioTxReady = 0x8000;

inline void print(const char *text) {
    for (const char *at = text; *at != '\0'; at++) {
        while ((*reinterpret_cast<volatile uint32_t *>(kSioIsr) & kSioTxReady) != 0) {
        }
        *reinterpret_cast<volatile uint8_t *>(kSioTx) = static_cast<uint8_t>(*at);
    }
}

// A driver's or a loader's refusal is a small negative number, and which
// number it is is the whole diagnosis.
inline void printSigned(int32_t value) {
    if (value < 0) {
        print("-");
    }
    uint32_t magnitude = value < 0 ? -static_cast<uint32_t>(value) : value;
    char digits[12];
    uint32_t n = 0;
    do {
        digits[n++] = static_cast<char>('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude != 0);
    char text[13];
    for (uint32_t k = 0; k < n; k++) {
        text[k] = digits[n - 1 - k];
    }
    text[n] = '\0';
    print(text);
}

inline void printHex(uint32_t value) {
    char text[11] = {'0', 'x'};
    for (uint32_t k = 0; k < 8; k++) {
        const uint32_t nibble = (value >> (28 - 4 * k)) & 0xF;
        text[2 + k] = static_cast<char>(nibble < 10 ? '0' + nibble : 'a' + nibble - 10);
    }
    text[10] = '\0';
    print(text);
}

}  // namespace ps2::console
