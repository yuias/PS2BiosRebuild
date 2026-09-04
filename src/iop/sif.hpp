// What the two SIF libraries share: the hardware registers the bus meets on,
// and the transfer descriptor `sceSifSetDma` takes.
//
// docs/spec/03-boot-chain.md BOOT-10 and BOOT-11. `SIFMAN` owns the DMA and
// the flag registers; `SIFCMD` owns the command buffer whose address SMCOM
// publishes, which is why the register addresses live here rather than in
// either module.

#pragma once

#include <stdint.h>

namespace ps2::sif {

inline constexpr uintptr_t kMscom = 0xBD000000;
inline constexpr uintptr_t kSmcom = 0xBD000010;
inline constexpr uintptr_t kMsflg = 0xBD000020;
inline constexpr uintptr_t kSmflg = 0xBD000030;
inline constexpr uintptr_t kCtrl = 0xBD000040;

inline constexpr uint32_t kFlagSifInit = 0x00010000;  // BOOT-10: SIF_STAT_SIFINIT
inline constexpr uint32_t kFlagCmdInit = 0x00020000;  // BOOT-12b: SIF_STAT_CMDINIT
inline constexpr uint32_t kFlagBootEnd = 0x00040000;  // SIF_STAT_BOOTEND [header]

// `sceSifSetDma`'s descriptor [header]: IOP memory to send, where in EE
// memory it lands, and the attributes of the run.
struct Transfer {
    uint32_t src;
    uint32_t dest;
    uint32_t size;
    uint32_t attr;
};

// The one attribute this project acts on: the last block of a run carries the
// tag bit that stops -- and interrupts -- the EE's receiving channel. A run
// without it leaves that channel armed, so a later run can end it, which is
// what an RPC server's data-then-packet pair depends on (BOOT-12d).
inline constexpr uint32_t kAttrEndEe = 0x4;           // SIF_DMA_INT_O [header]

}  // namespace ps2::sif
