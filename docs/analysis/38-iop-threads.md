# IOP Threads: THREADMAN's Seven Libraries, Read for Behaviour

`docs/analysis/09` found `THREADMAN`'s shape — seven exported libraries, two
registration APIs, the `stdio` supersession timing — but stopped at structure.
`docs/analysis/37` then read the IOP's interrupt-return path far enough to
name the two hooks `INTRMAN` calls back through, `SetNewCtxCb`/
`SetShouldPreemptCb` (ordinals 28/30), and to show that `THREADMAN` is the
only module that imports them. This document is the other half: what
`THREADMAN` actually does with those hooks, and what the seven libraries'
functions do internally — the thread record, the ready queues, the tagged-
pointer thread ID, the wait-object model shared by events and semaphores, and
the two threads `THREADMAN`'s own entry creates at boot.

```sh
python3 tools/romdir.py assets/SCPH-50000.bin --extract <outdir>   # outside the repo
python3 tools/irxinfo.py <outdir>/THREADMAN --exports --imports
python3 tools/irxinfo.py <outdir>/THREADMAN --dump-load <outdir>/THREADMAN.text
python3 tools/romdis.py <outdir>/THREADMAN.text --cpu iop --vma 0 --range START END
```

All addresses below are `THREADMAN.text` offsets (module vaddr, base 0 —
**not** raw file offsets; `--dump-load` must run first or every address is
wrong by the ELF header's size). ps2sdk's `iop/system/threadman/include/
{thbase,thevent,thsemap,thmsgbx}.h` and `iop/kernel/include/kerr.h` were
fetched to name ordinals and error codes; every such name is marked
`[header]`. `intrman.h` and `sysclib.h`/`sysmem.h`/`timrman.h`/`heaplib.h`/
`loadcore.h` were also consulted, for the same reason, to name THREADMAN's own
*imports* — those are marked `[header]` too. The binary is the authority
throughout; several names below turn out to be present in the export table
but **not implemented** by this firmware, found only by reading the body.

## 0. Cross-references established, not re-derived

- `docs/analysis/37` §2.1/§2.4/§3.5 already read `INTRMANP`'s side of the
  reschedule hook: `QueryIntrContext()` is `QueryIntrStack($sp)` — a check of
  whether `$sp` falls inside the fixed `[0x1290,0x1a90)` interrupt stack, not
  a flag — and the interrupt-return path calls `ShouldPreemptCb` (skipped
  entirely for a nested interrupt) then, if it answers yes, tops up the
  interrupted context's saved registers and calls `NewCtxCb`, using its
  return value as the new `$sp`. This document does not re-derive any of
  that; §4 below reads what `THREADMAN` registers at those two cells and what
  its own code does when called back through them.
- `docs/analysis/09` already covers the seven-library export shape and the
  two `loadcore` registration APIs (versioned ordinal 6 vs. pinned ordinal
  10). Not repeated here except where a specific address is needed.

## 1. Ordinal tables

### 1.1 `thbase` (`0x5f80`, v1.01, 42 entries)

| Ord | Addr | Name [header] | Notes |
|---|---|---|---|
| 0 | `0x10` | module entry | §5 |
| 1–3 | `0x61dc` | reserved | shared `syscall 0xfc` trap stub (a bare `syscall`/`jr $ra` at `0x61dc`, common to every reserved slot in all seven tables) |
| 3 | — | `GetThreadmanData` | **not at `0x61dc`** — see below |
| 4 | `0xc5c` | `CreateThread` | §2 |
| 5 | `0xe10` | `DeleteThread` | §2 |
| 6 | `0x1028` | `StartThread` | §2 |
| 7 | `0x1118` | `StartThreadArgs` | §2 |
| 8 | `0x123c` | `ExitThread` | §2 |
| 9 | `0x12d0` | `ExitDeleteThread` | **stub**, always `-1` (§2) |
| 10 | `0x12d8` | `TerminateThread` | §2 |
| 11 | `0x1434` | `iTerminateThread` | §2 |
| 12 | `0x1558` | `DisableDispatchThread` | **stub**, always `-1` |
| 13 | `0x1560` | `EnableDispatchThread` | **stub**, always `-1` |
| 14 | `0x1568` | `ChangeThreadPriority` | §2 |
| 15 | `0x1738` | `iChangeThreadPriority` | not separately traced; same shape as 14 |
| 16 | `0x1874` | `RotateThreadReadyQueue` | not separately traced |
| 17 | `0x1974` | `iRotateThreadReadyQueue` | not separately traced |
| 18 | `0x1a6c` | `ReleaseWaitThread` | §2 |
| 19 | `0x1bac` | `iReleaseWaitThread` | §2 |
| 20 | `0x1ca8` | `GetThreadId` | §2 |
| 21 | `0x1d78` | `CheckThreadStack` | §2 |
| 22 | `0x1f00` | `ReferThreadStatus` | §2 |
| 23 | `0x1fac` | `iReferThreadStatus` | §2 |
| 24 | `0x200c` | `SleepThread` | §2 |
| 25 | `0x20e4` | `WakeupThread` | §2 |
| 26 | `0x2204` | `iWakeupThread` | §2 |
| 27 | `0x22dc` | `CancelWakeupThread` | §2 |
| 28 | `0x2378` | `iCancelWakeupThread` | §2 |
| 29 | `0x23e4` | `SuspendThread` | **stub**, always `-1` |
| 30 | `0x23ec` | `iSuspendThread` | **stub**, always `-1` |
| 31 | `0x23f4` | `ResumeThread` | **stub**, always `-1` |
| 32 | `0x23fc` | `iResumeThread` | **stub**, always `-1` |
| 33 | `0x2444` | `DelayThread` | §2 |
| 34 | `0x5738` | `GetSystemTime` | not traced |
| 35 | `0x5764` | `SetAlarm` | called internally by `DelayThread` (§2) |
| 36 | `0x5914` | `iSetAlarm` | not traced |
| 37 | `0x5b04` | `CancelAlarm` | called internally by `TerminateThread`/`ReleaseWaitThread` (§2) |
| 38 | `0x5be0` | `iCancelAlarm` | called internally by `iTerminateThread`/`iReleaseWaitThread` |
| 39 | `0x5598` | `USec2SysClock` | called internally by `DelayThread` |
| 40 | `0x5614` | `SysClock2USec` | not traced |
| 41 | `0x254c` | `GetSystemStatusFlag` | two instructions: `return *0x6c7c` |

`GetThreadmanData` (ordinal 3) is a genuine three-instruction function at
module offset **`0x0`** — the very first bytes of the text segment, *before*
the module entry at `0x10` — returning the literal address `0x67d0`:

```
0:   lui  $2,0
4:   addiu $2,$2,0x67d0
8:   jr   $ra
```

`0x67d0` turns out to be the base of THREADMAN's entire `.bss` (§4): the
current-thread pointer, the pending-next-thread pointer, the 4-word ready
bitmap, the 128 ready-queue heads, and the boot-time anchors all live at
fixed offsets from it. (An early pass at this document, before switching to
the `--dump-load`-produced text, misread ordinal 3's export-table word `0x0`
as "unimplemented" — it is not; `0x0` is simply where the text segment
starts. `--dump-load` is not optional for this module.)

**Seven confirmed stubs.** `ExitDeleteThread`, `DisableDispatchThread`,
`EnableDispatchThread`, `SuspendThread`, `iSuspendThread`, `ResumeThread` and
`iResumeThread` are each a bare two-instruction function, `jr $ra` / `addiu
$2,$zero,-1` (`KE_ERROR` [header]) — present in the export table (so an
importer's bind succeeds) but doing nothing. This is the "verify by body, not
by header" case the task set out to find: ps2sdk's headers describe all seven
as working calls; this v1.01 THREADMAN answers every one of them with
`KE_ERROR` unconditionally.

### 1.2 `thevent` (`0x6044`, v1.01, 15 entries)

| Ord | Addr | Name [header] |
|---|---|---|
| 0–3 | `0x61dc` | reserved |
| 4 | `0x2560` | `CreateEventFlag` |
| 5 | `0x2660` | `DeleteEventFlag` |
| 6 | `0x2788` | `SetEventFlag` |
| 7 | `0x2960` | `iSetEventFlag` |
| 8 | `0x2abc` | `ClearEventFlag` |
| 9 | `0x2b54` | `iClearEventFlag` |
| 10 | `0x2bd0` | `WaitEventFlag` |
| 11 | `0x2db0` | `PollEventFlag` |
| 12 | `0x61dc` | reserved (header skips this ordinal too — a clean corroboration) |
| 13 | `0x2f4c` | `ReferEventFlagStatus` |
| 14 | `0x2fe4` | `iReferEventFlagStatus` |

### 1.3 `thsemap` (`0x609c`, v1.01, 13 entries)

| Ord | Addr | Name [header] |
|---|---|---|
| 0–3 | `0x61dc` | reserved |
| 4 | `0x3060` | `CreateSema` |
| 5 | `0x3164` | `DeleteSema` |
| 6 | `0x328c` | `SignalSema` |
| 7 | `0x3374` | `iSignalSema` |
| 8 | `0x3444` | `WaitSema` |
| 9 | `0x35b4` | `PollSema` |
| 10 | `0x61dc` | reserved (header skips it too) |
| 11 | `0x36a4` | `ReferSemaStatus` |
| 12 | `0x373c` | `iReferSemaStatus` |

### 1.4 The other four libraries, briefly

- **`thmsgbx`** (`0x60ec`, 13 entries, `0x37c0`–`0x3fa4`): `CreateMbx`
  (`0x37c0`) follows the exact CreateThread/CreateSema/CreateEventFlag shape
  — pool tag `0x7f04`, record size `0x28`, the same tagged-pointer ID scheme,
  its own generation counter at `0x6c42`. Send/receive semantics not traced.
- **`thfpool`** (`0x613c`, 13 entries, `0x4830`–`0x4f94`): `CreateFpl`
  (`0x4830`) validates `attr & ~0x202` and a block-count/block-size sanity
  check before rounding size and suspending interrupts; pool tag not
  confirmed in the traced window.
- **`thvpool`** (`0x618c`, 13 entries, `0x4020`–`0x47b0`): `CreateVpl`
  (`0x4020`), pool tag `0x7f05`, record size `0x28`, rejects a zero pool-size
  field with `KE_ILLEGAL_MEMSIZE` (`-427`).
- **`thrdman`** (`0x6230`, v1.02, 4 entries): **all four ordinals share the
  literal same address**, `0x6258` — confirmed a bare `jr $ra` / `nop`, not
  even the reserved-slot `syscall 0xfc` trap. Declared, unimplemented.

Every `Create*` above allocates from the *same* internal fixed-block pool —
`jal 0x5ef0(tag, size)` — and every object type gets its own tag: `0x7f01`
threads, `0x7f02` semaphores, `0x7f03` event flags, `0x7f04` message boxes,
`0x7f05` variable pools (fixed pools' tag wasn't reached in the traced
window). `0x5f50` is the matching free. Every ID-taking function reconstructs
its pointer and checks the tag before trusting anything else in the record
(§2.1).

## 2. The thread model

### 2.1 The record, and the ID that is not a small integer

`CreateThread`'s validation and allocation (`0xc5c`–`0xe0c`) pin the `iop_
thread_t` parameter block [header] field-for-field by the offsets it reads
from `$a0`:

```c
struct iop_thread_t { u32 attr; u32 option; void (*thread)(void*); u32 stacksize; u32 priority; };
//                     +0x0      +0x4        +0x8                   +0xc          +0x10
```

and validates, in order (each failure resumes interrupts and returns
immediately):

```
c6c:  jal  0x6334              # intrman ord.23, QueryIntrContext [header]
c74:  bnez $2,0xdfc             # nonzero (on the interrupt stack) -> KE_ILLEGAL_CONTEXT (-100)
c88:  and  $2,attr,0x1cfffff7   # any bit outside 0xe3000008 -> KE_ILLEGAL_ATTR (-401)
ca0:  sltiu $2,priority-1,0x7e  # priority not in [1,126] -> KE_ILLEGAL_PRIORITY (-403)
cb4:  andi $2,entry,3           # entry not 4-byte aligned -> KE_ILLEGAL_ENTRY (-402)
cc8:  sltiu $2,stacksize,0x130  # stacksize < 0x130 (304) -> KE_ILLEGAL_STACK_SIZE (-404)
```

`0xe3000008` accepts `TH_ASM`(`0x01000000`)/`TH_C`(`0x02000000`)/`TH_UMODE`
(`8`) [header] plus bits 31/30/29, which thbase.h does not name — and
**rejects** `TH_NO_FILLSTACK`(`0x00100000`)/`TH_CLEAR_STACK`(`0x00200000`)
[header] outright: any caller setting either bit gets `KE_ILLEGAL_ATTR`. Both
flags are declared by ps2sdk's header but this v1.01 `CreateThread` has never
heard of them.

After validation: `CpuSuspendIntr`, pop a `0x50`-byte record (tag `0x7f01`)
from the internal pool, round `stacksize` up to the next `0x100` and allocate
it via `sysmem`'s `AllocSysMemory` [header] — **writing the rounded size back
into the caller's own `iop_thread_t.stacksize`**, a real side effect on the
input struct. Record fields, all confirmed by the store instructions:

| Off | Width | Field | Set by |
|---|---|---|---|
| `+0x0`/`+0x4` | 32/32 | generic list link (next/prev) | whichever list currently owns the record: a ready-queue head, a wait-object's wait list, or the dormant/all-threads list |
| `+0x8` | u16 | pool tag (`0x7f01` for threads) | allocator |
| `+0xa` | u16 | rolling creation counter, mod `0x3f` | `CreateThread`, from a global at `0x6c3c` |
| `+0xc` | u8 | state (`THS_*` [header]) | every state transition |
| `+0xe` | u16 | **current** priority | `CreateThread` (from input `priority`), rewritten by `ChangeThreadPriority` |
| `+0x10` | 32 | pointer to the saved-register context frame | primed by `StartThread`/its helper `0xf40` (§4), not by `CreateThread` |
| `+0x1c` | u16 | wait-type (`TSW_*` [header]) | set on block, cleared on wake |
| `+0x1e` | u16 | wakeup count | `WakeupThread`/`SleepThread`/`CancelWakeupThread` |
| `+0x20` | 32 | wait-object pointer (semaphore/event-flag record) or scratch | set on block |
| `+0x24` | 32 | permanent link into the global "all threads" list at `0x6c60` | `CreateThread`, once |
| `+0x2e` | u16 | **initial** priority | `CreateThread` (from input `priority`) |
| `+0x38` | 32 | entry point | `CreateThread` (from input `thread`) |
| `+0x3c`/`+0x40` | 32/32 | stack base / stack size (rounded) | `CreateThread` |
| `+0x44` | 32 | saved `$gp` | `CreateThread` (the caller's own `$gp`, i.e. THREADMAN's) |
| `+0x48`/`+0x4c` | 32/32 | attr / option | `CreateThread` (verbatim from input) |

Total `0x50` bytes, matching the pool allocation. `ReferThreadStatus`'s copy
routine (§2.3) independently confirms every offset above by copying it
field-for-field into `iop_thread_info_t` [header].

**The thread ID is a tagged pointer, not an index.** `CreateThread`'s tail:

```
de4:  lhu  $3,0xa($16)          # +0xa: creation counter
de8:  sll  $2,$16,0x5           # record-pointer << 5
dec:  andi $3,$3,0x3f           # counter mod 64
df0:  sll  $3,$3,0x1
df4:  or   $2,$2,$3
df8:  ori  $2,$2,0x1            # id = (record<<5) | ((counter&0x3f)<<1) | 1
```

Because pool-allocated records are at least 4-byte aligned, `record<<5`'s low
two result bits are always zero, so bits 5–6 of the id are free for the
counter's own top two bits without colliding with the pointer — the encoding
is deliberate, not sloppy. Every id-taking function reverses it identically:

```
srl  $2,$id,0x7
sll  $ptr,$2,0x2                # ptr = (id>>7)<<2  (discards the 7 tag/counter bits, re-aligns)
lhu  $3,0x8($ptr) ; bne $3,<tag>,ERROR       # KE_UNKNOWN_THID (-407) on tag mismatch
sra  $2,$id,0x1 ; andi $2,$2,0x3f
lhu  $3,0xa($ptr) ; andi $3,$3,0x3f ; bne $2,$3,ERROR   # generation mismatch, same error
```

The generation check means a stale ID pointing at a freed-and-reused record
slot is rejected rather than silently hitting the wrong thread — the mod-64
counter wraps, so this is a probabilistic guard, not an absolute one, exactly
like a classic tagged-handle / ABA-guard scheme. `thevent`/`thsemap`/
`thmsgbx`/`thvpool` all use the identical `(ptr<<5)|((gen&0x3f)<<1)|1`
construction with their own per-type tag and their own generation counter
global (`0x6c3e` semaphores, `0x6c40` event flags, `0x6c42` message boxes;
threads' is `0x6c3c` — four adjacent halfwords, one small table).

### 2.2 States, priorities, ready queues

States (`THS_*` [header], stored as a byte at `+0xc`): `0x10`=DORMANT (fresh
or exited/terminated), `2`=READY, `1`=RUN, `4`=WAIT, `8`=SUSPEND, `0xC`=WAIT|
SUSPEND — `SuspendThread`/`ResumeThread` being stubs (§1.1), `0x8`/`0xC` are
never actually reached by this firmware's own code, only documented by the
header.

**Ready queues**: 128 doubly-linked circular list heads at absolute
`0x67ec`, stride `8` (`0x67ec + priority*8`), one per priority `0`–`127`,
initialised at boot (§5). A parallel 4-word (128-bit) occupancy bitmap sits
at `0x67d8`; the enqueue primitives (`0x47c`, `0x4e0` — two call sites with
identical bodies, one for "enqueue new" one for "re-enqueue outgoing",
confirmed byte-identical) set the priority's bit; the dequeue primitive
(`0x548`) clears it once a list empties. `FindLowestReadyPriority` (`0x658`)
scans the four bitmap words and returns the lowest set bit's global index, or
`128` if none are set — the numerically-lowest priority value is the
highest-priority thread, matching `thbase.h`'s `HIGHEST_PRIORITY=1`/
`LOWEST_PRIORITY=126` convention. `ChangeThreadPriority`/`iChangeThreadPriority`
(`0x1568`/`0x1738`) write `+0xe` and, if the target is currently READY,
dequeue-then-re-enqueue at the new priority (not separately re-disassembled
past confirming the same primitives are called).

### 2.3 The lifecycle operations

**`StartThread(id, arg)`** (`0x1028`) / **`StartThreadArgs(id, argc, argp)`**
(`0x1118`): id-reconstruct + tag/generation check, `state==DORMANT` else
`KE_NOT_DORMANT` (`-414`). Computes a `0xb8`-byte context frame near the top
of the thread's own stack (`stackBase + (stackSize&~3) - 0xb8`), zeroes it,
stores it into the record's `+0x10`, then calls the shared finalizer `0xf40`
(below) to prime it and enqueue the thread. `StartThread` stores `arg` into
the frame's `+0x10`; `StartThreadArgs` additionally `memcpy`s `argp` (via
`sysclib` ordinal 12 [header]) into a region past the frame and stores the
resulting pointer/count.

**`0xf40` (the "prime and go" finalizer, called by `StartThread`/`Start
ThreadArgs`, also used by `TerminateThread`'s counterpart `0x3bb8`-equivalent
reset)**: sets current priority `+0xe` from initial `+0x2e`; clears wait
fields; primes the context frame — `+0x74`/`+0x78` (a stack-pointer-shaped
field, both set to `frame+0x98`, past the register-save area), `+0x7c`
(**set to `0x123c` — `ExitThread`'s own address**, i.e. `$ra` for the new
thread's very first instruction: a function that returns normally lands
directly in `ExitThread`, not undefined behaviour), `+0x70` (`$gp`, copied
from the record's `+0x44`), `+0x88` (a primed status word, `0x404 |
(attr&0xf0000000) | (attr&8)` — the fixed template plus the record's
top-three attr bits and the `TH_UMODE` bit, almost certainly the initial COP0
`Status` the low-level restore writes on first dispatch, though the exact
bit semantics were not independently checked against the R3000's field
layout), `+0x8c` (EPC — the entry point, from `+0x38`). Then `jal 0x5e90`
(mark "started"/link generic) and `jal 0x5b8` — the enqueue-and-maybe-
preempt-immediately primitive (§4).

**`ExitThread`** (`0x123c`, no `i`-form, never returns to its own caller):
sets `state=DORMANT`, links onto the `0x6c60` dormant/all-threads list,
clears the pending-next global `0x67d4`, then issues the reschedule trap
(`jal 0x46c`, §4) with all four arguments zero. If control ever *does* return
here — it should not, by construction — the code prints a Kprintf warning and
executes `break 0x1`, then loops back and retries: a defensive assertion, not
normal behaviour.

**`ExitDeleteThread`** (`0x12d0`): the two-instruction stub (§1.1) — not
implemented in this firmware.

**`TerminateThread`/`iTerminateThread`** (`0x12d8`/`0x1434`): id `0`
rejected outright (`KE_ILLEGAL_THID`, `-406`) — and, distinct from the EE
analogue (`docs/analysis/33`), a target whose reconstructed pointer equals
the *caller's own current thread* is also rejected the same way, checked by
direct pointer comparison against `0x67d0` before the tag/generation check
even runs. `state==DORMANT` -> `KE_DORMANT` (`-413`). Otherwise: unconditional
unlink (`jal 0x5e90` — the same primitive whether the thread was READY or
WAIT, since both use the same `+0x0`/`+0x4` link node), and if it was WAIT
with `waitType==TSW_DELAY`(`2`) [header], cancels the pending timeout alarm
(`jal 0x5b04`/`0x5be0` — `CancelAlarm`/`iCancelAlarm`, §2.4) before finalizing
to DORMANT via the same `0x6c60`-list insert `ExitThread` uses. Returns `0`
on success — **not the id**, unlike the EE analogue.

**`ReleaseWaitThread`/`iReleaseWaitThread`** (`0x1a6c`/`0x1bac`): id
reconstruct, `state==DORMANT` -> `KE_DORMANT`; `state!=WAIT` -> `KE_NOT_WAIT`
(`-416`). On a genuine WAIT target: forces the blocked call's own return slot
(`*(record+0x10)+0x8`, the frame's cached `$v0` position) to `KE_RELEASE_
WAIT` (`-418`) — confirmed against `kerr.h` exactly — unlinks, sets `READY`,
cancels a pending `TSW_DELAY` alarm the same way `TerminateThread` does, and
decrements the owning wait-object's waiter count (`+0x10` of whatever `+0x20`
points at) for the five "real" wait-object types (`TSW_SEMA`/`EVENTFLAG`/
`MBX`/`VPL`/`FPL`, values 3–7) — `TSW_SLEEP`(1) and `TSW_DELAY`(2) have no
such object and are explicitly excluded from the decrement. Finishes with
`jal 0x5b8` (immediate-preempt-aware enqueue).

**`GetThreadId`** (`0x1ca8`): requires `QueryIntrContext()==0` (ordinary
thread context) else `KE_ILLEGAL_CONTEXT`; on success, **recomputes** the
tagged ID fresh from `*0x67d0` and its generation counter — it does not cache
or return a stored id.

**`ReferThreadStatus`/`iReferThreadStatus`** (`0x1f00`/`0x1fac`): `id==0`
means self (`*0x67d0` directly, no tag check needed); otherwise the usual
reconstruct/tag/generation check, `KE_UNKNOWN_THID` on mismatch. Both call a
shared copy helper (`0x1da8`) that `memset`s the destination to zero (`0x44`
= 68 bytes, exactly `sizeof(iop_thread_info_t)` [header]) then copies,
confirmed field-for-field: `status<-+0xc`(byte, widened), `currentPriority<-
+0xe`, `initPriority<-+0x2e`, `entry<-+0x38`, `stack<-+0x3c`, `stackSize<-
+0x40`, `gpReg<-+0x44`, `attr<-+0x48`, `option<-+0x4c`; when `status==WAIT`,
also `waitType<-+0x1c` and a `waitId` derived from `+0x20` (the raw value for
`TSW_DELAY`, else that pointer's own tagged public id, reconstructed the same
`(ptr<<5)|...` way — wait-objects have publicly-visible ids too); always
`wakeupCount<-+0x1e`. The final `regContext` field's exact source condition
(dormant vs. running vs. other) was traced but not fully disentangled from
the surrounding branches — reported as observed, not claimed complete.

**`SleepThread`** (`0x200c`, no `i`-form): if `wakeupCount(+0x1e)>0`,
decrements it and **returns `0` without blocking** (note: not the caller's
own id, unlike the EE analogue's `SleepThread`). Otherwise sets
`state=WAIT`, `waitType=TSW_SLEEP`(`1`) [header], links onto the sleeping-
threads list at `0x67d0+0x480`, clears `0x67d4`, and issues the reschedule
trap — a genuine block.

**`WakeupThread`/`iWakeupThread`** (`0x20e4`/`0x2204`): id `0` rejected
(`iWakeupThread` — unlike `WakeupThread` — additionally clears `0x67d4`
unconditionally on its success path). `state==DORMANT` -> `KE_DORMANT`.
`state==WAIT && waitType==TSW_SLEEP`: unlink, `READY`, enqueue (`0x5b8` for
the non-`i` form, the deferred `0x47c` for the `i`-form — consistent with
`i`-forms never triggering an immediate context switch of their own). Any
other reachable state: **increment** `wakeupCount` and return `0` — the
pending-wakeup counter `SleepThread`'s fast path consumes.

**`CancelWakeupThread`/`iCancelWakeupThread`** (`0x22dc`/`0x2378`): **does**
accept id `0` as self (unlike `WakeupThread`), tag/generation check
otherwise; returns the *previous* `wakeupCount` and resets it to `0`, no
state check at all.

**`DelayThread(usec)`** (`0x2444`) — the timer link the task asked about:

```
246c:  jal 0x5598($4=usec, $5=&localSysClock)     # thbase ord.39, USec2SysClock [header]
24d4:  jal 0x5764($4=&localSysClock, $5=0x2404, $6=self)   # thbase ord.35, SetAlarm [header]
```

`SetAlarm(clock, callback, arg)` is called with a **fixed internal callback
address, `0x2404`**, and the calling thread's own record as `arg` — this is
not traced further as `SetAlarm`'s own body (thbase ordinal 35, `0x5764`,
which presumably reaches `timrman`'s `AllocHardTimer`/`SetTimerCompare`
[header] — THREADMAN imports exactly those, plus `SetTimerCounter`/
`SetTimerMode`/`GetTimerCounter`/`GetHardTimerIntrCode`, seven `timrman`
ordinals total, confirming a real hardware timer backs this, not traced
past the import list). On success: `state=WAIT`, `waitType=TSW_DELAY`(`2`)
[header] (matching thbase.h exactly and confirming the `TerminateThread`/
`ReleaseWaitThread` special-casing of value `2`, §2.3), links onto a third
list at `0x67d0+0x488` (distinct from `SleepThread`'s `+0x480`), issues the
reschedule trap. **`0x2404` itself is the alarm's expiry callback** — reading
it closes the loop:

```
2404:  jal 0x5e90($4=arg)      # unlink the woken thread from the delay list
2420:  jal 0x47c($4=thread)    # ready-enqueue, deferred form (alarm fires from a callback/interrupt context)
2434:  sw $zero,0x67d4($0)     # clear pending-next
```

— exactly the counterpart `TerminateThread`/`ReleaseWaitThread` disarm via
`CancelAlarm(0x2404, thread)`/`iCancelAlarm` when killing or force-waking a
thread before its delay expires.

## 3. Event flags and semaphores

Both wait-object libraries reuse thread-record fields directly as their wait
nodes — a blocked thread's own `+0x1c`(waitType)/`+0x20`(back-pointer to the
wait object)/`+0x28`(wanted bits, event flags only)/`+0x2c`(mode, event flags
only) fields *are* the queue entry; there is no separate wait-block
allocation. Both share `thbase`'s `QueryIntrContext`/`CpuSuspendIntr`/
`CpuResumeIntr` bracketing, the same internal pool allocator, and the same
tagged-pointer ID scheme (§2.1).

### 3.1 Event flags (`iop_event_t{attr,option,bits}` [header])

Record, `0x28` bytes, pool tag `0x7f03`:

| Off | Field | Off | Field |
|---|---|---|---|
| `+0x8`/`+0xa` | pool tag / generation | `+0x1c` | option |
| `+0xc` | attr (only `EA_MULTI`=`2` [header] legal; any other bit -> `KE_ILLEGAL_ATTR`) | `+0x20` | currBits |
| `+0x10` | waiter count | `+0x24` | initBits |
| `+0x14` | wait-list head (self-referencing sentinel when empty) | | |

`CreateEventFlag` (`0x2560`): context/attr gate, pool alloc (`KE_NO_MEMORY`
on failure), fields as above, linked onto a global creation list.

`SetEventFlag`/`iSetEventFlag` (`0x2788`/`0x2960`, identical logic, `i`-form
skips only the suspend/resume bracket — both still call `QueryIntrContext`,
§4): `bits==0` short-circuits before touching `currBits` at all. Otherwise
`currBits |= bits`, then walks the wait list; each waiter's condition is
`(currBits & wantBits) != 0` for `WEF_OR`(`mode&1`) or `(currBits & wantBits)
== wantBits` for AND (`mode&1==0`); a satisfied waiter has `currBits` written
through a cached result-pointer at its own context `+0x8`, is unlinked, and
readied. **`mode & WEF_CLEAR`(`0x10`) resets `currBits` to `0` entirely** —
not just the bits that were waited on — confirmed identical in `Set*` and in
`WaitEventFlag`/`PollEventFlag`'s own immediate-success path.

`ClearEventFlag`/`iClearEventFlag` (`0x2abc`/`0x2b54`): `currBits = currBits
& bits` — a raw AND with the caller's argument as a **keep-mask**, not a
clear-mask (no complement instruction anywhere in either body). Never
touches the wait list (clearing bits can't newly satisfy anyone).

`WaitEventFlag(id, bits, mode, &result)` (`0x2bd0`): `mode & ~0x11 != 0` ->
`KE_ILLEGAL_MODE` (`-405`); `bits==0` -> `KE_EVF_ILPAT` (`-423`);
`!(attr&EA_MULTI) && waiterCount>0` -> `KE_EVF_MULTI` (`-422`). If already
satisfied: write `*result`, apply `WEF_CLEAR` if requested, return `0`, no
block. Otherwise: `state=WAIT`, `waitType=TSW_EVENTFLAG`(`4`) [header]
(matches thbase.h exactly), `+0x20`=event record, `+0x28`=bits, `+0x2c`=mode,
link onto the event's wait list, issue the reschedule trap.

`PollEventFlag` (`0x2db0`): byte-identical to `WaitEventFlag` through the
satisfied-immediately path; on not-satisfied, returns `KE_EVF_COND` (`-421`)
directly instead of blocking.

`ReferEventFlagStatus`/`iReferEventFlagStatus` (`0x2f4c`/`0x2fe4`): shared
copy helper, confirmed field-for-field against `iop_event_info_t` [header]:
`attr<-+0xc`, `option<-+0x1c`, `initBits<-+0x24`, `currBits<-+0x20`,
`numThreads<-+0x10`.

### 3.2 Semaphores (`iop_sema_t{attr,option,initial,max}` [header])

Record, `0x2c` bytes, pool tag `0x7f02`:

| Off | Field | Off | Field |
|---|---|---|---|
| `+0x8`/`+0xa` | pool tag / generation | `+0x1c` | option |
| `+0xc` | attr (`~0x101` must be zero — `SA_THFIFO`(0)/`SA_THPRI`(1)/`SA_IHTHPRI`(0x100) [header] legal; the gate is `addiu $3,$zero,-0x102; and; bnez`, and `-0x102` as a 32-bit word is `0xfffffefe`, which is `~0x101` and not `~0x102` — reading the immediate as the mask would reject `SA_THPRI`, the one attribute this same document's own wait-ordering note depends on) | `+0x20` | current count |
| `+0x10` | numWaitThreads | `+0x24` | max |
| `+0x14` | wait-list head (self-referencing sentinel) | `+0x28` | initial |

`CreateSema` (`0x3060`): context/attr gate, pool alloc, fields as above,
linked onto a global list via the same generic `0x5ec8` insert `thbase` uses
for its own "all threads" list — confirming that primitive is generic, not
thread-specific.

`SignalSema`/`iSignalSema` (`0x328c`/`0x3374`): fast path
(`numWaitThreads==0`): `if (current<max) current++`; **if `current>=max` the
function returns `0` without incrementing and without producing `KE_SEMA_
OVF`** — the header names that error code, but this firmware silently caps
at `max` instead of returning it. Wake path (`numWaitThreads!=0`): pop the
first waiter, decrement, unlink, ready, hand off via `0x5b8`(non-`i`)/`0x47c`
(`i`); the woken thread's cached return slot is zeroed so `WaitSema` sees
`0`.

`WaitSema` (`0x3444`): fast path `if(current>0) current--`, return.
Block path (`current<=0`): `state=WAIT`, `waitType=TSW_SEMA`(`3`) [header],
`+0x20`=sema record, increments `numWaitThreads`; **`SA_THPRI` is real and
implemented** — the code branches on `attr&1`: FIFO (default) appends at the
tail, `SA_THPRI` walks the existing list comparing each waiter's `+0xe`
(current priority) and inserts before the first strictly-worse entry — a
genuine priority-ordered insertion, not a no-op despite the flag's obscurity.
Issues the reschedule trap.

`PollSema` (`0x35b4`): identical validation; on `current<=0` returns
`KE_SEMA_ZERO` (`-419`) directly.

`ReferSemaStatus`/`iReferSemaStatus` (`0x36a4`/`0x373c`): shared copy helper,
confirmed against `iop_sema_info_t` [header]: `attr<-+0xc`, `option<-+0x1c`,
`initial<-+0x28`, `max<-+0x24`, `current<-+0x20`, `numWaitThreads<-+0x10`.

Return codes confirmed against `kerr.h` for both libraries: `KE_ILLEGAL_
CONTEXT`(-100), `KE_NO_MEMORY`(-400), `KE_ILLEGAL_ATTR`(-401), `KE_ILLEGAL_
MODE`(-405, event flags only), `KE_UNKNOWN_SEMID`(-408)/`KE_UNKNOWN_EVFID`
(-409), `KE_SEMA_ZERO`(-419), `KE_EVF_COND`(-421), `KE_EVF_MULTI`(-422),
`KE_EVF_ILPAT`(-423), `KE_WAIT_DELETE`(-425, forced into a deleted object's
waiters' return slots by both `DeleteEventFlag` and `DeleteSema`).

## 4. The dispatcher

### 4.1 Two globals decide everything

`0x67d0` = pointer to the *current* thread's record; `0x67d4` = pointer to
the thread that *should* be running once the current interrupt/syscall
returns ("pending-next"). Nothing else drives the switch decision. The two
`INTRMAN` hooks `docs/analysis/37` §3.5 found `THREADMAN` registering at boot
(§5) are both trivial reads of these two cells:

`SetShouldPreemptCb`'s target, `0xbc4`:

```
bc4:  lui $3,0 ; addiu $3,$3,0x67d4
bcc:  lw  $2,0x0($3)      # *0x67d4
bd0:  lw  $3,-0x4($3)     # *0x67d0  (0x67d4-4 == 0x67d0)
bd8:  xor $2,$2,$3
be0:  sltu $2,$zero,$2    # return (pending != current)
```

Seven instructions: "should we preempt" is exactly "does anyone disagree with
`0x67d0` about who should be running." Every operation in §2/§3 that decides
a switch is warranted — `WakeupThread` finding a sleeper, `SignalSema`
finding a waiter, a fired `DelayThread` alarm, `TerminateThread` killing the
running thread — does its work by writing a new value into `0x67d4` (or
leaving it equal to `0x67d0` when nothing changed); nothing calls into the
dispatcher's decision logic directly.

`SetNewCtxCb`'s target, `0x940`, is what actually resolves a mismatch. Per
`docs/analysis/37` §3.5, `INTRMAN` calls it with the interrupted thread's
just-completed saved-context frame in `$a0`, after `ShouldPreemptCb` answered
yes, and uses its return value as the new `$sp`. `0x940`:

- Bumps a switch-count statistic (`0x6c80`) and, gated by a debug-flags word
  at `0x67e8`, optionally `Kprintf`s a trace of the outgoing/incoming
  threads.
- Stores the incoming `$a0` (the outgoing thread's saved frame) into `*0x67d0
  + 0x10` — the record field established in §2.1 as "pointer to my saved
  context frame." This is the moment a thread's `+0x10` is actually kept
  current: it is not maintained continuously, only refreshed here, at the
  point a real switch happens.
- Reads elapsed hardware-timer ticks (`jal 0x63d8`, `timrman` ordinal 10,
  `GetTimerCounter` [header]) since the last switch and accumulates them into
  a running 64-bit clock at `0x67d0+0x30/+0x34` — the mechanism behind
  `iop_thread_run_status_t.runClocks`/`iop_sys_status_t.idleClocks` [header].
- If `0x67d4` is still zero (nothing pre-selected), falls back to `0x6bc`
  (§4.2) to pick from the ready queues; otherwise uses the pre-selected
  thread directly.
- Runs a defensive sanity check (`0x8ac`) if the target's record address
  looks suspiciously low relative to the outgoing thread's own stack base —
  prints a warning and `break 0x1`s if `SearchModuleCBByAddr` [header]
  (`loadcore` ordinal 24) can't place the entry point inside a known module.
  A debug-build assertion left in the retail image.
- Returns `*(*0x67d4 + 0x10)` — the **target** thread's own saved-frame
  pointer — which is what `INTRMAN` then loads into `$sp` to actually resume
  execution as that thread.

So the low-level register save/restore itself — the `sq`/`lq`-equivalent
work analogous to the EE's `0x80003680`/`0x80003800` prologue/epilogue
(`docs/analysis/33`) — happens **outside `THREADMAN.text`**, in the
`INTRMAN`-owned interrupt-return path `docs/analysis/37` §3.2/§3.5 already
read (the "top up the interrupted context's saved-register set" step, and
the final restore-and-`jr`). `THREADMAN`'s own code only ever touches the
*pointer* to a context frame, never the frame's contents directly except to
prime it once at `StartThread` time (§2.3, `0xf40`).

### 4.2 The pick loop, `0x6bc`

Called only as `0x940`'s fallback when nothing was pre-selected. Reads
`*0x67d0`'s own state byte:

- **If the current thread is no longer RUN** (it already set itself READY or
  WAIT before triggering the reschedule — every voluntary block/yield in
  §2/§3 does this before calling `0x46c`): find the lowest ready priority
  (`0x658`); if none (`>=128`), log a message and return without picking
  anything (no fatal halt here, unlike the EE analogue's panic path — see
  §6); otherwise pop the head of that priority's queue, mark it RUN, and set
  `0x67d4` to it.
- **If the current thread is still RUN** (a genuine involuntary-preemption
  check, e.g. from a hardware-timer interrupt): find the lowest ready
  priority; if it is **not** strictly better (numerically lower) than the
  current thread's own priority, leave `0x67d4` alone — no preemption, the
  running thread keeps running. Otherwise: pop the better thread, mark it
  RUN, set `0x67d4` to it, and **re-enqueue the outgoing thread as READY** —
  the round trip a preempted-but-not-blocked thread makes back into its
  ready queue.

`FindLowestReadyPriority` (`0x658`) scans the 4-word bitmap at `0x67d8` and
returns the lowest set bit's absolute index (`0`–`127`), or `128` if all four
words are zero. The queue-pop/push primitives (`0x548` dequeue, `0x47c`/
`0x4e0` enqueue) maintain that bitmap in lockstep with the 128 list heads at
`0x67ec` (§2.2).

### 4.3 When it runs: a dedicated syscall, not a bare function call

Every voluntary block/yield point in §2/§3 (`ExitThread`, `SleepThread`,
`WaitEventFlag`'s block path, `WaitSema`'s block path, `DelayThread`) funnels
through the same three-instruction trampoline, `0x46c`:

```
46c:  addiu $2,$zero,0x20
470:  syscall
474:  jr    $ra
```

— a plain **`syscall 0x20`**, with up to four caller-supplied arguments left
untouched in `$a0`–`$a3` for the kernel-side handler to read. `docs/analysis/
37` §2.5 traced `INTRMANP`'s SYSCALL-exception case dispatch for syscalls
4/8/0xc/0x10/0x14 (the `Cpu{Disable,Enable,Suspend,Resume}Intr`/
`CpuInvokeInKmode` group) but did not decode syscall `0x20`'s own case body —
**out of scope for this document too**; what is established here is only
that `THREADMAN` treats it as *the* reschedule-now request, and that the
actual switch decision it eventually reaches is `THREADMAN`'s own
`ShouldPreemptCb`/`NewCtxCb` pair (§4.1), the same pair a hardware
interrupt's own return path consults. The natural reading — that syscall
`0x20`'s handler performs (or falls through to) the same "top up saved
registers, ask `ShouldPreemptCb`, call `NewCtxCb`" tail `docs/analysis/37`
§3.5 already read for the interrupt path — was not directly confirmed by
disassembling that case body; flagged in §7.

Two other invocation shapes exist:

- **Involuntary preemption**, presumably from a periodic hardware-timer
  interrupt reaching the end of `INTRMANP`'s interrupt-return path
  (`docs/analysis/37` §3.5) with the *current* thread still RUN — this is
  exactly `0x6bc`'s "still RUN" branch (§4.2), reached without any explicit
  syscall from `THREADMAN` at all; the timer interrupt's own return is what
  triggers the `ShouldPreemptCb`/`NewCtxCb` pair.
- **Immediate hand-off on enqueue**: `0x5b8`, called by `StartThread`,
  `WakeupThread`, `SignalSema`'s wake path and `ReleaseWaitThread` (never by
  their `i`-suffixed counterparts, which use the deferred `0x47c` instead —
  §2.3/§3), compares the newly-readied thread's priority against `*0x67d0`'s
  own; if the new thread is strictly better, it marks the *current* thread
  READY, re-enqueues it, marks the *new* thread RUN, sets `0x67d4`, and calls
  `0x4e0`/`0x46c`(TerminateThread-tail shape) — i.e. it does not itself save
  registers or switch `$sp`; it stages the decision (exactly `0x67d4`'s
  purpose) and, for the caller's own return path (a `syscall`-based operation
  that's about to return through the same exception machinery anyway), lets
  the normal interrupt/syscall-return tail pick it up. If the new thread is
  *not* better, `0x5b8` just enqueues it (`0x47c`-equivalent) and calls
  `CpuResumeIntr` itself, since no reschedule is due and no exception-return
  path will do it. This is also why `StartThread`'s own body (§2.3) has no
  visible `CpuResumeIntr` call on its success path: `0x5b8` owns that
  responsibility on both of its branches.

### 4.4 No separate idle thread; the idle *loop* is what the boot record primes

There is no dedicated "idle" library function distinct from an ordinary
thread — priority `127` (one below the lowest user-creatable priority,
`126` [header]) is simply reserved by convention for the one thread `THREAD
MAN`'s own entry primes with an infinite empty loop as its entry point (§5).
When `0x6bc`'s pick loop finds *no* ready thread at all (`FindLowestReady
Priority` returning `128`), it logs a diagnostic and returns without
selecting anything, rather than halting outright — in practice this never
happens once the idle thread exists, since it is always ready or running.

## 5. Boot: what THREADMAN's entry creates

`THREADMAN`'s module entry (`0x10`–`0x3dc`) does, in order:

1. Register `thrdman` via `loadcore`'s **pinned** API (ordinal 10, `docs/
   analysis/09`), abort on failure; then register `thbase`/`thevent`/
   `thsemap`/`thmsgbx`/`thfpool`/`thvpool` via the **versioned** API
   (ordinal 6) — matching `09`'s already-established order exactly.
2. `memset` the entire `0x4c4`-byte `.bss` region at `0x67d0` to zero (`0x4c4`
   matches the module header's declared bss size precisely — `0x67d0` really
   is bss's base, confirming `GetThreadmanData`'s return value, §1.1).
3. Initialise several individual list-head sentinels, then **128** more in a
   loop, `0x67ec + i*8` for `i` in `0..127` — the ready-queue heads (§2.2).
4. `CreateHeap(0x800, 1)` [header] — a private `2048`-byte heap; not traced
   further, but distinct from the `0x5ef0` fixed-block pool the object types
   use.
5. **Create the idle thread**, entirely inline (not through the public
   `CreateThread`): pool-allocate a `0x50`-byte record (tag `0x7f01`), a
   `0x200`-byte stack via `AllocSysMemory`, **priority `0x7f` (127)** —
   thbase.h's `LOWEST_PRIORITY` is `126`; this is one below it, reserved —
   attr `TH_C` only, entry point set to `0x464`:

   ```
   464:  j 0x464
   468:  nop
   ```

   an infinite empty loop. **State is set to READY (`2`), not RUN** — the
   exact same surprise `docs/analysis/33` §5 found for the EE kernel's own
   boot thread, now confirmed on the IOP side too: the pick loop, not direct
   kernel init, is what's normally responsible for setting a thread RUN, and
   both CPUs' boot code bypasses it once, at the very start.
6. **Create a second thread record for the code that is already running** —
   THREADMAN's own entry, i.e. whatever `LOADCORE` module-init caller invoked
   it. This one does **not** allocate a fresh stack: it calls `sysmem`'s
   `QueryBlockTopAddress`/`QueryBlockSize` [header] (ordinals 9/10) against
   the *current* stack pointer to discover which existing memory block it
   belongs to, and records those bounds instead. Its **initial** priority
   (`+0x2e`) is set to `8`, but its **current** priority (`+0xe`) is forced
   to `1` (the highest) immediately afterward — an asymmetry with no
   analogue in the idle-thread creation, not otherwise explained. `attr` is
   the same `TH_C`; **state is set to `1` (RUN)** directly, and both `0x67d0`
   and `0x67d4` are pointed at it — this is the thread that goes on to run
   the rest of `IOPBTCONF`'s boot list (`docs/analysis/03`), since `LOADCORE`
   simply keeps calling each subsequent module's entry on whatever thread
   context it's already executing in.
7. Register a reboot-notification handler (`loadcore` ordinal 20,
   `AddRebootNotifyHandler` [header]) tied to the boot-continuation thread's
   record.
8. **Register the two `INTRMAN` reschedule hooks** (`docs/analysis/37` §2.1/
   §3.5): `SetNewCtxCb(0x940)` (ordinal 28), `SetShouldPreemptCb(0xbc4)`
   (ordinal 30) — exactly the pair read in §4.1.
9. Call an internal finisher (`0x50a0`, not traced).
10. `CreateEventFlag({attr:EA_MULTI, option:0, bits:0})` — an internal
    synchronization flag for THREADMAN's own use, id cached at `0x6c7c+4`;
    conditionally `SetEventFlag`s one of its low four bits based on a
    `QueryBootMode(4)` [header] boot-record lookup — the same boot-parameter-
    table mechanism `docs/analysis/09` found `EECONF` using with key `3`, now
    seen a second time with key `4`.
11. Two more `AddRebootNotifyHandler` registrations (ids `2`, `3`).
12. `CpuEnableIntr` (`intrman` ordinal 9) — interrupts are turned on as one
    of the very last steps of THREADMAN's own init, after every piece of its
    own state is in place.

## 6. What this pins for the rebuild

The minimal set our own THREADMAN needs so that `SIFMAN`/`SIFCMD`/`LOADFILE`
(which import `thbase` 4/6/20 = `CreateThread`/`StartThread`/`GetThreadId`,
plus `thevent`/`thsemap` per `docs/analysis/09`'s import table) and
`SIO2MAN` (`thbase` 4/6/20; `thevent` 4/6/7/8/10 = `CreateEventFlag`/
`SetEventFlag`/`iSetEventFlag`/`ClearEventFlag`/`WaitEventFlag`) start and
run:

- **Thread IDs must round-trip through a validity check**, not just be
  usable as an array index — every consumer here validates a tag and a
  generation before trusting an id-derived pointer. A rebuild is free to
  choose its own encoding (a small-index scheme is simpler and equally
  correct for a from-scratch kernel), but it must reject a stale/foreign id
  the same way, since `SIO2MAN`/`SIFCMD` code compiled against ps2sdk only
  ever treats ids as opaque integers.
- **`CreateThread`'s validation order and exact bounds are load-bearing**:
  priority `[1,126]`, entry 4-byte aligned, stack `>=0x130` (rounded up to
  `0x100`), and the attr mask that accepts `TH_ASM`/`TH_C`/`TH_UMODE` but
  rejects `TH_NO_FILLSTACK`/`TH_CLEAR_STACK` — any importer built against a
  ps2sdk that sets those two flags will get `KE_ILLEGAL_ATTR` against this
  retail firmware's real behaviour, and a rebuild that instead *accepts*
  them would silently diverge from what SIFCMD/LOADFILE's client code was
  tested against.
- **`StartThread` must make the started thread's function return into `Exit
  Thread`**, and `ExitThread` must never return to its own caller — this is
  how a normal C-style `void thread(void*)` that just falls off the end
  behaves correctly, and nothing else primes that return address.
- **Two globals and a bitmap-plus-128-queues are the entire scheduler
  state**: "current" and "pending-next" thread pointers, decided by simple
  pointer comparison, consulted only at syscall/interrupt return. A rebuild
  does not need to reproduce the tagged-pointer id scheme, the exact byte
  offsets, or even the bitmap — but it does need the same *protocol*:
  something equivalent to `ShouldPreemptCb` returning false must be a valid,
  safe no-op (§6 of `docs/analysis/37` already made this same point from the
  `INTRMAN` side), since interrupts must be able to return to the
  interrupted thread with zero scheduler support present.
- **`SIF_CMD`/`SIF_MAN`-class modules only need `thbase` 4/6/20 plus
  `thevent`/`thsemap`'s Create/Set/Wait triads** — none of them import
  `ChangeThreadPriority`, `Suspend`/`ResumeThread` (fortunate, since those
  are unimplemented stubs in the reference anyway, §1.1), or the pool
  libraries. A rebuild's THREADMAN can defer `thmsgbx`/`thfpool`/`thvpool`
  and the seven confirmed-stub functions entirely and still satisfy every
  boot-critical importer this pass and `docs/analysis/09` identified.
- **`DelayThread` needs a real hardware timer**, not a spin loop: it arms a
  `timrman`-backed alarm and blocks, and the callback that fires on expiry
  runs the same `unlink → ready → clear-pending` sequence any other wakeup
  path uses. A rebuild without a working `timrman`/alarm mechanism breaks
  `DelayThread` silently (it would simply never return), which several
  modules' init/retry loops (documented elsewhere as spinning on IO) depend
  on completing.

## 7. Unresolved

- **Syscall `0x20`'s own case body**, inside `INTRMANP`'s SYSCALL-exception
  handler, was not disassembled. `docs/analysis/37` §2.5 covered syscalls
  4/8/0xc/0x10/0x14; this document establishes that `THREADMAN` treats `0x20`
  as the universal "reschedule now" request and that the switch decision it
  reaches is `THREADMAN`'s own `ShouldPreemptCb`/`NewCtxCb` pair, but not the
  exact mechanism `INTRMANP` uses to get from "syscall 0x20 arrived" to "run
  the same tail the interrupt-return path uses."
- **`0x940`'s incoming `$a0` and the exact GPR-slot layout of the `0xb8`-byte
  context frame** were pinned only at the handful of offsets `StartThread`/
  `0xf40` prime (`+0x70`/`+0x74`/`+0x78`/`+0x7c`/`+0x88`/`+0x8c`) and the one
  `StartThread` writes (`+0x10`, the incoming `arg`) — the full register-to-
  offset mapping for the remaining ~40 saved words was not derived.
- **`0x8ac`'s exact purpose** — read only far enough to identify it as a
  debug/sanity assertion (`SearchModuleCBByAddr`-based, `break 0x1` on
  failure) triggered from `0x940` when a target thread's address looks
  suspiciously low; not fully traced.
- **`ReferThreadStatus`'s `regContext` field** — the exact condition
  selecting between the record's `+0x30`/`+0x34`/`+0x10` sources was observed
  branching on the state byte but not fully disentangled.
- **`RotateThreadReadyQueue`/`iRotateThreadReadyQueue`, `ChangeThreadPriority`
  /`iChangeThreadPriority`'s exact re-enqueue mechanics, `GetSystemTime`/
  `SetAlarm`/`iSetAlarm`/`SysClock2USec`'s own bodies** — confirmed present,
  confirmed which ordinal each is, not independently disassembled past what
  their callers (`DelayThread`, `TerminateThread`) already established.
- **`thmsgbx`'s send/receive semantics, `thfpool`/`thvpool`'s allocation
  bodies past `Create*`** — out of scope for this pass (§1.4).
- **The exact hardware-Status-bit meaning of the top three `attr` bits**
  (`0x1c000000`'s complement's top nibble) `CreateThread` accepts but
  `thbase.h` never names, consumed by `0xf40` when priming a new thread's
  initial status word (`+0x88`) — plausibly privilege/coprocessor-usable
  bits, not confirmed against the R3000 COP0 `Status` layout.
- **Why the boot-continuation thread's current priority (`1`) diverges from
  its initial priority (`8`)** — observed, not explained; no code path that
  later reconciles them (e.g. a `ChangeThreadPriority` call back to `8`) was
  found in the traced boot-entry range.

## Summary

This document pins THREADMAN's seven ordinal tables (verifying, not just
naming, `thbase`/`thevent`/`thsemap` against their function bodies — finding
seven exports that are declared but unimplemented stubs, and two accepted-
but-rejected `CreateThread` attribute flags along the way); the `0x50`-byte
thread record and the tagged-pointer thread-ID scheme every object type in
the module shares; the full lifecycle (Create/Start/Exit/Terminate/Release/
Sleep/Wakeup/Delay) with every `KE_*` return code tied to `kerr.h`; the
event-flag and semaphore record layouts, wait-list mechanics, and — cross-
checked directly against the binary rather than assumed from the header —
the real AND/OR/CLEAR semantics and the fact that `SA_THPRI` is genuinely
implemented while `KE_SEMA_OVF` is not; and the dispatcher, down to the two
globals (`0x67d0`/`0x67d4`) and the seven-instruction `ShouldPreemptCb` that
is the entire reschedule decision, cross-confirmed against `docs/analysis/
37`'s independent read of the `INTRMAN` side of the same two hooks. Left
open: syscall `0x20`'s own handler body, the full context-frame layout
beyond the handful of primed fields, and a few lower-priority ordinals
(`Rotate`/`ChangeThreadPriority`'s re-enqueue mechanics, the alarm/`SysClock`
group's own bodies) that this pass identified but did not independently
disassemble.
