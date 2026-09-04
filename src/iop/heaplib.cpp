// HEAPLIB: a K&R-style arena allocator over blocks taken from SYSMEM, with no
// state of its own -- every heap lives entirely in the caller's SYSMEM memory.
//
// docs/spec/06-iop-kernel.md IOP-12, from docs/analysis/50. SYSMEM hands out
// 0x100-granular blocks and only ever takes back the last one at each end
// (src/iop/sysmem.cpp), so nothing built directly on it can allocate and free
// in an arbitrary order; this module supplies that. A heap is a small header
// (IOP-12c) followed by its first arena; an arena is K&R `malloc` in a fixed
// region, in 8-byte units (IOP-12d). `Block` plays the role of K&R's `Header`
// union: it is exactly one unit, and pointer arithmetic on `Block *` already
// steps by units, which is what lets the allocator and the free list read as
// the textbook algorithm rather than a byte-offset rewrite of it.

#include "module.hpp"

#include <stdint.h>

namespace {

using ps2::module::ExportTable;
using ps2::module::reservedHook;
using ps2::module::slot;

// IOP-12j: allocation failure is 0 everywhere (a null Heap*/void*, or a 0 from
// HeapAlloc). The codes below are only for the ordinals that answer int.
constexpr int kBadHeap = -4;     // IOP-12b: heap magic didn't match
constexpr int kBadArena = -4;    // IOP-12b: arena magic didn't match
constexpr int kNotOwned = -1;    // IOP-12d/12g: the ownership tag test failed
// Not named by the spec text (IOP-12g only describes three of HeapFree's five
// codes in prose); the pointer falling outside the arena's own memory is the
// one gap left for -2, checked before the header at `ptr - 8` is ever read.
constexpr int kOutOfRange = -2;
constexpr int kCorrupted = -3;   // IOP-12g: the three address-bracket checks

// IOP-12i: SYSMEM ordinal 4's mode argument -- 1 (highest) when CreateHeap's
// `flags` bit 1 asked for it, 0 (lowest) otherwise. Also, packed into bit 0
// of a heap's own +0x4 word (IOP-12c), it is what a growing chunk reuses.
constexpr uint32_t kSysmemHighest = 1;
constexpr uint32_t kSysmemLowest = 0;

// IOP-12i: the growth path's floor -- the chunk's own overhead. 8 for the
// list node, 0x10 for the arena header, 8 for the block header, 8 for the
// sentinel.
constexpr uint32_t kChunkOverhead = 0x28;

extern "C" {
int _import_loadcore_register(void *table);
int _import_sysmem_allocate(uint32_t mode, uint32_t size, uint32_t address);
int _import_sysmem_release(uint32_t address);
int _import_sysmem_block_size(uint32_t address);
}

// IOP-12d: one unit, `{next, size_in_units}`. While a block is free, `next`
// links the address-ordered circular free list; while it is allocated, `next`
// holds the arena's own address -- the ownership tag HeapFree tests. `size`
// counts the header, so the user pointer is `this + 1`.
struct Block {
    Block *next;
    uint32_t size;
};
static_assert(sizeof(Block) == 8);

// IOP-12d: `+0x0` magic (`arena - 1`, IOP-12b), `+0x4` size in bytes as given,
// `+0x8` units allocated, `+0xc` the rover. The first block header follows at
// `+0x10`, i.e. `arena + 1`.
struct Arena {
    int32_t magic;
    uint32_t size;
    uint32_t used;
    Block *rover;
};
static_assert(sizeof(Arena) == 0x10);

// IOP-12c: the `{next, previous}` sentinel of the grown-chunk list, and (for a
// grown chunk taken from SYSMEM) the first 8 bytes of the chunk itself -- its
// arena begins right after, at `this + 1`.
struct ChunkNode {
    ChunkNode *next;
    ChunkNode *previous;
};
static_assert(sizeof(ChunkNode) == 8);

// IOP-12c: `+0x0` magic (`heap + 1`), `+0x4` the growth size in the upper
// bits with the SYSMEM mode in bit 0, `+0x8`/`+0xc` the sentinel. Arena 0
// follows at `+0x10`, i.e. `heap + 1`.
struct Heap {
    int32_t magic;
    uint32_t growth_and_mode;
    ChunkNode sentinel;
};
static_assert(sizeof(Heap) == 0x10);

[[nodiscard]] int32_t heapMagicFor(const void *heap) {
    return static_cast<int32_t>(reinterpret_cast<uintptr_t>(heap)) + 1;
}

[[nodiscard]] int32_t arenaMagicFor(const void *arena) {
    return static_cast<int32_t>(reinterpret_cast<uintptr_t>(arena)) - 1;
}

[[nodiscard]] bool isValidHeap(const Heap *heap) {
    return heap != nullptr && heap->magic == heapMagicFor(heap);
}

[[nodiscard]] bool isValidArena(const Arena *arena) {
    return arena != nullptr && arena->magic == arenaMagicFor(arena);
}

[[nodiscard]] Arena *arenaOf(ChunkNode *node) {
    return reinterpret_cast<Arena *>(node + 1);
}

[[nodiscard]] Block *firstBlockOf(Arena *arena) {
    return reinterpret_cast<Block *>(arena + 1);
}

// --- ordinals 11-15: the arena primitives (IOP-12d/e/f/g/h) -----------------

// Ordinal 11. IOP-12e: a `size` below 0x29 leaves the memory untouched and
// reports nothing -- the magic never appears, so every later call on this
// arena fails on IOP-12b's check. That is the behaviour, not an oversight.
void heapPrepare(void *memory, int32_t size) {
    if (memory == nullptr || size < 0x29) {
        return;
    }
    Arena *arena = static_cast<Arena *>(memory);
    arena->magic = arenaMagicFor(arena);
    arena->size = static_cast<uint32_t>(size);
    arena->used = 0;
    const uint32_t units = (static_cast<uint32_t>(size) - 0x10) >> 3;
    Block *first = firstBlockOf(arena);
    Block *sentinel = first + (units - 1);
    first->size = units - 1;
    first->next = sentinel;
    sentinel->size = 0;             // IOP-12e: size zero satisfies no request
    sentinel->next = first;
    arena->rover = first;
}

// Ordinal 12.
int heapIsEmpty(void *arena_ptr) {
    const Arena *arena = static_cast<const Arena *>(arena_ptr);
    if (!isValidArena(arena)) {
        return kBadArena;
    }
    return arena->used == 0 ? 1 : 0;
}

// Ordinal 13. IOP-12f: next-fit, starting after the rover; an exact fit is
// unlinked, a larger one split from its tail, which is why no re-linking of
// the free list is ever needed. One full circuit without a fit answers 0.
void *heapAlloc(void *arena_ptr, uint32_t requested) {
    Arena *arena = static_cast<Arena *>(arena_ptr);
    if (!isValidArena(arena)) {
        return nullptr;
    }
    const uint32_t n = requested < 8 ? 8 : requested;
    const uint32_t units = ((n + 7) >> 3) + 1;
    Block *prev = arena->rover;
    Block *p = prev->next;
    for (;;) {
        if (p->size >= units) {
            if (p->size == units) {
                prev->next = p->next;
            } else {
                p->size -= units;
                p += p->size;
                p->size = units;
            }
            arena->rover = prev;
            arena->used += units;
            p->next = reinterpret_cast<Block *>(arena);   // the ownership tag
            return reinterpret_cast<uint8_t *>(p) + 8;
        }
        if (p == arena->rover) {
            return nullptr;
        }
        prev = p;
        p = p->next;
    }
}

// Ordinal 14. IOP-12g: bracket the block by address in the free list, refuse
// with -3 for the three ways a double free or a corrupted pointer shows up,
// otherwise coalesce and leave the rover on the predecessor.
int heapFree(void *arena_ptr, void *ptr) {
    Arena *arena = static_cast<Arena *>(arena_ptr);
    if (!isValidArena(arena)) {
        return kBadArena;
    }
    Block *first = firstBlockOf(arena);
    const uint8_t *extent_end = reinterpret_cast<uint8_t *>(arena) + arena->size;
    if (ptr == nullptr) {
        return kOutOfRange;
    }
    Block *bp = reinterpret_cast<Block *>(static_cast<uint8_t *>(ptr) - 8);
    if (reinterpret_cast<uint8_t *>(bp) < reinterpret_cast<uint8_t *>(first)
        || reinterpret_cast<uint8_t *>(bp) >= extent_end) {
        return kOutOfRange;
    }
    if (bp->next != reinterpret_cast<Block *>(arena)) {
        return kNotOwned;    // not currently allocated by this arena
    }

    Block *p = arena->rover;
    for (;;) {
        // Checked every iteration, not just at the bracket: a plain
        // `p < bp && bp < p->next` test never stops on `bp == p->next`, so a
        // pointer that lands exactly on a free header would otherwise just
        // walk past it instead of being refused.
        if (bp == p || bp == p->next) {
            return kCorrupted;         // starts on a free header
        }
        // The preceding-block overlap test is vacuous at the list's wrap
        // point (`p >= p->next`), where `p` is the highest free address;
        // `p < bp` keeps it from firing there.
        if (p < bp && p + p->size > bp) {
            return kCorrupted;         // lies inside the preceding free block
        }
        if (p < p->next) {
            if (p < bp && bp < p->next) {
                break;
            }
        } else if (bp > p || bp < p->next) {
            break;
        }
        p = p->next;
    }
    // The sentinel (`size == 0`) is never a real neighbour to coalesce into;
    // doing so would erase the list's terminator and break HeapAlloc's
    // circuit test.
    if (p->next->size != 0 && bp + bp->size > p->next) {
        return kCorrupted;             // runs into the following free block
    }

    arena->used -= bp->size;           // before coalescing changes bp->size

    if (p->next->size != 0 && bp + bp->size == p->next) {
        bp->size += p->next->size;
        bp->next = p->next->next;
    } else {
        bp->next = p->next;
    }
    if (p + p->size == bp) {
        p->size += bp->size;
        p->next = bp->next;
    } else {
        p->next = bp;
    }
    arena->rover = p;
    return 0;
}

// Ordinal 15. IOP-12h: free bytes, no validation at all -- the one number a
// title can read, so it has to match the reference's formula exactly.
int heapChunkSize(void *arena_ptr) {
    const Arena *arena = static_cast<const Arena *>(arena_ptr);
    const int32_t units = (static_cast<int32_t>(arena->size) - 0x10) >> 3;
    return (units - static_cast<int32_t>(arena->used) - 1) << 3;
}

// --- ordinals 4-8: the public interface, built from the above (IOP-12i) ----

// Ordinal 4. IOP-12i: SYSMEM is asked for exactly the rounded request, and
// that whole block becomes the heap object's footprint -- its own 0x10-byte
// header eats into the request rather than being additional overhead, which
// is the up-to-0xFF-byte slack IOP-12i describes.
Heap *createHeap(int32_t size, int32_t flags) {
    const uint32_t rounded = (static_cast<uint32_t>(size) + 3) & ~3u;
    const uint32_t mode = (flags & 2) ? kSysmemHighest : kSysmemLowest;
    const int block = _import_sysmem_allocate(mode, rounded, 0);
    if (block == 0) {
        return nullptr;
    }
    Heap *heap = reinterpret_cast<Heap *>(block);
    heap->magic = heapMagicFor(heap);
    // IOP-12c: growable heaps carry `rounded` in the upper bits; a
    // non-growable one reads as a growth size of zero.
    heap->growth_and_mode = ((flags & 1) ? rounded : 0) | mode;
    heap->sentinel.next = &heap->sentinel;
    heap->sentinel.previous = &heap->sentinel;
    heapPrepare(heap + 1, static_cast<int32_t>(rounded) - 0x10);
    return heap;
}

// Ordinal 5. IOP-12g/12j: every grown chunk, then the heap; no liveness
// check. Answers whatever the last SYSMEM release did.
int deleteHeap(void *heap_ptr) {
    Heap *heap = static_cast<Heap *>(heap_ptr);
    if (!isValidHeap(heap)) {
        return kBadHeap;
    }
    ChunkNode *node = heap->sentinel.next;
    while (node != &heap->sentinel) {
        ChunkNode *next = node->next;
        _import_sysmem_release(reinterpret_cast<uint32_t>(node));
        node = next;
    }
    return _import_sysmem_release(reinterpret_cast<uint32_t>(heap));
}

// Ordinal 6. IOP-12c: the newest chunk is tried first, chunk 0 (the
// sentinel's own arena) last -- a new chunk is always inserted right after
// the sentinel, so following `next` from there and stopping back at the
// sentinel visits every grown chunk before it.
void *allocHeapMemory(void *heap_ptr, uint32_t size) {
    Heap *heap = static_cast<Heap *>(heap_ptr);
    if (!isValidHeap(heap)) {
        return nullptr;
    }
    ChunkNode *node = heap->sentinel.next;
    for (;;) {
        void *result = heapAlloc(arenaOf(node), size);
        if (result != nullptr) {
            return result;
        }
        if (node == &heap->sentinel) {
            break;
        }
        node = node->next;
    }

    const uint32_t growth = heap->growth_and_mode & ~1u;
    if (growth == 0) {
        return nullptr;    // not growable
    }
    const uint32_t mode = heap->growth_and_mode & 1u;
    const uint32_t floor = size + kChunkOverhead;
    const uint32_t chunk_size = growth > floor ? growth : floor;
    const int chunk_base = _import_sysmem_allocate(mode, chunk_size, 0);
    if (chunk_base == 0) {
        return nullptr;
    }
    // IOP-12i: sized from what SYSMEM actually granted, not the request, so
    // the growth path recovers the rounding slack CreateHeap leaves behind.
    const int actual_size = _import_sysmem_block_size(static_cast<uint32_t>(chunk_base));

    ChunkNode *new_node = reinterpret_cast<ChunkNode *>(chunk_base);
    new_node->next = heap->sentinel.next;
    new_node->previous = &heap->sentinel;
    heap->sentinel.next->previous = new_node;
    heap->sentinel.next = new_node;

    heapPrepare(arenaOf(new_node), actual_size - 8);
    return heapAlloc(arenaOf(new_node), size);
}

// Ordinal 7. IOP-12j: -1 for a null pointer, for a pointer no chunk owns, and
// for a double free of a block that has not been reissued -- all three read
// as the same ownership-tag failure once the tag is compared directly rather
// than probed chunk by chunk.
int freeHeapMemory(void *heap_ptr, void *ptr) {
    Heap *heap = static_cast<Heap *>(heap_ptr);
    if (!isValidHeap(heap)) {
        return kBadHeap;
    }
    if (ptr == nullptr) {
        return kNotOwned;
    }
    Block *tag = reinterpret_cast<Block *>(static_cast<uint8_t *>(ptr) - 8)->next;
    ChunkNode *node = heap->sentinel.next;
    for (;;) {
        Arena *arena = arenaOf(node);
        if (tag == reinterpret_cast<Block *>(arena)) {
            const int result = heapFree(arena, ptr);
            // A grown chunk whose last block has just been freed is unlinked
            // and returned to SYSMEM (IOP-12g); chunk 0 lives in the heap
            // object itself and is never released this way.
            if (result == 0 && node != &heap->sentinel && arena->used == 0) {
                node->previous->next = node->next;
                node->next->previous = node->previous;
                _import_sysmem_release(reinterpret_cast<uint32_t>(node));
            }
            return result;
        }
        if (node == &heap->sentinel) {
            return kNotOwned;
        }
        node = node->next;
    }
}

// Ordinal 8. IOP-12h: summed over every chunk -- the number a variable
// pool's capacity is read from.
int heapTotalFreeSize(void *heap_ptr) {
    Heap *heap = static_cast<Heap *>(heap_ptr);
    if (!isValidHeap(heap)) {
        return kBadHeap;
    }
    int total = 0;
    ChunkNode *node = heap->sentinel.next;
    for (;;) {
        total += heapChunkSize(arenaOf(node));
        if (node == &heap->sentinel) {
            break;
        }
        node = node->next;
    }
    return total;
}

// IOP-12a: eighteen slots so that every later ordinal a client expects stays
// where it is (an ordinal past the end binds to `jr $ra` with no
// diagnostic). 1, 2, 3, 9, 10, 16 and 17 are stubs that leave $v0 untouched.
PS2_EXPORT_TABLE ExportTable<18> heaplib_exports = {
    ps2::module::kExportMagic,
    0,
    0x0101,
    0,
    {'h', 'e', 'a', 'p', 'l', 'i', 'b', 0},
    {
        slot(reservedHook),          // 0  the entry, in the reference
        slot(reservedHook),          // 1
        slot(reservedHook),          // 2
        slot(reservedHook),          // 3
        slot(createHeap),            // 4  CreateHeap
        slot(deleteHeap),            // 5  DeleteHeap
        slot(allocHeapMemory),       // 6  AllocHeapMemory
        slot(freeHeapMemory),        // 7  FreeHeapMemory
        slot(heapTotalFreeSize),     // 8  HeapTotalFreeSize
        slot(reservedHook),          // 9
        slot(reservedHook),          // 10
        slot(heapPrepare),           // 11 HeapPrepare
        slot(heapIsEmpty),           // 12 HeapIsEmpty
        slot(heapAlloc),             // 13 HeapAlloc
        slot(heapFree),              // 14 HeapFree
        slot(heapChunkSize),         // 15 HeapChunkSize
        slot(reservedHook),          // 16
        slot(reservedHook),          // 17
        nullptr,
    },
};

}  // namespace

PS2_IMPORTS_BEGIN("sysmem\0\0", 0x0101)
PS2_IMPORT(_import_sysmem_allocate, 4)
PS2_IMPORT(_import_sysmem_release, 5)
PS2_IMPORT(_import_sysmem_block_size, 10)
PS2_IMPORTS_END()

PS2_IMPORTS_BEGIN("loadcore", 0x0101)
PS2_IMPORT(_import_loadcore_register, 6)
PS2_IMPORTS_END()

extern "C" {

// IOP-12a: a refused registration ends the entry non-resident, and nothing
// else runs -- no chunk, no arena, no global state -- since a heap lives
// entirely in memory the caller holds.
int _module_start(int, char **) {
    return _import_loadcore_register(&heaplib_exports) < 0 ? 1 : 0;
}

}  // extern "C"
