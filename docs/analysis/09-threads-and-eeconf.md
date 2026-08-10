# Threads and EE Configuration: THREADMAN and EECONF

`EECONF` and `THREADMAN` are boot-list entries twelve and thirteen
(`docs/analysis/03-iopboot-and-boot-list.md`). `THREADMAN` is the largest module
in the IOP kernel — text `0x6440`, seven exported libraries — and its
registration sequence answers a question left open by
`docs/analysis/08-c-library-and-heap.md`: what sets the "pinned" flag that
exempts a client from supersession.

```sh
python3 tools/romdir.py assets/SCPH-50000.bin --extract <outdir>   # outside the repo
python3 tools/irxinfo.py <outdir>/THREADMAN --imports
python3 tools/irxinfo.py <outdir>/EECONF --imports
```

## THREADMAN exports seven libraries

| Table | Library | Entries |
| --- | --- | --- |
| `0x5F80` | `thbase` 1.01 | 42 |
| `0x6044` | `thevent` 1.01 | 15 |
| `0x609C` | `thsemap` 1.01 | 13 |
| `0x60EC` | `thmsgbx` 1.01 | 13 |
| `0x613C` | `thfpool` 1.01 | 13 |
| `0x618C` | `thvpool` 1.01 | 13 |
| `0x6230` | `thrdman` 1.02 | 4 |

One file, seven independently bindable libraries: base threading, events,
semaphores, message boxes, fixed-size pools, variable-size pools, and a small
four-entry `thrdman`. Importers bind to whichever groups they need, so the split
is not cosmetic — it is the granularity at which the rest of the kernel depends
on threading. The tables sit among tail-end code rather than in one block; the
bytes between `thvpool`'s terminator and `thrdman` are ordinary routines.

Its imports show the fan-in, seven libraries deep:

| Library | Ordinals |
| --- | --- |
| `sysmem` | 4, 5, 9, 10, 14 |
| `loadcore` | 6, 10, 12, 20, 24 |
| `intrman` | 4, 6, 8, 9, 14, 17, 18, 23, 28, 30 |
| `stdio` | 4 |
| `sysclib` | 12, 14 |
| `timrman` | 4, 7, 8, 9, 10, 11, 16 |
| `heaplib` | 4, 5, 6, 7, 8 |

## Two registration APIs

`THREADMAN`'s entry registers all seven tables, but not all the same way:

```sh
python3 tools/irxinfo.py <outdir>/THREADMAN --dump-load <outdir>/THREADMAN.text
python3 tools/romdis.py <outdir>/THREADMAN.text --cpu iop --vma 0 --range 0x10 0xb0
```

Stub addresses follow from the tables (`07` §"Naming ordinals from their use"):
the `loadcore` table at `0x62A4` puts ordinal 6 at `0x62B8` and ordinal 10 at
`0x62C0`. The entry calls ordinal **10** first, with the `thrdman` table, and
aborts if it fails; then calls ordinal **6** six times, once per remaining
table.

`LOADCORE` ordinal 10 turns out to be a different kind of registration
altogether:

```sh
python3 tools/romdis.py <outdir>/LOADCORE.text --cpu iop --vma 0 --range 0xa58 0xab8
```

```
 a58:  beqz  $a0, ...            # null -> -1
 a64:  lw    $v1, 0($a0)
 a68:  lui   $v0, 0x41c0
 a6c:  beq   $v1, $v0, 0xa7c     # export magic required
 a7c:  lhu   $v0, 0xa($a0)
 a88:  ori   $v0, $v0, 1         # set flags bit 0
 a8c:  sh    $v0, 0xa($a0)
 a90:  lw    $v0, 0(registry)    # link at the head, unconditionally
 a98:  sw    $v0, 0($a0)
 aa0:  sw    $a0, 0(registry)
```

It validates the magic, **sets flags bit 0 on the table itself**, and links it
into the registry head with **no tag or version comparison at all**. Contrast
ordinal 6 (`08` §"What registration actually compares"), which arbitrates by tag
and version and can be rejected.

So the two entry points are:

- **ordinal 6** — versioned registration: same tag and major version means the
  same library, and a strictly greater minor supersedes it, inheriting its
  unpinned clients.
- **ordinal 10** — pinned registration: unconditional, and it marks the library
  with the very flag that makes supersession skip its clients.

That closes the loop `08` left open. Flags bit 0 is not something a build tool
stamps into the stored table — `THREADMAN`'s stored `thrdman` table has
`flags 0x0`, and the bit is set at run time by the API that registers it.

## A supersession that actually happens

`08` showed `SYSCLIB` publishing a provisional `stdio` 1.01 so that modules
loading before `STDIO` have something to bind to. `THREADMAN` is one of those
modules, and the timing is concrete: in `IOPBTCONF` order `SYSCLIB` is tenth,
`THREADMAN` thirteenth, `STDIO` eighteenth. `THREADMAN` imports `stdio`
ordinal 4 with `flags 0x0` — bit 0 clear, so unpinned — meaning it binds to
`SYSCLIB`'s provisional implementation at load time and is re-bound to `STDIO`'s
when that registers five modules later.

A rebuild gets this wrong in a way that is easy to miss: if `SYSCLIB` registered
its `stdio` at the stored 1.02, `STDIO`'s registration would be rejected and
`THREADMAN` would keep calling the provisional implementation forever, with no
error anywhere.

## EECONF queries a boot record

`EECONF` (`eeconfig` 1.03, 8 exports) imports just `sysmem` 4 and `loadcore` 12,
and its entry leads with the latter:

```sh
python3 tools/irxinfo.py <outdir>/EECONF --dump-load <outdir>/EECONF.text
python3 tools/romdis.py <outdir>/EECONF.text --cpu iop --vma 0 --range 0x638 0x690
```

```
 638:  addiu $sp, $sp, -0x40
 63c:  addiu $a0, $zero, 3
 650:  jal   0xba8               # loadcore 12, key = 3
 658:  beqz  $v0, 0x684          # not found -> skip
 660:  lw    $v1, 4($v0)         # the record's flag word
 668:  andi  $v0, $v1, 1
 66c:  bnez  $v0, 0xb14          # -> return 1
 674:  andi  $v0, $v1, 2
 678:  bnez  $v0, 0xb14          # -> return 1
```

`LOADCORE` ordinal 12 is a lookup by a small integer key over a structure
reached through the **absolute low-memory address `0x3F0`**:

```sh
python3 tools/romdis.py <outdir>/LOADCORE.text --cpu iop --vma 0 --range 0x640 0x670
```

```
 640:  lw    $v1, 0x3f0($zero)   # fixed low-RAM anchor
 648:  lw    $v0, 0($v1)
 658:  lbu   $v0, 2($v1)
 660:  beq   $a0, $v0, ...       # match the key byte
```

That is a boot-parameter table handed down through low RAM rather than passed as
an argument — the first structure seen at a fixed absolute address since the
exception vector at 0 (`06`). Its layout and who populates it belong with
`IOPBOOT`'s load step; what matters here is that a fixed address is part of the
kernel's ABI and a rebuild cannot relocate it.

`EECONF` returning 1 when either of two record flag bits is set is the same
non-residency convention the `P`/`I` variant pairs use (`06`), now seen driven
by boot-time configuration rather than by a hardware probe. It strengthens the
case that a `1` return means "do not stay resident" — still the open question
recorded in `docs/project-state.md`.

## What this pins for the rebuild

- `THREADMAN` must export seven separately bindable libraries with those entry
  counts, since importers bind per library and by ordinal.
- Registration has two APIs and they are not interchangeable: ordinal 6
  arbitrates by version, ordinal 10 pins unconditionally and sets flags bit 0
  at run time.
- The boot-parameter table anchored at absolute `0x3F0` is ABI.

Next in boot order: `VBLANK`, `IOMAN` and `MODLOAD` — with `MODLOAD` likely to
settle the open questions about per-module load and the `1` return
(`docs/project-state.md` §5).
