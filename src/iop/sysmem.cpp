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
// the reset path latched (spec/03 BOOT-4 step 5), still not plumbed through.
constexpr uint32_t kHeapStart = 0x00020000;
constexpr uint32_t kHeapEnd = 0x001F0000;

// IRX-15a: the reference allocates in units of 0x100, which is also the
// boundary IRX-12b rounds a released module base down to.
constexpr uint32_t kAlignment = 0x100;

// IRX-15b: the three modes.
enum Mode : uint32_t { Lowest = 0, Highest = 1, AtAddress = 2 };

constexpr uint32_t kExportMagic = 0x41C00000;

// IRX-15c: two cursors growing towards each other, so mode 0 and mode 1 never
// interleave. `low_cursor` is zero until the entry runs, which is what every
// ordinal tests for "initialised" (IRX-15e).
uint32_t low_cursor;
uint32_t high_cursor;
// The most recent block at each end -- the only ones `release` can give back.
uint32_t low_last;
uint32_t high_last;
uint32_t high_last_end;

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
// Not a free list: each end of the heap is a bump cursor, and the two grow
// towards each other. That is enough to honour IRX-15b's low and high modes,
// which is what makes `MODLOAD`'s release of a raw file actually give the
// memory back -- the file comes off the top, the image it builds off the
// bottom, so the file is still the topmost block when it is released. Mode 2
// (`AtAddress`) has no caller here and is refused rather than misplaced.
// Anything needing a real lifetime arrives with the heap of `HEAPLIB`.
[[nodiscard]] int allocate(uint32_t mode, uint32_t size,
                           [[maybe_unused]] uint32_t address) {
    if (size == 0 || low_cursor == 0) {
        return 0;
    }
    const uint32_t rounded = (size + kAlignment - 1) & ~(kAlignment - 1);
    if (rounded > high_cursor - low_cursor) {
        return 0;                      // IRX-15d: no fallback to the other end
    }
    if (mode == Lowest) {
        const uint32_t block = low_cursor;
        low_last = block;
        low_cursor += rounded;
        return static_cast<int>(block);
    }
    if (mode == Highest) {
        high_last_end = high_cursor;
        high_cursor -= rounded;
        high_last = high_cursor;
        return static_cast<int>(high_cursor);
    }
    return 0;
}

// Ordinal 5: release(address) -> 0, or -1.
//
// A pair of bump cursors can give back only what each end handed out last.
// Saying so is better than accepting every address and leaking: a caller that
// frees out of order finds out at once rather than exhausting the heap later.
[[nodiscard]] int deallocate(uint32_t address) {
    if (low_last != 0 && address == low_last) {
        low_cursor = low_last;
        low_last = 0;
        return 0;
    }
    if (high_last != 0 && address == high_last) {
        high_cursor = high_last_end;
        high_last = 0;
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
[[gnu::used]] ExportTable sysmem_exports = {
    kExportMagic,
    0,                                  // head of the bound-client list
    0x0101,                             // version 1.01, BCD
    0,                                  // flags
    {'s', 'y', 's', 'm', 'e', 'm', '\0', '\0'},   // IRX-4a: eight bytes, NUL-padded
    {
        reservedHook,                   // 0  reserved
        reservedHook,                   // 1  reserved
        unimplemented,                  // 2
        unimplemented,                  // 3
        reinterpret_cast<int (*)(uint32_t)>(allocate),   // 4  allocate(mode, size, address)
        deallocate,                     // 5  release
        unimplemented,                  // 6
        unimplemented,                  // 7
        unimplemented,                  // 8
        unimplemented,                  // 9
        unimplemented,                  // 10
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
int _module_start(int, char **) {
    low_cursor = kHeapStart;
    high_cursor = kHeapEnd;
    low_last = 0;
    high_last = 0;
    return 0;                          // resident
}

}  // extern "C"
