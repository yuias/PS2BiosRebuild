// The IOP's saved context, as INTRMAN pushes it and a thread manager primes it.
//
// docs/spec/06-iop-kernel.md IOP-2e and IOP-3h: an exception saves the
// interrupted registers, the hooks THREADMAN installs may answer with a
// different thread's frame, and the exception's return restores from
// whichever frame it is handed. **The shape is the reference's, not ours to
// choose**: a merged kernel runs this INTRMAN beside a THREADMAN out of a
// title's own image, and that THREADMAN primes frames at these offsets and
// reads them back at them. Read out of `INTRMANI` 1.01's dispatcher and
// confirmed against `THREADMAN` 2.03's thread-start path.
//
// The frame is 0x98 bytes on the stack of the thread that was interrupted,
// so a thread's frame goes wherever its stack does:
//
//     +0x00          the save-state tag, below
//     +0x04..+0x7c   $1..$31
//     +0x80, +0x84   hi, lo
//     +0x88          Status
//     +0x8c          EPC
//     +0x90          the I_CTRL gate the exception found
//     +0x94          unused; the reference allocates 0x98 and writes 0x94
//
// **Word 0 is not `$0`.** It is how the reference defers saving registers a
// handler will not touch: `0xAC0000FE` means only $1..$7, hi, lo, Status and
// EPC are present, `0xFF00FFFE` adds $8..$15, $24, $25, $gp and $fp, and
// `0xFFFFFFFE` adds $16..$23 and so describes a complete frame. The
// reference's restore reads the tag and skips the groups a frame does not
// carry. This module saves everything always, so every frame it builds is
// tagged `kTagFull` and the restore has nothing to branch on -- and the only
// other frames on this image come from `THREADMAN` 2.03's thread-start path,
// which writes the same tag.
//
// `status` is `Status` as the exception left it -- with the pre-exception
// interrupt enable in IEp, one level down -- because the return is an `rfe`,
// which pops that level back into IEc. A frame made for a thread that has
// never run therefore puts the interrupt state it should start under in IEp.

#pragma once

#include <stdint.h>

namespace ps2::context {

inline constexpr uint32_t kWords = 38;
inline constexpr uint32_t kBytes = kWords * 4;   // 0x98, the reference's size

inline constexpr uint32_t kTag = 0;              // *not* $0; see above
inline constexpr uint32_t kRegisters = 0;        // $1..$31 at +0x04..+0x7c
inline constexpr uint32_t kHi = 32;
inline constexpr uint32_t kLo = 33;
inline constexpr uint32_t kStatus = 34;
inline constexpr uint32_t kEpc = 35;
inline constexpr uint32_t kIntCtrl = 36;         // the I_CTRL gate, IOP-2e

// A frame carrying every register. The reference's two partial tags are
// never written here, so nothing reads this back.
inline constexpr uint32_t kTagFull = 0xFFFFFFFEu;

// Valid for $1..$31; slot 0 belongs to the tag.
[[nodiscard]] inline constexpr uint32_t slotOf(uint32_t reg) {
    return kRegisters + reg;
}

}  // namespace ps2::context
