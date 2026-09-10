# UDNL: staging the merged kernel, and the hand-over into it

`docs/analysis/45` §2 read the reboot chain as far as `UDNL`'s merge rule and
stopped: the per-module copy and the hand-over were never reached, and it
guessed at both. This is the rest of the module, and the guesses were wrong in
a way worth stating plainly: **nothing is copied per module, and the buffer is
not an archive.** Offsets are `UDNL` module-relative.

## 1. What the buffer is for

`UDNL` allocates one buffer and puts three things in it: an eight-word boot
block (`docs/spec/03-boot-chain.md` BOOT-8c), a boot list of single words
(BOOT-8d), and the **whole file** of every image its `argv` named, one after
another. The modules themselves are never moved: the list holds a pointer to
each winning module's bytes *where they already lie*, either inside one of
those images or in the ROM window at `0xBFC0xxxx`.

The size is `0x420` plus the sum of the argv sources' **file sizes**, each
rounded up to 16 -- and it is computed before any name is resolved, so it has
nothing to do with which modules win. `0x420` is `0x20` for the block and
`0x400` for the list, which is therefore capped at 256 words. Nothing checks
that cap: the terminating zero is written unconditionally, so a boot list of
more than 255 names would run into the first image.

Each source's size comes from `ioman` ordinal 8 (`lseek`) to the end and back
-- `docs/analysis/45` names ordinal 6 for this, which is `read`.

## 2. Where the buffer goes

Not from an ordinary allocation. `UDNL` asks `sysmem` ordinal 9
(`QueryBlockTopAddress`) about its own image's end, then walks the block list
with ordinal 10 (`QueryBlockSize`) -- the reference `SYSMEM` sets bit 31 of
both answers for a **free** block -- skipping blocks that are in use or too
small, and allocates at the first that fits with **mode 2**, the
allocate-at-address mode (IRX-15b). So the staged kernel is built immediately
above `UDNL` itself, in memory the previous kernel has already released.

A failure prints a panic, sleeps the thread and breaks; every panic in this
module has that shape.

## 3. The copy loop

For each source, in order: `read` the whole file to the cursor, which starts
at `buffer + 0x420`; run the archive scan (`ARC-4`) over the first 0x4000
bytes of what was read to find that image's `ROMDIR`; record it; advance the
cursor by `align16(size)`. Nothing is closed, and a file whose table is not
found is **silently overwritten by the next one**, since the cursor does not
advance.

There is no index, no `ROMDIR` rebuilt over the buffer, and no `EXTINFO`. The
merge's output is the boot list: one word per name, the address of the winning
module's file image.

## 4. Two corrections to the `IOPBTCONF` grammar

`docs/analysis/45` has `"!addr "` and `"!include "` the wrong way round.
`src/iop/udnl.cpp` takes them from this table, not from `45`, and recognises
and skips both -- neither retail list carries either:

| Token | Effect |
| --- | --- |
| `@hex` | writes the boot block's `+0xc` -- the load address `SYSMEM` will go to |
| `!addr hex` | appends `(hex << 2) \| 1` to the list: BOOT-8d's tagged entry, which tells the new `loadcore` where to put the module after it |
| `!include name` | resolves *a name* across the sources and **recurses** into this same parser over what it finds |

The version compared is the `EXTINFO` type `0x02` record (`ARC-6b`), as our
model already has it. Names match on ten bytes. Candidates are walked
**newest source first** and replaced only on a strictly greater version, so an
equal version leaves the newest winner standing. `src/iop/udnl.cpp` walked the
other way when this was written, which agrees only while there is exactly one
named source; `db21b27` turned it round.

A source whose name begins with `mc`, `hd`, `net` or `dev` followed by a digit
is refused outright, silently, before anything is opened.

## 5. The hand-over

Interrupts are disabled first and never re-enabled. `$sp` is not touched: the
sequence runs on `UDNL`'s own thread stack, and it is the new `loadcore` that
resets the stack from the block's `+0x0`.

1. Parse `list[0]` -- `SYSMEM` -- with `UDNL`'s own loader (§6). Its load
   address is the block's `+0xc` plus `0x30`; every rom0 boot list says `@800`,
   so `SYSMEM`'s record is at `0x800` and its image at `0x830`.
2. Place it, and write the 0x30-byte module record below it: name pointer at
   `+0x4`, version `+0x8`, entry `+0x10`, `$gp` `+0x14`, text start `+0x18`,
   and the text, data and bss sizes at `+0x1c`, `+0x20`, `+0x24`.
3. Flush the I-cache.
4. Call `SYSMEM`'s entry with **RAM size in bytes** in `$a0`. **Its return
   value is where `loadcore`'s record goes**: `loadcore` is placed at that
   address plus `0x30`.
5. Place `loadcore` the same way, write the block's `+0xc` with `SYSMEM`'s
   actual load address, and call `loadcore`'s entry with the block's address.
   It does not return; if it did, the code stores to `0x80000000` forever.

That is the same sequence, step for step, as `IOPBOOT`'s at a cold boot. The
differences are only that the block lives inside the buffer rather than at a
fixed low address, that its mode is `3` rather than `0`, that there is no
command line -- so the new kernel builds no key-5 record and `MODLOAD`
registers no callback -- and that the list points at bytes in RAM and ROM
rather than at names to resolve. **`IOPBOOT` is not re-entered**, no syscall or
`eret` is involved, and no `0x3F0` table is built here: the new `loadcore`
builds it from the block exactly as at a cold boot.

The block `UDNL` fills in:

| Offset | Value |
| --- | --- |
| `+0x00` | RAM in MiB, from `sysmem` ordinal 6 |
| `+0x04` | `3` -- the boot mode `IGREETING`'s fourth banner selects on |
| `+0x08` | `0`: no command line |
| `+0x0c` | the `@` address from the boot list, overwritten at the end with `SYSMEM`'s actual load address |
| `+0x10` | `(buffer + 0x420)` rounded down to `0x100`: the reserved region's base |
| `+0x14` | its size, covering every image read in |
| `+0x18` | how many names the list holds |
| `+0x1c` | `buffer + 0x20` |

The reserved region is the images themselves: the new kernel loads its modules
*out of* them, so it must not allocate over them while it does.

## 6. UDNL's loader is stricter than the one it hands to

`UDNL` carries its own ELF loader rather than using any `loadcore`. It accepts
COFF, an absolute ELF (`e_type` 2) and a relocatable one (`e_type` `0xFF80`),
requires exactly two program headers with the first being `.iopmod`, and never
checks the ELF magic. Its relocator handles `R_MIPS_16`, `R_MIPS_32`,
`R_MIPS_26` and `R_MIPS_HI16` only: a `HI16` **consumes the next entry as its
`LO16` unconditionally**, and a `LO16` that reaches the switch on its own is a
no-op rather than being rebased.

That is stricter than the `LOADCORE` 2.06 a title's image carries, which at
least applies a lone `LO16` (`docs/spec/02-module-abi.md` IRX-3b). Both agree
that a run of two `HI16` is not loadable, which is why `tools/mkirx.py`
re-orders the fixups into strict pairs.

## 7. `0x1F8100` is not in the reference

`docs/analysis/45` left it as an open question after failing to find it in
`UDNL`, `IOPBOOT` or `REBOOT`. It is not there because it is **ours**: the
private boot bookkeeping `src/boot/iopboot.cpp` writes and
`src/iop/loadcore.cpp` and `src/iop/eesync.cpp` read.
