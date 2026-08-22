// The IOP's saved context, as INTRMAN pushes it and THREADMAN primes it.
//
// docs/spec/06-iop-kernel.md IOP-2e and IOP-3h: an exception saves the
// interrupted registers, the hooks THREADMAN installs may answer with a
// different thread's frame, and the exception's return restores from
// whichever frame it is handed. Both modules are ours, so the frame's shape
// is an interface between them rather than the reference's (whose own
// layout docs/analysis/38 leaves only partly read); it is a plain array of
// words, pushed on the stack of the thread that was interrupted, so that a
// thread's frame goes wherever its stack does.
//
// `status` is `Status` as the exception left it -- with the pre-exception
// interrupt enable in IEp, one level down -- because the return is an `rfe`,
// which pops that level back into IEc. A frame made for a thread that has
// never run therefore puts the interrupt state it should start under in IEp.

#pragma once

#include <stdint.h>

namespace ps2::context {

inline constexpr uint32_t kWords = 36;
inline constexpr uint32_t kBytes = kWords * 4;   // 0x90, a multiple of 16

inline constexpr uint32_t kEpc = 0;
inline constexpr uint32_t kStatus = 1;
inline constexpr uint32_t kHi = 2;
inline constexpr uint32_t kLo = 3;
inline constexpr uint32_t kRegisters = 4;        // $0..$31 follow, in order

[[nodiscard]] inline constexpr uint32_t slotOf(uint32_t reg) {
    return kRegisters + reg;
}

}  // namespace ps2::context
