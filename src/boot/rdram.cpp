// Memory-controller bring-up, reached by the EE reset path (spec/04 EE-2).
//
// On hardware this negotiates with the RDRAM controller, counts devices and
// reports the size it found. An emulator presents its RAM already, so there is
// nothing to negotiate: the file exists because the reset path calls it, and
// the call is part of the specified sequence.
//
// EE-2b: what it returns is the size of main memory, which the reset path
// passes to the kernel and the kernel keeps as the top of memory; a negative
// return would mean failure. It is called with the scratchpad for a stack and
// from wherever the archive put it, so it can rely on nothing but registers --
// a constant needs nothing else.

#include <stdint.h>

namespace {
constexpr int32_t kMainMemory = 32 * 1024 * 1024;
}

extern "C" {
int32_t rdram() asm("_rdram");
int32_t rdram() {
    return kMainMemory;
}
}
