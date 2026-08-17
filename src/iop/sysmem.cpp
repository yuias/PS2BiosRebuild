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

// Provisional extent. The reference takes the machine's memory size from the
// boot parameters the reset path latched (spec/03 BOOT-4 step 5); until that
// is plumbed through, a retail 2 MiB machine's spare middle is the heap, kept
// clear of the stack the boot block put at the top.
constexpr uint32_t kHeapStart = 0x00100000;
constexpr uint32_t kHeapEnd = 0x001F0000;
constexpr uint32_t kAlignment = 16;

constexpr uint32_t kExportMagic = 0x41C00000;

// The bump allocator's state: the next free address, and the most recent
// allocation -- the only one `release` can give back.
uint32_t heap_cursor;
uint32_t heap_last;

// IRX-7: slots 0 and 1 are reserved hooks that nothing imports, kept occupied
// rather than removed (IRX-6a), so they need somewhere to point.
[[nodiscard]] int reservedHook(uint32_t) {
    return 0;
}

[[nodiscard]] int unimplemented(uint32_t) {
    return -1;
}

// Ordinal 4: allocate(size) -> address, or 0.
//
// A bump allocator, which is all the boot needs: every allocation the module
// list makes lives as long as the machine does. Anything with a real lifetime
// arrives with the heap of `HEAPLIB`.
[[nodiscard]] int allocate(uint32_t size) {
    if (size == 0) {
        return 0;
    }
    const uint32_t rounded = (size + kAlignment - 1) & ~(kAlignment - 1);
    const uint32_t next = heap_cursor + rounded;
    if (next > kHeapEnd) {
        return 0;                      // past the end of the heap
    }
    const uint32_t address = heap_cursor;
    heap_last = heap_cursor;
    heap_cursor = next;
    return static_cast<int>(address);
}

// Ordinal 5: release(address) -> 0, or -1.
//
// A bump allocator can give back only what it handed out last. Saying so is
// better than accepting every address and leaking: a caller that frees out of
// order finds out at once rather than exhausting the heap later.
[[nodiscard]] int deallocate(uint32_t address) {
    if (heap_last == 0 || address != heap_last) {
        return -1;
    }
    heap_cursor = heap_last;
    heap_last = 0;
    return 0;
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
    int (*entries[9])(uint32_t);       // eight ordinals plus the IRX-5b terminator
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
        allocate,                       // 4  allocate
        deallocate,                     // 5  release
        unimplemented,                  // 6
        unimplemented,                  // 7
        nullptr,                        // IRX-5b: the terminator
    },
};

}  // namespace

extern "C" {

// IRX-12: entry(argc, argv, 0, module_record), with the module's own $gp
// installed. `return & 3` decides residency -- clear keeps the module -- and
// a memory manager stays.
int _module_start(int, char **) {
    heap_cursor = kHeapStart;
    heap_last = 0;
    return 0;                          // resident
}

}  // extern "C"
