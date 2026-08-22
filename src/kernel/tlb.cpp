// The kernel's TLB: spec/04 EE-12.
//
// docs/spec/04-ee-kernel.md EE-12, from docs/analysis/36. The reset path
// maps the scratchpad (EE-1a) and nothing else; everything a program reaches
// through KUSEG -- main memory cached, uncached and uncached-accelerated, the
// hardware registers -- is mapped by the kernel, from a fixed table, before
// any program runs. An emulator that walks the TLB answers an unmapped access
// with nothing, which is how the table's absence was found: the SDK's command
// handler reads its packet through the uncached window and saw only zeroes.

#include <stdint.h>

namespace {

constexpr uint32_t kEntries = 48;                // the R5900's TLB
constexpr uint32_t kWired = 31;                  // entries below it are fixed

struct Entry {
    uint32_t page_mask;
    uint32_t entry_hi;
    uint32_t entry_lo0;
    uint32_t entry_lo1;
};

// EE-12a: the table, by index. Entry 0 repeats EE-1a's scratchpad mapping;
// 1-12 are the kernel's own, 13-21 main memory cached, 22-30 the same
// uncached, 31-38 uncached and accelerated. EntryLo's low bits are
// `C << 3 | D << 2 | V << 1 | G`: 0x1F cached, 0x17 uncached, 0x3F
// accelerated, 0x13 uncached and write-protected -- 0x10001000, and the page
// the SIF channels' registers are on, which a program operates through
// syscalls -- and 0x15 an odd page that is not valid.
constexpr Entry kTable[] = {
    {0x00000000, 0x70000000, 0x80000007, 0x00000007},   // 0: scratchpad
    {0x00006000, 0xFFFF8000, 0x00001E1F, 0x00001F1F},   // 1: 0x78000, 32 KiB
    {0x00000000, 0x10000000, 0x00400017, 0x00400053},   // 2-9: hardware
    {0x00000000, 0x10002000, 0x00400097, 0x004000D7},
    {0x00000000, 0x10004000, 0x00400117, 0x00400157},
    {0x00000000, 0x10006000, 0x00400197, 0x004001D7},
    {0x00000000, 0x10008000, 0x00400217, 0x00400257},
    {0x00000000, 0x1000A000, 0x00400297, 0x004002D7},
    {0x00000000, 0x1000C000, 0x00400313, 0x00400357},
    {0x00000000, 0x1000E000, 0x00400397, 0x004003D7},
    {0x0001E000, 0x11000000, 0x00440017, 0x00440415},   // 10: VU memory
    {0x0001E000, 0x12000000, 0x00480017, 0x00480415},   // 11: GS registers
    {0x01FFE000, 0x1E000000, 0x00780017, 0x007C0017},   // 12: the ROM
    {0x0007E000, 0x00080000, 0x0000201F, 0x0000301F},   // 13-21: RAM, cached
    {0x0007E000, 0x00100000, 0x0000401F, 0x0000501F},
    {0x0007E000, 0x00180000, 0x0000601F, 0x0000701F},
    {0x001FE000, 0x00200000, 0x0000801F, 0x0000C01F},
    {0x001FE000, 0x00400000, 0x0001001F, 0x0001401F},
    {0x001FE000, 0x00600000, 0x0001801F, 0x0001C01F},
    {0x007FE000, 0x00800000, 0x0002001F, 0x0003001F},
    {0x007FE000, 0x01000000, 0x0004001F, 0x0005001F},
    {0x007FE000, 0x01800000, 0x0006001F, 0x0007001F},
    {0x0007E000, 0x20080000, 0x00002017, 0x00003017},   // 22-30: RAM, uncached
    {0x0007E000, 0x20100000, 0x00004017, 0x00005017},
    {0x0007E000, 0x20180000, 0x00006017, 0x00007017},
    {0x001FE000, 0x20200000, 0x00008017, 0x0000C017},
    {0x001FE000, 0x20400000, 0x00010017, 0x00014017},
    {0x001FE000, 0x20600000, 0x00018017, 0x0001C017},
    {0x007FE000, 0x20800000, 0x00020017, 0x00030017},
    {0x007FE000, 0x21000000, 0x00040017, 0x00050017},
    {0x007FE000, 0x21800000, 0x00060017, 0x00070017},
    {0x0007E000, 0x30100000, 0x0000403F, 0x0000503F},   // 31-38: accelerated
    {0x0007E000, 0x30180000, 0x0000603F, 0x0000703F},
    {0x001FE000, 0x30200000, 0x0000803F, 0x0000C03F},
    {0x001FE000, 0x30400000, 0x0001003F, 0x0001403F},
    {0x001FE000, 0x30600000, 0x0001803F, 0x0001C03F},
    {0x007FE000, 0x30800000, 0x0002003F, 0x0003003F},
    {0x007FE000, 0x31000000, 0x0004003F, 0x0005003F},
    {0x007FE000, 0x31800000, 0x0006003F, 0x0007003F},
};
constexpr uint32_t kTableEntries = sizeof(kTable) / sizeof(kTable[0]);
static_assert(kTableEntries == 39);

// `sync.p` is a `.word` because LLVM has no R5900 target to assemble it with
// (syscall.S says the same); it separates the CP0 writes from the `tlbwi`
// that reads them.
void writeEntry(uint32_t index, const Entry &entry) {
    asm volatile(
        "mtc0 %0, $0\n\t"           // Index
        "mtc0 %1, $5\n\t"           // PageMask
        "mtc0 %2, $10\n\t"          // EntryHi
        "mtc0 %3, $2\n\t"           // EntryLo0
        "mtc0 %4, $3\n\t"           // EntryLo1
        ".word 0x0000040F\n\t"
        "tlbwi\n\t"
        ".word 0x0000040F"
        :
        : "r"(index), "r"(entry.page_mask), "r"(entry.entry_hi),
          "r"(entry.entry_lo0), "r"(entry.entry_lo1)
        : "memory");
}

}  // namespace

extern "C" {

// EE-12b: every entry is first made invalid under a virtual address nothing
// uses -- the reference does this to all 48, the reset's scratchpad entry
// included, and then writes the table over it -- and `Wired` is left so
// that the entries a program may add (SYS-7b) land above the fixed ones.
void installTlb() {
    for (uint32_t index = 0; index < kEntries; index++) {
        const Entry invalid = {0, 0xE0000000 + index * 0x2000, 0, 0};
        writeEntry(index, invalid);
    }
    for (uint32_t index = 0; index < kTableEntries; index++) {
        writeEntry(index, kTable[index]);
    }
    asm volatile("mtc0 %0, $6\n\t.word 0x0000040F" : : "r"(kWired) : "memory");
}

}  // extern "C"
