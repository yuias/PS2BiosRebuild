# HEAPLIB: the heap the reference's thread manager allocates from

`SYSMEM` is two bump cursors (`docs/spec/06-iop-kernel.md` IRX-15): it hands
out 0x100-granular blocks and takes back only the last one at each end.
Nothing built on it can allocate and free in an arbitrary order, and the
kernel needs that in two places -- a variable pool (`CreateVpl`) is exactly
"allocate and free in any order", and the thread manager's alarm records come
and go one at a time. `HEAPLIB` is the module that supplies it: a K&R-style
arena allocator over blocks it takes from `SYSMEM`, with no state of its own.

Read from the reference `rom0` entry `HEAPLIB` (`Heap_lib` 1.01, text 0x880,
data 0x20, no bss), its `SYSMEM` ordinal 10, and the call sites in the
`THREADMAN` 2.03 a title's `IOPRP` image carries. Offsets below are
module-relative, as `tools/irxinfo.py --dump-load` gives them.

## 1. Shape

It exports `heaplib` v1.01 with **18 entries** and imports only `sysmem` 4, 5
and 10 and `loadcore` 6. The entry point registers the export table through
`loadcore` 6 and returns that result unchanged -- so a refused registration
unloads the module (IRX-12a) -- and does nothing else. There is no `.bss` and
no global: every heap lives in the `SYSMEM` memory the caller holds a handle
to, and the module is pure code. Its 0x20 bytes of data are the name string
and the `moduleinfo` pair, not state.

Ordinals 4 to 8 are the public interface; 11 to 15 are the arena primitives
those five are built from, exported as they are; 1, 2, 3, 9, 10, 16 and 17
are stubs that return with `$v0` untouched. The extent 18 is load-bearing in
the usual way (IRX-6a): an ordinal past the end binds to `jr $ra` with no
diagnostic, so a table that stops earlier would silently mute a caller.

| ord | name | signature | behaviour |
| --- | --- | --- | --- |
| 4 | `CreateHeap` | `(int size, int flags) -> Heap *` | round `size` up to 4, take a `SYSMEM` block of it, build the heap object |
| 5 | `DeleteHeap` | `(Heap *) -> int` | free every grown chunk, then the heap; no liveness check |
| 6 | `AllocHeapMemory` | `(Heap *, size_t) -> void *` | newest chunk first, chunk 0 last, then grow if allowed |
| 7 | `FreeHeapMemory` | `(Heap *, void *) -> int` | 0, or -1 if no chunk owns the pointer, or -4 for a bad heap |
| 8 | `HeapTotalFreeSize` | `(Heap *) -> int` | sum of `HeapChunkSize` over every chunk, or -4 |
| 11 | `HeapPrepare` | `(void *memory, int size) -> void` | initialise a raw arena in the caller's memory |
| 12 | `HeapIsEmpty` | `(void *arena) -> int` | 1 when nothing is allocated |
| 13 | `HeapAlloc` | `(void *arena, size_t) -> void *` | the next-fit allocator |
| 14 | `HeapFree` | `(void *arena, void *) -> int` | 0, -1, -2, -3 or -4 |
| 15 | `HeapChunkSize` | `(void *arena) -> int` | free bytes; no validation at all |

## 2. The heap object and the arena

Two distinct **self-relative magics** keep the two kinds of object from being
mistaken for each other: a heap's first word is `heap + 1`, an arena's is
`arena - 1`. A pointer that has been handed to the wrong entry point fails on
the arithmetic rather than on a constant that could occur by accident.

A **heap** is 0x10 bytes followed by its first arena:

| offset | field |
| --- | --- |
| `+0x0` | magic, `heap + 1` |
| `+0x4` | the growth size in its upper bits, the `SYSMEM` mode in bit 0 |
| `+0x8`, `+0xc` | the `{next, previous}` sentinel of the grown-chunk list |
| `+0x10` | arena 0 |

`+0x4` carries two things at once: the rounded `size` `CreateHeap` was given,
if `flags` bit 0 asked for a growable heap and zero if it did not, and in bit
0 the `SYSMEM` mode the heap allocates with -- 1 (highest) when `flags` bit 1
was set, 0 (lowest) otherwise. So "may this heap grow" and "how big is a new
chunk" are the same test, and a non-growable heap reads as a growth size of
zero.

The sentinel at `+0x8` is also chunk 0's list node, because arena 0 begins at
`+0x10` and a grown chunk's arena begins at *its* `+0x8`: every walk reaches an
arena as `node + 8` and stops when it comes back to the sentinel. A new chunk
is inserted directly after the sentinel, so **allocation tries the newest
chunk first and chunk 0 last**.

An **arena** is K&R `malloc` in a fixed region, in units of 8 bytes:

| offset | field |
| --- | --- |
| `+0x0` | magic, `arena - 1` |
| `+0x4` | the arena's size in bytes, as given |
| `+0x8` | units currently allocated, headers included |
| `+0xc` | the rover: the free-list node the next search starts after |
| `+0x10` | the first block header |

A block header is one unit, `{next, size_in_units}`, and `size` counts the
header. The user pointer is the header plus 8, so user memory is 8-byte
aligned and never 16. While a block is free, `next` links the address-ordered
circular free list; **while it is allocated, `next` holds the arena's own
address**, and that is the ownership tag `HeapFree` tests -- which is why
freeing a pointer into the wrong heap is detected rather than corrupting it.

`HeapPrepare` lays out `units = (size - 0x10) >> 3` of them: the last unit
becomes an **end sentinel** with `size` zero, the first block gets
`units - 1`, the free list is the circular pair of the two, and the rover
starts at the first block. A `size` below 0x29 leaves the memory untouched and
says nothing, so the caller's magic never appears and every later call on that
arena fails. The sentinel is what makes the search well founded: `size` zero
never satisfies a request, so a full circuit always terminates.

**Allocation** is next-fit. A request below 8 is raised to 8, so a zero-size
request succeeds with 8 usable bytes, and `units = ((n + 7) >> 3) + 1`. The
search starts after the rover and takes the first block that fits; an exact
fit is unlinked, a larger one is **split from its tail**, which leaves the
free list's order untouched and is why no re-linking is needed. The rover then
points at the predecessor of what was taken, `next` becomes the arena tag, and
the used count rises. One full circuit without a fit answers 0.

**Freeing** finds the free-list pair that brackets the block by address and
refuses, with -3, a block that starts on a free header, that runs into the
following free block, or that lies inside the preceding one -- the three ways
a double free or a corrupted pointer shows up. Otherwise it coalesces forward
when the next block is not the sentinel, backward when the preceding free
block ends exactly at it, and leaves the rover on the predecessor. The free
list therefore stays address-sorted with the sentinel highest.

`HeapChunkSize` reports `(((size - 0x10) >> 3) - used - 1) << 3` -- the units
the arena has, less the ones in use, less the sentinel, in bytes. It validates
nothing.

## 3. What it asks of SYSMEM

| caller | ordinal | arguments |
| --- | --- | --- |
| `CreateHeap` | 4 | `(flags & 2 ? 1 : 0, size rounded to 4, 0)` |
| `AllocHeapMemory`, growing | 4 | `(the heap's mode bit, max(growth size, n + 0x28), 0)` |
| `AllocHeapMemory`, growing | 10 | the chunk it just got |
| `FreeHeapMemory` | 5 | a grown chunk whose last block has just been freed |
| `DeleteHeap` | 5 | every chunk, then the heap |

The two sizings are not the same, and the difference is the interesting part.
`CreateHeap` sizes arena 0 from the **request**, so up to 0xFF bytes of the
0x100-granular block it was given are never used. The growth path sizes the
new chunk's arena from **`SYSMEM` ordinal 10**, so it recovers that slack --
which is why ordinal 10 has to answer correctly for a block that was just
allocated, and why an unimplemented -1 there is not a failed allocation but an
arena of 0xFFFFFFF7 bytes whose end sentinel lands below its own memory. The
0x28 floor is the chunk's own overhead: 8 for the list node, 0x10 for the
arena header, 8 for the block header and 8 for the sentinel.

`SYSMEM` ordinal 10 itself walks the block records: each packs a base and a
size, a record matches **any address inside its block** rather than only its
base, and the answer is the size in bytes, a multiple of 0x100. A record whose
low bit is clear has 0x80000000 set in the answer; what sets that bit was not
read, and `HEAPLIB` never sees it, since it only ever queries a block it has
just been given.

## 4. Failure

Every entry point that takes a heap or an arena checks its magic first and
answers -4 (heap) or 0 / -4 (arena) rather than touching the memory.
Allocation failure is 0 throughout: a full non-growable heap, a growable one
whose `SYSMEM` request failed, a request so large that the unit count reads
negative. `FreeHeapMemory` answers -1 for a null pointer, for a pointer no
chunk owns, and for a double free of a block that has not been reissued --
that last because the header's `next` is a free-list link by then and no
longer the arena tag. A double free of a block that *has* been reissued
succeeds and frees the new owner's memory, silently; nothing can detect it.
`DeleteHeap` frees everything whether or not it is live, and answers what its
last `SYSMEM` release did. Nothing in the module disables interrupts or takes
a lock, so serialising a heap is the caller's business.

## 5. Why the numbers are title-visible

The `THREADMAN` a title's `IOPRP` carries uses `HEAPLIB` for both of the jobs
above. Its entry creates one growable heap of 0x800 bytes for alarm records.
`CreateVpl` is `CreateHeap(size, attr & 0x200 ? 2 : 0)` -- so a variable pool
*is* a heap -- and it then stores `HeapTotalFreeSize` of the new heap as the
pool's capacity, which `ReferVplStatus` reports and a title may act on.
`AllocateVpl` and `FreeVpl` are ordinals 6 and 7 directly.

So `HeapChunkSize`'s formula is not an internal detail: for a 0x800-byte pool
it has to read 0x7d8, because that is the number the reference publishes.
