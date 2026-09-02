# VBLANK: the IOP's Two Vertical-Blank Lines, and the `vblank` Library

Nothing in this project had read `VBLANK` before. It became worth reading when a
retail title's `PADMAN.IRX` turned out to import `vblank` ordinals 8 and 9 and,
finding no exporter, to load and go quiet — the same silent failure
`docs/analysis/38` §3.3 documented for `thmsgbx`.

The module is small (3 KiB), it is the fourteenth name in the reference's
`IOPBTCONF`, and what it does is narrow: it owns the IOP's two vertical-blank
interrupt lines and multiplexes each into a priority-ordered callback list, plus
one event flag with four bits. This document reads it exhaustively.

```sh
python3 tools/romdir.py assets/SCPH-50000.bin --extract <outdir>   # outside the repo
python3 tools/irxinfo.py <outdir>/VBLANK --exports --imports
python3 tools/irxinfo.py <outdir>/VBLANK --dump-load <outdir>/VBLANK.text
python3 tools/romdis.py <outdir>/VBLANK.text --cpu iop --vma 0
readelf -S <outdir>/VBLANK; readelf -r <outdir>/VBLANK
# the client
python3 tools/irxinfo.py <outdir>/PADMAN --imports
python3 tools/romdis.py <outdir>/PADMAN.text --cpu iop --vma 0 --range 0x2e9c 0x2f90
```

Addresses are `VBLANK.text` offsets (module vaddr, base 0). Names marked
`[header]` come from ps2sdk's `vblank.h`/`thevent.h`/`intrman.h`/`kerr.h`; the
binary is the authority.

## 0. Cross-references established, not re-derived

- `docs/analysis/37` §2.2/§3.5: `RegisterIntrHandler(irq, mode, handler, arg)`
  takes one handler per line, and the dispatcher re-enables the line in
  `I_MASK` only if the handler returns nonzero. §2.4: `QueryIntrContext()` is a
  `$sp`-range test. **Nothing in this reading contradicts that document.**
- `docs/analysis/38` §3.1: `iClearEventFlag`'s argument is a **keep**-mask, and
  `EA_MULTI` (`2`) is the only attribute `CreateEventFlag` accepts.
- `docs/analysis/08`: `sysclib` ordinal 14 is `memset`.

## 1. Ordinal table

`vblank` v1.01 at `0x690`, 10 entries.

| Ord | Addr | Name [header] | Notes |
|---|---|---|---|
| 0 | `0x0` | module entry | §2 |
| 1, 2 | `0x6d0` | reserved | a bare `jr $ra` / `nop`, as in `docs/analysis/38` §1.1 |
| 3 | `0x154` | — | returns the module's `.bss` base, the shape `GetThreadmanData` has |
| 4 | `0x544` | `WaitVblankStart` | `WaitEventFlag(evf, 0x1, WEF_OR, NULL)` |
| 5 | `0x574` | `WaitVblankEnd` | `WaitEventFlag(evf, 0x4, WEF_OR, NULL)` |
| 6 | `0x5a4` | `WaitVblank` | `WaitEventFlag(evf, 0x2, WEF_OR, NULL)` |
| 7 | `0x5d4` | `WaitNonVblank` | `WaitEventFlag(evf, 0x8, WEF_OR, NULL)` |
| 8 | `0x164` | `RegisterVblankHandler` | §3 |
| 9 | `0x2ac` | `ReleaseVblankHandler` | §3 |

Every slot but the two reserved ones is a real implementation; there is nothing
here of the declared-but-stubbed kind `docs/analysis/38` found in `thbase`.

Unexported bodies referred to below: `0x374` and `0x42c` are the two `INTRMAN`
line handlers, `0x4b4` and `0x4fc` the module's own two callbacks, and
`0x610`/`0x61c`/`0x630`/`0x668` the list primitives. `0x650` is an unreferenced
`prev == next` test — dead code in this build.

Import stubs, resolved from the stub table at `0x6e0` (each `jr $ra` /
`addiu $zero,$zero,ordinal`): `loadcore` 6, `intrman` 4/6/17/18/23, `sysclib`
14, `thbase` 41, `thevent` 4/7/9/10.

## 2. The entry

The whole of it runs inside one `CpuSuspendIntr` bracket:

1. `RegisterLibraryEntries` on the table at `0x690`. **A nonzero answer ends
   the entry**: it resumes interrupts and returns 1, not resident, having
   created no event flag and registered no interrupt handler.
2. `memset` the whole `0x160`-byte `.bss`.
3. Self-link three list heads — start, end, free — and append 16 pool nodes of
   `0x14` bytes each to the free list.
4. `CreateEventFlag({ attr = EA_MULTI, option = 0, bits = 0 })`, id kept at the
   `.bss` base.
5. Register its **own** two callbacks through its own exported ordinal 8, at
   priority `0x80`, with the `.bss` base as their argument — one on each list.
   The nested suspend this implies is discussed in §3.4.
6. `RegisterIntrHandler(0, 1, ...)` and `RegisterIntrHandler(0xb, 1, ...)`,
   then `EnableIntr` on both. Neither return value is checked.
7. Resume, return 0 — resident.

No thread is created, and nothing is registered with `loadcore` beyond the
library table.

## 3. Ordinals 8 and 9

```
int RegisterVblankHandler(int startend, int priority, int (*handler)(void *), void *arg);
int ReleaseVblankHandler (int startend, int (*handler)(void *));
```

`startend` selects a list by a `bnez` test: **0 is the start list, anything
else is the end list**. `priority` is compared signed. Neither `handler` nor
`arg` is validated.

### 3.1 Register

In order: `QueryIntrContext()` nonzero → `-100` `KE_ILLEGAL_CONTEXT`, before
any bracket is opened. Then suspend; free list empty → `-400` `KE_NO_MEMORY`;
a node already on **this** list with the same `handler` → `-104`
`KE_FOUND_HANDLER`. Otherwise the node is inserted before the first entry whose
priority is **greater** than the new one — ascending priority, ties behind, so
a lower number runs first — filled in with `{priority, handler, arg}`, and the
call answers 0.

Two consequences an implementer needs. **Identity is (list, handler pointer)**:
`arg` and `priority` play no part in the duplicate test, and the same handler
may be registered once on each list. And the pool of 16 nodes is **shared by
both lists**, two of which the module spends on itself at boot, leaving 14.

### 3.2 Release

`QueryIntrContext()` nonzero → `-100`. Then suspend, scan the chosen list for
the handler pointer; not found → `-105` `KE_NOTFOUND_HANDLER`; found → unlink,
return the node to the free list, answer 0. The node's fields are not cleared,
only overwritten on the next allocation.

**`PADMAN` retries this call for ever on any nonzero answer** (its `padEnd`
path loops `Release`, prints a diagnostic, and jumps back). A rebuild whose
ordinal 9 answers anything but 0 for a registered handler hangs that thread —
and so, for what it is worth, would the reference on a second `padEnd`.

### 3.3 What the line handlers do

Both are the same shape. Walking its list, each handler **captures a node's
`next` before calling it**, calls `handler(arg)`, and — if the callback answers
**0** — unlinks that node and returns it to the free list. A callback answering
nonzero stays registered. Self-removal is therefore safe, and removal of
another node from inside a callback is impossible anyway, since ordinal 9
refuses interrupt context.

Both line handlers **always answer 1**, so `INTRMAN` re-enables the line after
every dispatch (`docs/analysis/37` §3.5) and the two lines stay armed for the
life of the system.

Callbacks run inside the interrupt dispatch, on the interrupt stack, so
`QueryIntrContext()` is nonzero throughout — which is exactly what they need,
because both the module's own callbacks and `PADMAN`'s use the `i` forms of the
event-flag calls, and those answer `-100` from thread context. **A rebuild that
delivered these callbacks from a thread would break pad input with no error
anywhere.**

The start handler additionally keeps a counter of dispatches, and on the very
first one — before the counter moves — raises bit `0x200` in `THREADMAN`'s
system status flag (`thbase` ordinal 41). Nothing in the reference archive
waits on that bit (§5).

### 3.4 Nested suspend is the normal case

The entry calls ordinal 8 inside its own bracket, and `PADMAN` calls it inside
one of its own; ordinal 8 opens a second bracket regardless, ignores what
`CpuSuspendIntr` answers, and resumes with the value it saved. For that to be
correct, suspend must save the *actual* state and resume must write that state
back rather than unconditionally enabling. This constrains a rebuild; it does
not contradict `docs/analysis/37` §2.5.

## 4. Ordinals 4–7, and the four bits

All four are `WaitEventFlag(id, bits, WEF_OR, NULL)` on the module's own flag,
with **a null result pointer** — which both of `THREADMAN`'s paths check before
writing through, so it is safe. Their return is whatever `WaitEventFlag`
returns.

The bits are set and cleared only by the module's own two callbacks, and the
split is between *pulses* and *levels*:

| Bit | Set by | Cleared by | Meaning |
|---|---|---|---|
| `0x1` | the start callback | itself, in the same call | start **pulse** |
| `0x2` | the start callback | the end callback | **level**: inside vblank |
| `0x4` | the end callback | itself | end **pulse** |
| `0x8` | the end callback | the start callback | **level**: outside vblank |

Each callback sets its two bits and then clears — with a keep-mask — its own
pulse bit and the other callback's level bit, in that order. So the pulse is
set and cleared inside one interrupt: waiters are released at the set, and a
caller arriving afterwards blocks until the next edge. `WaitVblank` and
`WaitNonVblank` return at once when the level already holds. Before the first
interrupt of each kind every bit is 0, so every waiter blocks.

## 5. State

`.text 0x7d0`, `.rodata` `0x7d0`–`0x7e0` (the module-name string), `.data`
`0x7e0`–`0x7f0` (the record the `.iopmod` header points at: a relocated pointer
to the name, the version word, two zeros — loader-facing only, nothing in
`.text` reads it), `.bss` `0x7f0`–`0x950`:

| Off | Field |
|---|---|
| `+0x0` | event flag id |
| `+0x4` | start-dispatch counter, read only by the first-time test |
| `+0x8` | start list head `{next, prev}` |
| `+0x10` | end list head |
| `+0x18` | free list head |
| `+0x20` | 16 pool nodes of `{next, prev, priority, handler, arg}` |

The pool ends exactly at the end of `.bss`. Ordinal 3 hands out the base, so a
caller holding it can read the counter and walk the lists.

## 6. The interrupt lines, and who owns them

`I_STAT`/`I_MASK` bit **0** is the start-of-vblank edge and bit **11** the end
(`IOP_IRQ_VBLANK`/`IOP_IRQ_EVBLANK` [header]). `VBLANK` registers `mode = 1`
handlers on both and enables both, which agrees with the boot-time
`I_MASK = 0x1080D` `docs/analysis/44` recorded.

`INTRMAN` allows one handler per line, so ownership is exclusive, and two
sweeps over the whole extracted archive confirm `VBLANK` is the only claimant:
reading `$a0` at every call site of `intrman` ordinals 4 and 6 across every
module that imports them yields lines `0x2`, `0x9`, `0xd`, `0x11`, `0x24`,
`0x28`, `0x2a`, `0x2b` and the timer lines — and `0` and `0xb` only from
`VBLANK`; and no module outside `INTRMAN` itself writes the handler table's
slots for those two lines directly. Sharing happens one level up, in `VBLANK`'s
own priority lists.

So what a rebuild must wire is: a `VBLANK` module registering on lines 0 and
`0xb`, an `INTRMAN` that delivers those two `I_STAT` bits on the interrupt
stack, and a machine that raises them at the start and end of vertical blank.
The EE's own INTC vblank lines are unrelated.

## 7. What this pins for the rebuild

- Callbacks are invoked from the interrupt dispatch, on the interrupt stack.
- Ordinal 9 answers exactly 0 for a registered handler.
- A callback answering 0 is unregistered on the spot; nonzero keeps it.
- Priority ascending, ties FIFO; identity is (list, handler).
- Both line handlers answer nonzero, so the lines stay enabled.
- The flag is `EA_MULTI`, initial 0, with §4's pulse/level split, and
  `WaitEventFlag` must accept a null result pointer.
- The entry is non-resident only when library registration fails.

## 8. Unresolved

- **Who consumes bit `0x200`** of the system status flag. All four reference
  importers of `thbase` 41 were checked and none waits on it; a disc-loaded
  module or SDK library code may. A rebuild should set it for fidelity.
- **What `0x650` was for** — an unreferenced list test, dead in this build.
- **`PADMAN`'s callback argument record** was read only as far as its vblank
  callback needs.
