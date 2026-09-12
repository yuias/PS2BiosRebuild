// SYSMEM: the IOP's memory manager -- the first module the boot list names.
//
// The container is docs/spec/02-module-abi.md IRX-1, the library table IRX-4,
// the ordinals IRX-6 and the entry convention IRX-12. Ordinals 4 and 5 are the
// allocator and its deallocator, which `docs/analysis/05-sysmem-and-loadcore.md`
// identified from how every other module calls them.
//
// The export table's function pointers each need an R_MIPS_32 fixup (IRX-3);
// a plain struct of function pointers gets that from the linker for free, so
// nothing here has to be written by hand except the table's own binary
// layout (IRX-4/IRX-5).

#include <stddef.h>
#include <stdint.h>

namespace {

// IRX-15e: the heap is what is left of the machine above the loaded kernel.
// The reference's low bound is the end of SYSMEM's own image, because every
// module after it is allocated out of this heap rather than placed; ours are
// placed by `IOPBOOT` from the boot list's base upwards, so the bound has to
// clear the whole boot image instead. `tools/imgcheck.py` gates the image
// against it. The high bound stops below the boot list at `0x001F8100` and
// the stack the boot block put above that; the reference's is the RAM size
// the reset path latched (spec/03 BOOT-4 step 5), which the entry receives
// as `ram_size` but this constant still caps.
constexpr uint32_t kHeapStart = 0x00020000;
// BOOT-4: what a retail machine latches, for an entry that was told nothing.
constexpr uint32_t kDefaultRamSize = 0x00200000;
constexpr uint32_t kHeapEnd = 0x001F0000;

// IRX-15a: the reference allocates in units of 0x100, which is also the
// boundary IRX-12b rounds a released module base down to.
constexpr uint32_t kAlignment = 0x100;

// IRX-15b: the three modes.
enum Mode : uint32_t { Lowest = 0, Highest = 1, AtAddress = 2 };

constexpr uint32_t kExportMagic = 0x41C00000;

// The heap the entry settled on (IRX-15e). `heap_low` is zero until the entry
// runs, which is what every ordinal tests for "initialised".
uint32_t heap_low;
uint32_t heap_high;
// What the entry was told the machine has, which ordinal 6 answers.
uint32_t ram_size;

// One record per block handed out, kept sorted by address. The reference
// chains nodes for the free ranges as well and refills the chain from fresh
// chunks; recording only what is in use makes the free ranges the gaps
// between records, which are maximal by construction -- so no coalescing step
// is needed and a release can never leave two adjacent free nodes unmerged.
//
// The capacity is the one deviation: the reference's chain grows, this array
// does not, and an allocation past the last record is refused like any other
// allocation that does not fit (IRX-15d). The boot leaves about forty blocks
// live and a title's own modules roughly double that.
constexpr uint32_t kMaxBlocks = 256;

struct Block {
    uint32_t start;
    uint32_t size;
};

Block blocks[kMaxBlocks];
uint32_t block_count;

// The free range containing `address`, as [start, end). Only meaningful when
// no record contains the address; the caller checks that first.
struct Gap {
    uint32_t start;
    uint32_t end;
};

// The index of the first record starting at or after `address`.
[[nodiscard]] uint32_t lowerBound(uint32_t address) {
    uint32_t i = 0;
    while (i < block_count && blocks[i].start < address) {
        ++i;
    }
    return i;
}

[[nodiscard]] const Block *blockContaining(uint32_t address) {
    for (uint32_t i = 0; i < block_count; ++i) {
        if (address >= blocks[i].start
            && address - blocks[i].start < blocks[i].size) {
            return &blocks[i];
        }
        if (blocks[i].start > address) {
            break;                     // sorted: nothing further can contain it
        }
    }
    return nullptr;
}

// The gap `index` opens: from the end of the record before it to the start of
// the record at it, clipped to the heap at both ends.
[[nodiscard]] Gap gapBefore(uint32_t index) {
    const uint32_t start = index == 0
        ? heap_low : blocks[index - 1].start + blocks[index - 1].size;
    const uint32_t end = index == block_count ? heap_high : blocks[index].start;
    return {start, end};
}

[[nodiscard]] bool insertBlock(uint32_t index, uint32_t start, uint32_t size) {
    if (block_count == kMaxBlocks) {
        return false;
    }
    for (uint32_t i = block_count; i > index; --i) {
        blocks[i] = blocks[i - 1];
    }
    blocks[index] = {start, size};
    ++block_count;
    return true;
}

// IRX-7: slots 0 and 1 are reserved hooks that nothing imports, kept occupied
// rather than removed (IRX-6a), so they need somewhere to point.
[[nodiscard]] int reservedHook(uint32_t) {
    return 0;
}

[[nodiscard]] int unimplemented(uint32_t) {
    return -1;
}

// Ordinals 14 and 15 (IRX-6c). `Kprintf` is not a printer: it forwards to a
// hook ordinal 15 installs, and answers 0 while there is none -- which is
// the state the reference ships in, since nothing in its archive installs
// one. Nearly every module imports 14, so the table has to reach it: an
// ordinal past the terminator binds to `jr $ra` and the call disappears.
using KprintfHook = int (*)(void *context, const char *format,
                            __builtin_va_list args);

KprintfHook kprintf_hook;
void *kprintf_context;

int kprintf(const char *format, ...) {
    if (kprintf_hook == nullptr) {
        return 0;
    }
    __builtin_va_list args;
    __builtin_va_start(args, format);
    const int answer = kprintf_hook(kprintf_context, format, args);
    __builtin_va_end(args);
    return answer;
}

int kprintfSet(KprintfHook hook, void *context) {
    kprintf_hook = hook;
    kprintf_context = context;
    return 0;
}

// Ordinal 4: allocate(mode, size, address) -> address, or 0.
//
// IRX-15b's three modes over the gaps between the records: mode 0 takes the
// lowest gap the request fits in and carves its bottom, mode 1 the highest and
// carves its top, mode 2 the gap the caller named. That the low and high modes
// carve opposite ends is IRX-15c, and it is what makes `MODLOAD`'s release of
// a raw file actually give the memory back -- the file comes off the top, the
// image it builds off the bottom.
[[nodiscard]] int allocate(uint32_t mode, uint32_t size, uint32_t address) {
    if (size == 0 || heap_low == 0) {
        return 0;
    }
    const uint32_t rounded = (size + kAlignment - 1) & ~(kAlignment - 1);

    if (mode == AtAddress) {
        // The reference refuses an unaligned address outright rather than
        // rounding it, and refuses a range that is not wholly free.
        if ((address & (kAlignment - 1)) != 0 || address < heap_low
            || rounded > heap_high - address) {
            return 0;
        }
        const uint32_t index = lowerBound(address);
        const Gap gap = gapBefore(index);
        if (address < gap.start || rounded > gap.end - address) {
            return 0;
        }
        return insertBlock(index, address, rounded)
            ? static_cast<int>(address) : 0;
    }

    if (mode == Lowest) {
        for (uint32_t i = 0; i <= block_count; ++i) {
            const Gap gap = gapBefore(i);
            if (gap.end - gap.start >= rounded) {
                return insertBlock(i, gap.start, rounded)
                    ? static_cast<int>(gap.start) : 0;
            }
        }
        return 0;                      // IRX-15d: no fallback to the other end
    }

    if (mode == Highest) {
        for (uint32_t i = block_count + 1; i > 0; --i) {
            const Gap gap = gapBefore(i - 1);
            if (gap.end - gap.start >= rounded) {
                const uint32_t block = gap.end - rounded;
                return insertBlock(i - 1, block, rounded)
                    ? static_cast<int>(block) : 0;
            }
        }
        return 0;
    }

    return 0;                          // IRX-15b: no fourth mode
}

// Ordinal 6: memSize() -> the machine's RAM in bytes, as the entry was told
// it (IRX-15). `UDNL` sizes the boot block's RAM-in-MiB word from it when it
// stages the next kernel, so it has to answer the same number the boot block
// started from rather than the top of this module's own heap.
[[nodiscard]] int memSize() {
    return static_cast<int>(ram_size);
}

// Ordinals 9 and 10 answer for the range that *contains* an address, in use
// or not, and mark a free one by setting bit 31 of the answer -- 9 returning
// the range's start and 10 its size. `HEAPLIB` is the caller that matters: it
// queries a chunk in the instruction after allocating it, to find the
// rounding slack it may use, so it always asks about a block that is in use.
constexpr uint32_t kFreeMark = 0x80000000;

// The gap containing `address`, or a zero-length one when the address is
// outside the heap entirely.
[[nodiscard]] Gap gapContaining(uint32_t address) {
    const uint32_t index = lowerBound(address);
    const Gap gap = gapBefore(index);
    return address >= gap.start && address < gap.end ? gap : Gap{0, 0};
}

// Ordinal 9: blockAddress(address) -> the start of the range containing it.
[[nodiscard]] int blockAddress(uint32_t address) {
    if (const Block *block = blockContaining(address); block != nullptr) {
        return static_cast<int>(block->start);
    }
    const Gap gap = gapContaining(address);
    if (gap.end == 0) {
        return -1;
    }
    return static_cast<int>(gap.start | kFreeMark);
}

// Ordinal 10: blockSize(address) -> the size in bytes of that same range.
[[nodiscard]] int blockSize(uint32_t address) {
    if (const Block *block = blockContaining(address); block != nullptr) {
        return static_cast<int>(block->size);
    }
    const Gap gap = gapContaining(address);
    if (gap.end == 0) {
        return -1;
    }
    return static_cast<int>((gap.end - gap.start) | kFreeMark);
}

// Ordinal 5: release(address) -> 0, or -1.
//
// The address must be the block's own start, aligned: the reference matches a
// record's start exactly rather than looking for the range the address falls
// in, so a pointer into the middle of a block is an error and not a release
// of the block around it.
[[nodiscard]] int deallocate(uint32_t address) {
    if ((address & (kAlignment - 1)) != 0) {
        return -1;
    }
    for (uint32_t i = 0; i < block_count; ++i) {
        if (blocks[i].start != address) {
            continue;
        }
        for (uint32_t j = i; j + 1 < block_count; ++j) {
            blocks[j] = blocks[j + 1];
        }
        --block_count;
        return 0;
    }
    return -1;
}

// IRX-4: the export table header, followed by IRX-5's pointer array. It is a
// mutable struct rather than `const` because registration writes the
// registry link over the first word (IRX-4c) -- a table in read-only memory
// could not be registered at all.
struct ExportTable {
    uint32_t magic;
    uint32_t clients;
    uint16_t version;
    uint16_t flags;
    char tag[8];
    int (*entries[17])(uint32_t);      // sixteen ordinals plus IRX-5b's terminator
};
static_assert(offsetof(ExportTable, entries) == 0x14);

// `used`: nothing in this translation unit takes its address -- the loader
// finds it by scanning for the magic word (IRX-4b), not through a reference
// -- so an optimiser that only sees the call graph would otherwise drop it.
[[gnu::used, gnu::section(".iopexport")]] ExportTable sysmem_exports = {
    kExportMagic,
    0,                                  // head of the bound-client list
    0x0101,                             // version 1.01, BCD
    0,                                  // flags
    {'s', 'y', 's', 'm', 'e', 'm', '\0', '\0'},   // IRX-4a: eight bytes, NUL-padded
    {
        reservedHook,                   // 0  reserved
        reservedHook,                   // 1  reserved
        reservedHook,                   // 2  reserved (IRX-6a)
        unimplemented,                  // 3
        reinterpret_cast<int (*)(uint32_t)>(allocate),   // 4  allocate(mode, size, address)
        deallocate,                     // 5  release
        reinterpret_cast<int (*)(uint32_t)>(memSize),      // 6  memory size
        unimplemented,                  // 7
        unimplemented,                  // 8
        blockAddress,                   // 9  block start
        blockSize,                      // 10 block size
        reservedHook,                   // 11 reserved (IRX-6a)
        reservedHook,                   // 12 reserved
        reservedHook,                   // 13 reserved
        reinterpret_cast<int (*)(uint32_t)>(kprintf),      // 14 Kprintf
        reinterpret_cast<int (*)(uint32_t)>(kprintfSet),   // 15 its hook
        nullptr,                        // IRX-5b: the terminator
    },
};

}  // namespace

extern "C" {

// IRX-12: entry(argc, argv, 0, module_record), with the module's own $gp
// installed. `return & 3` decides residency -- clear keeps the module -- and
// a memory manager stays.
// BOOT-8c: the entry takes the RAM size **in bytes** and answers the address
// the boot loader should place the next module at -- the reference computes
// both from its own image, putting its heap immediately above itself and
// answering the base of that heap's first free block.
//
// Ours cannot answer, and says so with `0`: its heap is a fixed window
// (IRX-15e's deviation, `docs/implementation.md`), not the space above its
// own image, so the address it would name is not a placement address at all.
// A `0` tells the boot loader to keep its own arithmetic. The RAM size is
// honoured: a machine with less than this window would otherwise have a heap
// running off the end of it.
int _module_start(int ram_size_byte, char **) {
    const auto ram_top = static_cast<uint32_t>(ram_size_byte) & ~uint32_t{0xFF};
    ram_size = ram_top != 0 ? ram_top : kDefaultRamSize;
    heap_high = ram_top != 0 && ram_top < kHeapEnd ? ram_top : kHeapEnd;
    // IRX-15e: a heap narrower than one unit leaves the module uninitialised,
    // which every ordinal reads off `heap_low` being zero.
    heap_low = heap_high >= kHeapStart + kAlignment ? kHeapStart : 0;
    block_count = 0;
    return 0;                          // resident, and no placement to offer
}

}  // extern "C"
