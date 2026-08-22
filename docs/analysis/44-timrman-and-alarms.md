# TIMRMAN and THREADMAN's Alarms: `DelayThread`, `SetAlarm`, and the System Clock

`docs/spec/06-iop-kernel.md` IOP-3j names the gap plainly: `DelayThread` and the
alarm ordinals answer `-1` because our THREADMAN has no timer manager behind
them (`docs/analysis/38` §2.3 already read as far as `SetAlarm`'s call site and
its callback at module offset `0x2404`, but left the timer allocation, the
queue, the ISR and the involuntary-preemption question open). This document
reads the retail `timrman` module and the rest of THREADMAN's alarm path to
close that gap: what hardware timer is used, exactly how it is programmed, what
its interrupt handler does, and what `USec2SysClock`/`GetSystemTime`/
`SysClock2USec` compute.

```sh
python3 tools/romdir.py assets/SCPH-50000.bin --extract <outdir>   # outside the repo
python3 tools/irxinfo.py <outdir>/TIMEMANP --exports --imports
python3 tools/irxinfo.py <outdir>/TIMEMANI --exports --imports
python3 tools/irxinfo.py <outdir>/TIMEMANI --dump-load <outdir>/TIMEMANI.text
python3 tools/irxinfo.py <outdir>/THREADMAN --dump-load <outdir>/THREADMAN.text
python3 tools/romdis.py <outdir>/TIMEMANI.text --cpu iop --vma 0
python3 tools/romdis.py <outdir>/THREADMAN.text --cpu iop --vma 0
python3 tools/iopsim.py assets/SCPH-50000.bin        # module-release tracking, §0
```

The archive has no file literally named `TIMRMAN`: the retail image ships the
same `P`/`I` variant pair `docs/analysis/06` and `37` found for `EXCEPMAN`/
`INTRMAN`, here named `TIMEMANP`/`TIMEMANI`, both exporting the library tag
`timrman` v1.01. ps2sdk's `iop/system/timrman/include/timrman.h` (fetched from
`https://raw.githubusercontent.com/ps2dev/ps2sdk/master/...`) names ordinals
and the mode-bit/source/timer table in its doc comment; every such name is
marked `[header]`. No ps2sdk `.c`/`.cpp` implementation was read. All addresses
below are module-relative (`--dump-load` output), matching `37`/`38`'s
convention.

## 0. Which variant is resident

Both `TIMEMANP` and `TIMEMANI` export `timrman` v1.01, 17 entries, at nearly
identical bodies (`TIMEMANP` text `0x640`, `TIMEMANI` text `0x690` — the size
difference is the same kind of small per-variant delta `06`/`37` found
elsewhere). `TIMEMANI`'s entry (module offset `0x0`) opens with the **same**
two-part predicate `37` §0 already characterised for `INTRMANI` — `PRId < 0x10`
bails immediately, then `*(u32*)0xBF801450 & 8` bails a second way, and only
when *both* are false does control reach the real init body — i.e. `TIMEMANI`
mirrors `INTRMANI`'s own resident-on-this-hardware condition exactly:

```
0:    addiu $sp,$sp,-0x18 ; sw $ra,0x10($sp)
8:    mfc0  $2,$15,0x0            ; PRId
10:   slti  $2,$2,0x10 ; bnez $2,0x70 ; addiu $2,$0,1   ; PRId<0x10 -> bail, return 1
1c:   lui $2,0xbf80 ; ori $2,$2,0x1450 ; lw $2,0($2)
2c:   andi $2,$2,8 ; beqz $2,0x40                        ; bit3 clear -> proceed
38:   j 0x70 ; addiu $2,$0,1                                ; bit3 set -> bail, return 1
40:   ... (zero six per-timer in-use counters, then register)
```

`docs/analysis/23`'s IRX-12 release table already recorded, from watching
`sysmem` ordinal 5 during an ordinary `tools/iopsim.py` run, that the *second*
of the boot's four freed images is `0x007D00`, tagged `timrman`,
`Timer_Manager` — i.e. `TIMEMANP` is the rejected half, freed the same way
`INTRMANP` is (`23` §"IRX-12 is observable earlier, too"). `TIMEMANI` is
therefore the resident variant analysed below; every address in §1–§7 is
`TIMEMANI`'s.

The entry's tail (`0x40`–`0x7c`) zeroes a byte at `0x6b0 + i*0xc` for `i` in
`0..5` (six per-timer "in-use" counters, §2), then calls the `loadcore`
ordinal-6 import stub (`RegisterLibraryEntries`, `IRX-14`) with the module's own
export table (`0x5c0`), and returns `0` on success / `1` on failure — the
`sltu $0,$2` at the tail converts `RegisterLibraryEntries`'s own C-style
error-code return (`0` = success) into IRX-12a's residency convention, so
`TIMEMANI` stays resident exactly when registration succeeds.

## 1. The exported ordinals

ps2sdk's `timrman.h` [header] gives ordinals `3`–`18`; this v1.01 firmware's
export table has 17 entries, `0`–`16`, and every one of them lines up
one-for-one with the header through ordinal 16 (`GetHardTimerIntrCode`) —
ordinals 17 (`GetTimerMode`) and 18 (`GetTimerReadFunc`), both marked "guessed
name" in the current header, are **absent** from this firmware's table
entirely:

| Ord | Addr | Name [header] |
| --- | --- | --- |
| 0 | `0x0` | module entry (§0) |
| 1, 2 | `0x61c` | reserved, shared `jr $ra` stub |
| 3 | `0x80` | `GetTimersTable() -> void*` — returns `&0x6a8`, the per-timer descriptor table's own base (§2) |
| 4 | `0x90` | `AllocHardTimer(source, size, prescale) -> timid \| -1` (§2) |
| 5 | `0x1b0` | `ReferHardTimer(source, size, mode, modemask) -> timid \| -1` — same scan shape as ordinal 4, not independently re-derived past confirming it exists and is not called by `THREADMAN` |
| 6 | `0x3a4` | `FreeHardTimer(timid) -> 0 \| -0x96` (§2) |
| 7 | `0x468` | `SetTimerMode(timid, mode)` (§2) |
| 8 | `0x478` | `GetTimerStatus(timid) -> u32` — reads the same MODE register `SetTimerMode` writes (§2, §6) |
| 9 | `0x488` | `SetTimerCounter(timid, count)` (§2) |
| 10 | `0x4b8` | `GetTimerCounter(timid) -> u32` (§2) |
| 11 | `0x4e8` | `SetTimerCompare(timid, compare)` (§2) |
| 12 | `0x51c` | `GetTimerCompare(timid) -> u32` — not imported by `THREADMAN` |
| 13 | `0x550` | `SetHoldMode(holdnum, mode)` — a *different* register block (`0xBF8014C0`), unrelated to the alarm path; not imported by `THREADMAN` |
| 14 | `0x588` | `GetHoldMode(holdnum) -> u32` |
| 15 | `0x5a4` | `GetHoldReg(holdnum) -> u32` — reads `0xBF8014B0 + holdnum*4` |
| 16 | `0x2f4` | `GetHardTimerIntrCode(timid) -> irq \| -1` (§2) |

`THREADMAN` imports exactly seven of these — ordinals `[4, 7, 8, 9, 10, 11,
16]` (`AllocHardTimer`, `SetTimerMode`, `GetTimerStatus`, `SetTimerCounter`,
`GetTimerCounter`, `SetTimerCompare`, `GetHardTimerIntrCode`) — matching
`docs/analysis/38`'s "seven `timrman` ordinals total" exactly. It never calls
`ReferHardTimer`, `FreeHardTimer`, `GetTimerCompare`, or any of the
`Hold*` functions. **There is no `SetTimerHandler` export in this firmware or
in ps2sdk's header** — the task brief's premise is corrected here: `THREADMAN`
finds its own IRQ number via `GetHardTimerIntrCode` and installs the handler
itself with `intrman`'s `RegisterIntrHandler`/`EnableIntr` (ordinals 4/6,
imported directly by `THREADMAN`, not `timrman`), exactly the same way
`SIO2MAN`'s IRQ-17 handler is installed (`spec/06` IOP-6a).

## 2. The six hardware timers, and how `AllocHardTimer` picks one

`GetHardTimerIntrCode` (`0x2f4`) is a straight-line comparison chain against
six literal register-base constants, fully decoded (delay slots included,
since several of its branches build the comparison constant in the delay
slot the branch shares):

| Register base (KSEG1 / physical) | Return |
| --- | --- |
| `0xBF801100` / `0x1F801100` | `4` |
| `0xBF801110` / `0x1F801110` | `5` |
| `0xBF801120` / `0x1F801120` | `6` |
| `0xBF801480` / `0x1F801480` | `14` (`0xe`) |
| `0xBF801490` / `0x1F801490` | `15` (`0xf`) |
| `0xBF8014A0` / `0x1F8014A0` | `16` (`0x10`) |
| anything else | `-1` |

These are exactly the six register blocks the task names, and the returns are
ps2sdk's `IOP_IRQ_TIMER0`..`IOP_IRQ_TIMER5` numbering. `timid` itself is not an
opaque handle: every `Set*`/`Get*Timer*` function does `regaddr = timid << 2`
and addresses the hardware directly (`sll $4,$4,0x2`, no table lookup) — i.e.
**`timid = register_base >> 2`**, confirmed independently by `FreeHardTimer`
computing `$16 = timid<<2` and comparing it against the descriptor table's own
stored base word.

**The per-timer descriptor table is static data, not code.** `AllocHardTimer`
(`0x90`) scans six 12-byte records at absolute `0x6a8`–`0x6f0` — module offset
`0x6a8`, past `TIMEMANI`'s `0x690`-byte text, inside its `0x60`-byte `.data`
(the module has no `.bss`). Reading those bytes directly (not inferring them
from code) gives, in scan order:

| Slot | `regbase` | `mask` | `size` | `prescale` | Timer [header names] |
| --- | --- | --- | --- | --- | --- |
| 0 | `0xBF801120` | `0x01` (SYSCLOCK) | 16 | 8 | RTC2 |
| 1 | `0xBF8014A0` | `0x01` (SYSCLOCK) | 32 | 256 | RTC5 |
| 2 | `0xBF801490` | `0x01` (SYSCLOCK) | 32 | 256 | RTC4 |
| 3 | `0xBF801480` | `0x05` (SYSCLOCK\|HLINE) | 32 | 1 | RTC3 |
| 4 | `0xBF801100` | `0x0b` (SYSCLOCK\|PIXEL\|HOLD) | 16 | 1 | RTC0 |
| 5 | `0xBF801110` | `0x0d` (SYSCLOCK\|HLINE\|HOLD) | 16 | 1 | RTC1 |

Every `(mask, size, prescale)` triple matches ps2sdk's header table for that
RTC exactly (source mask bits: `1`=`TC_SYSCLOCK`, `2`=`TC_PIXEL`, `4`=
`TC_HLINE`, `8`=`TC_HOLD` [header]) — this is a genuine cross-check, not
circular, since the table is raw data and the header names are independent.
**Scan order is not RTC0..RTC5** — it visits RTC2, RTC5, RTC4, RTC3, RTC0, RTC1.

`AllocHardTimer(source, size, prescale)` walks this table (`0x104`–`0x184`),
skipping any slot whose in-use byte (`+0x6b0`-relative, i.e. the same six
counters the entry function zeroes at boot) is nonzero, and for each remaining
slot checks `source & mask != 0`, `size == requested`, and — the operative
comparison —

```
158:  lh   $2,0x6ae($idx)     ; table.prescale
168:  slt  $2,$2,$19            ; table.prescale < requested ?
16c:  beqz $2,0xc8                ; NOT less, i.e. table.prescale >= requested -> claim this slot
```

**the match condition is `table.prescale >= requested`, not equality.** A
caller asking for the finest prescale (`1`) is satisfied by the *first*
size/source-matching slot the scan reaches, regardless of that slot's own
table prescale value. On a claim, the in-use byte is incremented (a
reference count, not a boolean — `FreeHardTimer` decrements the same byte) and
`GetTimersTable`'s base word (`0x6a8 + i*0xc`) is returned right-shifted by 2 as
`timid`. `ReferHardTimer` (ordinal 5) is the read-only sibling: same scan,
without claiming.

## 3. `THREADMAN` allocates timer 5, not timer 3

`THREADMAN`'s boot-time finisher (module offset `0x50a0` — this is
`docs/analysis/38` §5 step 9, "an internal finisher, not traced," now traced)
calls `AllocHardTimer(source = TC_SYSCLOCK (1), size = 32, prescale = 1)`:

```
5110:  addiu $4,$0,0x1     ; source = TC_SYSCLOCK
5114:  addiu $5,$0,0x20     ; size = 32
5118:  move  $6,$4            ; prescale = 1
...
5134:  jal 0x63b8               ; timrman ordinal 4, AllocHardTimer
```

Given §2's scan order (RTC2 first, then RTC5), and the `>=`-not-`==` prescale
match: **RTC2 is rejected on size (16 ≠ 32); RTC5 is the first candidate whose
source, size and prescale (`256 >= 1`) all pass — so `AllocHardTimer` returns
RTC5's `timid`, register base `0xBF8014A0` / physical `0x1F8014A0`, IRQ `16`.**
`RTC3` (base `0xBF801480`, IRQ `14`) — the timer this document's first pass
through the disassembly assumed, before the descriptor table was read as data
rather than inferred from scan-position — is never reached: RTC5 already
satisfies the request three slots earlier in scan order. This is a genuine
correction, caught by reading the raw table bytes rather than trusting an
assumed slot-to-RTC mapping.

**This independently confirms a lead already recorded in
`docs/project-state.md` §4** (from the sibling PS2e project's own tracing of
the same SCPH-50000 image): *"IOP I_MASK: `0x1080D` = VBLANK (0), CDVD (2), DMA
(3), EVBLANK (11) and **timer 5 (16)**"* — the disassembly above derives the
same answer (RTC5, IRQ 16) independently, from the module's own code and
static data, with no dependency on the lead. The lead is corroborated, not
merely carried forward unverified.

**Timer 5's programming, in the exact order the finisher issues it** (`0x5108`
onward, immediately after the `AllocHardTimer` call, `$20` = `timid`):

```
5108:  jal 0x5598($4=100,$5=&tmp)      ; USec2SysClock(100 usec, &tmp)   -- 100us worth of ticks, §4
5120:  lw $2,0x10($sp)                   ; $2 = tmp.lo  (== 3686, §4)
512c:  sw $2,0x6c84                        ; cache: ticks-per-100us
5138:  sw ($2<<1),0x6c88                    ; cache: ticks-per-200us (used by the batching re-arm, §6)
5140:  move $4,$20
5144:  jal 0x63e8 ; sw $20,0x6bec($16)         ; GetHardTimerIntrCode(timid); cache timid -> [0x6bec]
514c:  move $4,$2                                ; $4 = irq (16)
5150:  addiu $5,$0,1                                ; mode = 1
5154:  lui $6,0 ; addiu $6,0x5280                     ; handler = &0x5280 (the ISR, §6)
515c:  addiu $18,$16,-0x4b4                             ; arg = &0x67d0  (THREADMAN's own "current thread" pointer cell, docs/analysis/38 §4.1)
5160:  jal 0x62fc                                          ; intrman ordinal 4, RegisterIntrHandler(irq=16, mode=1, handler=0x5280, arg=&0x67d0)
5210:  move $4,$20
5214:  jal 0x63c0 ; move $5,$0                             ; SetTimerCounter(timid, 0)           -- reset the counter
521c:  lw $5,0x10($sp)                                       ; $5 = ticks-per-100us (3686)
5220:  jal 0x63e0 ; move $4,$20                                ; SetTimerCompare(timid, 3686)     -- first compare, 100us out
5238:  jal 0x63c0 ; addiu $5,$0,0x70                             ; SetTimerMode(timid, 0x70)      -- arm the timer
5240:  jal 0x63e8 ; move $4,$20                                    ; GetHardTimerIntrCode(timid) again -> $2
5248:  jal 0x6304 ; move $4,$2                                       ; intrman ordinal 6, EnableIntr(16)
```

**Register semantics, confirmed from `SetTimerMode`/`GetTimerStatus`/
`SetTimerCounter`/`GetTimerCounter`/`SetTimerCompare`/`GetTimerCompare`'s own
bodies (all six take `timid`, recover `regaddr = timid<<2`, no other
indirection):**

| Offset from `regaddr` | Width | Role |
| --- | --- | --- |
| `+0x0` | 16-bit (RTC0/1/2) or 32-bit (RTC3/4/5) | COUNT — `SetTimerCounter`/`GetTimerCounter` |
| `+0x4` | 16-bit, always | MODE — `SetTimerMode` writes it, `GetTimerStatus` reads the same register back |
| `+0x8` | 16-bit (RTC0/1/2) or 32-bit (RTC3/4/5) | COMPARE/TARGET — `SetTimerCompare`/`GetTimerCompare` |

The 16-vs-32-bit choice is made purely by comparing `regaddr` (or `regaddr+8`
for the compare register) against the fixed threshold `0xBF80147F` — RTC0–2
fall below it, RTC3–5 above.

**`MODE = 0x70`** is the exact value programmed. Against the header's bit
table (`0x10`=bit5 "Interrupt on target", `0x20`=bit6 "Interrupt on overflow",
`0x40`=bit7 "??? (Repeat?)"): `0x70` enables Interrupt-on-Target and
Interrupt-on-Overflow together, plus the undocumented bit7, and leaves bit4
(`0x08`, "Reset on target") **clear** — the counter free-runs past its compare
value rather than auto-resetting, which is why the ISR (§6) must always
reprogram the compare register itself for the next deadline.

## 4. `USec2SysClock`/`SysClock2USec`, and the clock-rate constant

**`USec2SysClock(usec, out*)`** (ordinal 39, `0x5598`) computes a 64-bit
`sysclock = usec * A / B` using two module-global constants read from
`0x6c8c`/`0x6c90` (a 64-bit multiply into a helper at `0x5ca4`, then a 64-bit
divide-with-remainder at `0x5ce0`). **`SysClock2USec(clock*, sec*, usec*)`**
(ordinal 40, `0x5614`) is the inverse shape: `usec_total = clock * B / A`
(same two constants, roles swapped), then that result is divided by
`1000000` (`0xF4240`, a literal immediate) to split whole seconds (`*sec`)
from the remainder (`*usec`), taken directly from the division's own
remainder output.

`A`/`B` default to **`4608`/`125`** — `4608/125 = 36.864` exactly, the IOP's
documented `TC_SYSCLOCK` rate (`36.864 MHz`, matching ps2sdk's own comment:
`"MIPS R3000A in 36.864MHz IOP mode"`). `100 usec * 4608/125 = 3686` (integer
truncation of `3686.4`), which is exactly the value the finisher programs into
timer 5's compare register at boot (§3).

**A boot-record override exists and was not resolved on this image.** The
finisher writes the defaults first, then calls `QueryBootMode(key = 7)`
(`loadcore` ordinal 12); if that returns a non-null record whose first
halfword equals `200` (`0xc8`), the constants are overwritten to **`A=25,
B=1`** — a flat 25 MHz clock, no fraction — before any `USec2SysClock` call
is made:

```
50d4:  addiu $2,$0,0x7d ; sw $2,0x6c90($16)       ; default B = 125
50d8:  jal 0x62c8 ($4=7)                             ; QueryBootMode(7)
50e0:  beqz $2,0x5108                                  ; no record -> keep defaults
50e8:  lhu $3,0($2) ; addiu $2,$0,0xc8 ; bne $3,$2,0x5108   ; record[0] != 200 -> keep defaults
50f8:  addiu $2,$0,0x19 ; sw $2,0x6c8c($16)            ; override A = 25
5100:  addiu $2,$0,0x1  ; sw $2,0x6c90($16)              ; override B = 1
```

Whether `SCPH-50000`'s boot-parameter table actually carries a key-7 record
matching `200` is not established here — see Open Questions. **A rebuild
should implement `36.864 MHz` as the default and treat the override as
optional**, since nothing downstream depends on it being present.

## 5. The alarm record and the sorted queue

**Record layout, 32 (`0x20`) bytes, from a dedicated free-list-backed heap**
(handle cached at `0x6c78`, populated once by `heaplib`; distinct from the
`0x800`-byte private heap `docs/analysis/38` §5 found for other pool
bookkeeping):

| Offset | Field |
| --- | --- |
| `+0x0` / `+0x4` | doubly-linked list next/prev — the *only* list a record is ever on is the alarm queue itself |
| `+0x8` | pool tag, always `0` for alarm records (no distinct tag byte the way thread/sema/event records use `0x7f01`–`0x7f05`) |
| `+0xa` | a **monotonic allocation-sequence number**, not a type enum (below) |
| `+0x10` / `+0x14` | 64-bit deadline (lo/hi), an absolute `sysclock` value |
| `+0x18` | callback function pointer |
| `+0x1c` | callback argument |

**`+0xa` is the free-list allocator's own running counter, reused as a
distinguishing tag rather than a designed "kind" field.** The pool allocator
(`0x5d78`) first tries to pop a node off a circular free-list at `0x6c34`; only
when that list is empty does it carve a fresh `0x20`-byte block from the heap
and, in that fresh-allocation path only, write a global counter (`0x6c48`,
zeroed at boot, incremented on every fresh carve) into the new record's `+0xa`
— **and only there**: a record recycled from the free-list keeps whatever
`+0xa` value it was originally carved with. Since the counter starts at `0`
and the very first call the finisher itself makes to this allocator (at boot,
before the free-list can hold anything) is exactly one fresh carve, **the
housekeeping record created by the finisher is permanently, uniquely tagged
`+0xa == 1`**, and no other record can ever independently acquire that same
value (the counter is never reset). The ISR's `+0xa == 1` check (§6) is
therefore "is this the one permanent record," implemented as a side effect of
allocation order rather than an explicit flag.

**`SetAlarm`/`iSetAlarm` share one algorithm**, differing only in the
interrupt-context test's polarity and whether `CpuSuspendIntr`/`CpuResumeIntr`
bracket it (`SetAlarm`, ordinal 35, `0x5764`, requires **thread** context —
`QueryIntrContext() != 0` returns `KE_ILLEGAL_CONTEXT` (`-0x64`); `iSetAlarm`,
ordinal 36, `0x5914`, requires the opposite — **interrupt/critical-section**
context, same error code otherwise, and skips the suspend/resume bracket since
its caller already holds one):

1. Scan the queue (sentinel at `0x6c2c`) for an existing record whose
   `callback`/`arg` fields both match the caller's own — if found, return
   `-0x68` without touching anything (**a second `SetAlarm` for the same
   `(callback, arg)` pair is rejected outright, not merged or replaced**).
2. Pop a record from the pool (§ above); pool exhaustion returns `-0x190`
   (`-400`, `KE_NO_MEMORY`).
3. **Clamp the caller's delta to a minimum of the cached `0x6c84` value**
   (100 us worth of ticks) if it is smaller and has no high word — the
   alarm system's effective minimum granularity is ~100 us, not zero.
4. Compute `deadline = now + delta` — `now` comes from `GetSystemTime`
   (`SetAlarm`) or the internal callback directly (`iSetAlarm`, §7) — and
   store it into the new record's `+0x10`/`+0x14`.
5. **Insert sorted, ascending by the 64-bit deadline**, ties broken so an
   equal-deadline newcomer goes *after* any existing equal entry (FIFO among
   ties) — the insertion routine (`0x5a88`) walks the list comparing
   hi-then-lo words and links in before the first strictly-later record.
6. Reprogram the hardware compare register for whatever is now the queue's
   effective head (`0x548c`, §6) — **every successful `SetAlarm`/`iSetAlarm`
   re-arms the timer immediately**, since the newly-inserted record might now
   be the earliest pending deadline.

**`CancelAlarm`/`iCancelAlarm`** (ordinals 37/38, `0x5b04`/`0x5be0`) have the
same context-requirement pairing as `SetAlarm`/`iSetAlarm` (**`CancelAlarm`
requires thread context, `iCancelAlarm` requires interrupt context** — the
same inversion, `-0x64` on the wrong one). Both scan the same queue for a
`(callback, arg)` match; on a hit, unlink, free the record back to the pool,
decrement the active-alarm counter (`0x6c4c`) and return `0`; on no match,
return `-0x69`. **Neither reprograms the hardware compare register.** This is
safe rather than sloppy: if the cancelled record happened to be the one
currently armed, the timer still fires at the old (now stale) compare value,
the ISR finds the queue's real head is not yet due, and reprograms the
compare register itself before returning (§6) — cancellation is
self-correcting through the next interrupt rather than needing its own
hardware write.

## 6. The ISR (`0x5280`, IRQ 16, mode 1) — status, the 64-bit clock, and expiry

Registered with `arg = &0x67d0` (`THREADMAN`'s own "current thread" pointer
cell, `docs/analysis/38` §4.1 — reused purely as a stable anchor into
`THREADMAN`'s `.bss`, not because the handler inspects the current thread).

**Status handling.** `GetTimerStatus(timid)` (a raw read of the MODE register,
§3) is read once at entry:

```
52b8:  andi $2,$16,0x1000    ; Overflow status bit
52bc:  beqz $2,0x52dc          ; clear -> skip the accumulator bump
52c4:  lw $2,0x420($18) ; addiu $2,$2,1 ; sw $2,0x420($18)   ; [0x6bf0] (hi word) += 1
52d4:  sw $17,0x424($18)         ; [0x6bf4] (cached low word) = counter value at overflow
52dc:  beqz $2,0x546c              ; Target status bit clear -> nothing else to do, return
```

`0x67d0 + 0x420 = 0x6bf0` / `0x67d0 + 0x424 = 0x6bf4` are the two halves of a
**software-maintained 64-bit clock layered on the 32-bit hardware counter**:
`[0x6bf0]` counts full 32-bit wraps, `[0x6bf4]` caches the low word for
wrap-detection elsewhere (§7). The Overflow branch only ever *advances* this
accumulator; **only the Target status bit gates the alarm-queue walk that
follows** — an Overflow-only interrupt (the counter simply wrapped, with no
alarm due) returns immediately after the bump above.

**The expiry loop**, once Target is set, walks the sorted queue from its head:

```
5344:  beq $16,$19,0x5454          ; reached the sentinel -> done, re-arm and return
534c-5378:  compare now (64-bit, from GetTimerCounter + the accumulator) against record.deadline
5358:  bnez $2,0x5454                ; now < deadline -> queue is sorted, nothing further is due, stop
537c:  jal 0x5e90($4=record)           ; unlink
5384:  lhu $3,0xa($16) ; addiu $2,$0,1 ; bne $3,$2,0x53c4    ; is this the tag==1 permanent record?
```

**The permanent record (`+0xa == 1`, §5) never calls a callback.** It simply
recomputes its own deadline as `deadline.hi += 1` (deadline unchanged in the
low word — i.e. exactly one full 32-bit counter period, `2^32` ticks, later)
and reinserts itself. Its function is purely to guarantee the ISR runs, and
the accumulator stays fresh, at least once per hardware-counter wrap even when
no user alarm is pending; it was seeded at boot with a *negative* initial
deadline (`-(2000 usec worth of ticks)`, computed by the same finisher, §3),
so it is unconditionally due the very first time the queue is ever walked.

**Every other due record** calls `callback(arg)` (fields `+0x18`/`+0x1c`):

```
53c4:  lw $4,0x1c($16) ; lw $2,0x18($16) ; jalr $2
53d8:  bnez $2,0x53fc     ; callback returned nonzero -> re-arm, don't free
53e0:  jal 0x5de4($4=record)   ; callback returned 0 -> free the record back to the pool
53e8-53f8:  [0x6c4c] (active-alarm count) -= 1
```

**A nonzero callback return is a self-repeat request.** The record is *not*
freed; instead `new_delta = min(callback_return, [0x6c88])` (`[0x6c88]` is
`2 * [0x6c84]`, i.e. ~200 us worth of ticks, computed once at boot) is added to
the record's *old* deadline, and it is reinserted — so a callback can keep
itself alive indefinitely by always returning a positive tick count, capped at
roughly 200 us per re-arm regardless of what it asks for.
`docs/analysis/38`'s `DelayThread` callback (module offset `0x2404`: unlink the
woken thread, ready-enqueue it, clear the pending-next global) is a **one-shot**
consumer of this protocol — it must return `0` for its record to be freed
rather than perpetually re-armed, which is consistent with everything already
read about it.

**After the loop drains** (either the sentinel is reached or the next record
is not yet due), the tail (`0x5454`) reprograms the hardware compare register
for the queue's current head via a shared subroutine (`0x548c`, also called
directly by `SetAlarm`/`iSetAlarm` on every successful insert, §5):

- It first scans forward from the head, grouping together any run of records
  whose deadlines are within `[0x6c88]` (~200 us) of each other, and uses the
  **last** record of that run as the one whose deadline actually gets
  programmed — a small coalescing pass so closely-spaced alarms don't each
  provoke their own hardware reprogram.
- If that record's deadline, minus `now`, is already `<= 0` (i.e. it is due
  or overdue *right now*, which can happen if reprogramming itself took long
  enough), the compare register is instead set to `GetTimerCounter(timid) +
  [0x6c88]` — arm ~200 us in the future from the current instant, rather than
  programming a compare value that has already passed.
- Otherwise the compare register is simply set to the target record's own
  deadline (low 32 bits only — the hardware compare register is 32 bits, and
  the low word is sufficient since the queue only ever holds records due
  within one hardware-counter period of `now`, given the permanent record's
  own `2^32`-tick self-repeat).
- An **empty queue** falls into the same code with the scan pointer left at
  the sentinel; its own `+0x10`/`+0x14` fields are read as if they were a real
  record's deadline. This document did not confirm what the sentinel's fixed
  content is (see Open Questions), but the code path requires no special case
  for "queue empty" to behave sanely, which is presumably the intent.

## 7. `GetSystemTime`: two calling conventions, only one of them works

The kernel-mode callback that actually reads the clock (`0x56bc`, invoked via
`intrman` ordinal 14, `CpuInvokeInKmode`) takes an optional output pointer in
`$4`:

```
56dc:  lw $4,-0x4($17)  ; [0x6bec] = cached timid
56e4:  jal 0x63d8($4=timid)   ; GetTimerCounter(timid) -> $2
56f4:  sltu $3,$2,$3            ; wrapped since the last read? (same [0x6bf0]/[0x6bf4] pair the ISR maintains)
5708:  sw $2,0x424($17)           ; cache the low word
570c:  beqz $18,0x571c              ; out-pointer null -> skip storing
5714:  sw $16,0x4($18) ; sw $2,0($18)  ; *out = { hi = accumulator, lo = current counter }
572c:  move $2,$0                       ; return value is *always* 0
```

`SetAlarm` and `iSetAlarm` both call this to get "now," but through two
different paths: `iSetAlarm` calls `0x56bc` **directly** (`jal 0x56bc ; addiu
$4,sp,0x18` — it is already in a suspended/critical context, so no trap is
needed), while `SetAlarm` goes through the **public ordinal**, `GetSystemTime`
itself (ordinal 34, `0x5738`):

```
5738:  addiu $sp,$sp,-0x18
573c:  move $5,$4                 ; forward the caller's OWN $4 as the callback's out-pointer
5740:  lui $4,0 ; addiu $4,0x56bc
574c:  jal 0x631c                   ; CpuInvokeInKmode(func=0x56bc, arg=$5)
575c:  jr $ra                         ; return whatever CpuInvokeInKmode/the callback left in $2 (i.e. 0)
```

`SetAlarm` itself calls `GetSystemTime` with `$4 = &stack_local` explicitly
(`5858: addiu $4,sp,0x18`) and then reads the 64-bit value back from that
stack slot — **never from `$2`/`$3`**. But ps2sdk's public header declares
`GetSystemTime` as `u64 GetSystemTime(void)` — zero arguments, return by
register, the normal MIPS o32 convention for a 64-bit result. A caller that
actually follows that public signature leaves `$4` as whatever it happened to
hold (there is no argument to set), and the callback's own tail **always
returns `0` in `$2` and never touches `$3`** regardless of whether `out` was
null. **Every confirmed call site in this firmware passes an explicit
pointer; none relies on the public zero-argument/register-return contract.**
Whether real ps2sdk-linked client code ever calls `GetSystemTime()` the
zero-argument way, and if so what it would actually observe, was not
established — flagged in Open Questions. A rebuild should implement the
pointer-based contract that `SetAlarm`/`iSetAlarm` actually depend on, and can
treat the public zero-arg path's behaviour as unspecified rather than
reproducing the apparent defect.

## 8. `DelayThread`'s exact blocking sequence

Already established by `docs/analysis/38` §2.3 and cross-confirmed here
against the record/callback protocol above; restated for completeness since
this document settles what `SetAlarm` and the alarm callback contract actually
require of it:

```
246c:  jal 0x5598($4=usec,$5=&localSysClock)          ; USec2SysClock(usec, &clock)  (§4)
24d4:  jal 0x5764($4=&localSysClock,$5=0x2404,$6=self)  ; SetAlarm(&clock, callback=0x2404, arg=current-thread-record)
```

then `state = WAIT`, `waitType = TSW_DELAY (2)`, link onto the sleeping-thread
list at `0x67d0+0x488`, issue the reschedule trap (`spec/06` IOP-3h). The
callback at `0x2404` — confirmed here to be a **one-shot** consumer of §6's
protocol, since it does not compute or return a nonzero re-arm delta —
unlinks the woken thread, ready-enqueues it (the deferred `0x47c` form, since
it runs from interrupt/callback context), and clears the pending-next global,
then implicitly returns `0` so the ISR frees its alarm record rather than
re-arming it. `TerminateThread`/`ReleaseWaitThread` disarm a still-pending
delay the same way `docs/analysis/38` already read: `CancelAlarm(0x2404,
thread)`/`iCancelAlarm`, matching §5's `(callback, arg)`-keyed cancellation
exactly.

## 9. Settling `docs/analysis/38`'s open "involuntary preemption" question

`38` §4.3 left open whether a periodic hardware-timer interrupt drives
`spec/06` IOP-3h's "involuntary preemption" branch (the pick loop's
"current thread still RUN" case) on a fixed schedule, the way a classic
round-robin quantum would. **Having now read timer 5's entire interrupt
handler end to end, no such mechanism exists in this firmware.** The ISR
(§6) does exactly two things: maintain the 64-bit clock accumulator, and
walk the alarm queue calling callbacks / re-arming the permanent
housekeeping record. It never calls `intrman`'s `ShouldPreemptCb`/`NewCtxCb`
hooks, `CpuInvokeInKmode`, or anything else that would force a reschedule
decision — the only THREADMAN primitives it calls are the generic
unlink/ready-enqueue pair (`0x5e90`/`0x47c`) that any wakeup path uses. Any
preemption that follows an alarm firing is entirely the *generic*
mechanism `37` §3.5 and `38` §4.1 already read: a non-nested interrupt's own
return path always consults `ShouldPreemptCb`, and if the alarm ISR's own
`WakeupThread`-equivalent call readied a higher-priority thread, that generic
check is what acts on it — nothing about the timer interrupt is special-cased
for scheduling. **There is no periodic round-robin/preemption ticker in this
retail image.** `spec/06` IOP-3h's "involuntary preemption" branch is real
code, reachable in principle from *any* interrupt's return path while a
thread is still `RUN`, but nothing in this image's boot sequence ever arms a
timer to drive it on a fixed cadence — it is purely opportunistic, triggered
only by whichever interrupt (an alarm expiry among them) happens to occur
while a thread is running.

## 10. What an emulator needs

For the alarm/`DelayThread` path specifically (narrower than `spec/06`
IOP-2's general interrupt-controller requirements, already covered by
`docs/analysis/37`/`24`):

- **Timer 5's three registers** (`0xBF8014A0`/`+4`/`+8`, physical
  `0x1F8014A0`/`+4`/`+8`, all 32-bit) must behave as a free-running up-counter
  gated by `MODE`: writing `MODE=0x70` must arm both an Overflow-status latch
  (set when the counter wraps `0xFFFFFFFF -> 0`) and a Target-status latch
  (set when `COUNT == COMPARE`), **without** auto-resetting `COUNT` on a
  target match (bit `0x08`, "Reset on target," is not set) — the counter must
  keep running past its compare value.
- **IRQ 16** (`I_MASK` bit 16) must reach the CPU when either status bit is
  set, acked the way `docs/analysis/37` IOP-2f already specifies for the
  general path (write-0-to-the-bit-and-1-elsewhere to `I_STAT`).
- **`GetTimerStatus`/reading `MODE`** is expected by real IOP timer hardware
  (the same PS1-derived counter block documented in the sibling project's own
  notes for the EE side's analogous `EQUF`/`OVFF` latches) to clear the status
  bits as a side effect of the read — this firmware's ISR never writes `MODE`
  to acknowledge Target/Overflow explicitly, only reads it once per
  invocation, so **an emulator that does not clear these latches on read will
  re-fire the same status forever.** This is inferred from known IOP/PS1
  timer hardware behaviour and from the ISR's own silence about any other
  acknowledgement path, not observed directly in software (a hardware side
  effect has no visible trace in static disassembly) — worth independent
  verification against PCSX2/PS2e's own timer models.
- **`COUNT` must actually free-run at the rate the boot-time `SetTimerCounter
  (timid, 0)` / `SetTimerMode(timid, 0x70)` sequence implies**, since
  `USec2SysClock`'s constant (§4, `36.864 MHz` by default) is baked into every
  deadline computed in software — whatever rate the emulator's hardware model
  actually runs timer 5 at must match that constant, or every alarm and every
  `DelayThread` will be systematically early or late by whatever ratio the two
  disagree by (see Open Questions on timer 5's own documented `/256` prescale
  and whether `MODE=0x70`'s lack of explicit prescale bits overrides it).

**Leads carried in from the sibling PS2e project** (`docs/project-state.md`
§4's convention: recorded, each marked as a lead, and checked against this
image's own tools/disassembly rather than trusted outright):

| Lead | Verified here? |
| --- | --- |
| "IOP I_MASK `0x1080D` ... and timer 5 (16). Thread delays rely on ... timer-5 target interrupts" | **Confirmed independently** — §3 derives the same timer (register base `0xBF8014A0`, IRQ 16) from `TIMEMANI`'s static descriptor table and `THREADMAN`'s own `AllocHardTimer` call, without reference to the lead |
| "IOP DMA completions report through DICR2 ... and I_STAT bit 3" | Out of scope here (interrupt-controller wiring, not timers) — see `docs/analysis/37` §2i/§3.4, already independently confirmed there |

No other lead in the sibling project's notes bears on `timrman`/`THREADMAN`'s
alarm path; the EE-timer section of those notes (`Tn_MODE` bits, `EQUF`) is a
different CPU's hardware entirely and is cited above only as an analogy for
what "read-to-clear status" typically means on this timer-block family, not as
a claim about the IOP's own register.

## What this pins for the rebuild

- **`TIMRMAN`'s exports map ordinal-for-ordinal onto ps2sdk's `timrman.h`
  through ordinal 16**; ordinals 17/18 do not exist in this firmware and need
  not be implemented for `THREADMAN` (or any other importer this project has
  found) to work.
- **Timer 5 (`0xBF8014A0`, IRQ 16) is THREADMAN's system-clock timer, not
  timer 3** — a rebuild's `THREADMAN` must request `AllocHardTimer(TC_SYSCLOCK,
  32, 1)` against a descriptor table whose scan order and `>=`-not-`==`
  prescale matching produce that same result, or must simply hard-code timer
  5 directly, matching the reference's observable behaviour either way.
- **`MODE = 0x70`, `COUNT` reset to `0`, initial `COMPARE` = 100 µs worth of
  ticks (`3686` at the default 36.864 MHz), `RegisterIntrHandler(16, mode=1,
  ...)`, `EnableIntr(16)`** is the exact, ordered boot-time programming
  sequence to reproduce.
- **The alarm queue is a single sorted doubly-linked list**, one hardware
  timer backing all of it (not one hardware timer per alarm) — every
  `SetAlarm`/expiry reprograms one shared compare register for whichever
  record is now earliest, with a ~200 µs coalescing/re-arm-in-the-future
  safety margin.
- **A callback's return value is a re-arm request, not just a status code** —
  zero frees the record, nonzero re-arms the *same* record `min(N, 200us
  worth of ticks)` later. `DelayThread`'s own callback must return zero.
- **`GetSystemTime`'s only proven-working contract in this firmware is
  pointer-based** (an internal convention this document surfaces, not part of
  the public ps2sdk prototype) — a rebuild should support that contract
  faithfully for `SetAlarm`/`DelayThread`'s own sake, and can define the
  public zero-argument path's behaviour freely.
- **No periodic preemption tick exists anywhere in this path** (§9) — a
  rebuild must not add one to make `spec/06` IOP-3h's involuntary-preemption
  branch "work harder" than the reference does; the reference relies entirely
  on opportunistic interrupts.

## Open questions

- **Whether `SCPH-50000`'s boot-parameter table actually carries a key-`7`
  record whose first halfword is `200`** — if it does, the resident clock rate
  is `25 MHz` flat rather than `36.864 MHz`, and every deadline computed by
  `USec2SysClock` would be scaled differently. Not resolved here; would need
  the same boot-record extraction `docs/analysis/09`/`38` used for
  `QueryBootMode` keys `3`/`4`.
- **Whether timer 5's documented `/256` prescale (`TIMEMANI`'s own descriptor
  table, §2) is a hardwired property of that specific counter or merely a
  ps2sdk-documented default that `THREADMAN`'s own `MODE=0x70` (no explicit
  prescale bits) overrides back to `/1`.** This matters directly for whether
  `USec2SysClock`'s `36.864 MHz` constant is internally consistent with what
  timer 5 actually ticks at once armed — genuinely not resolvable from static
  disassembly (a prescale divider is a hardware behaviour with no software
  trace beyond the mode bits already read), and not settled by the sibling
  project's own notes, which record *which* timer and IRQ are used but not its
  effective rate. Needs empirical verification: arm timer 5 as `THREADMAN`
  does under PCSX2 or PS2e and measure ticks against a known wall-clock
  interval.
- **The alarm-queue sentinel's own `+0x10`/`+0x14` content** (read as a
  "record" by `0x548c`'s empty-queue fallback, §6) was not independently
  confirmed — presumably initialised to a maximal or otherwise inert value so
  the shared code path behaves sanely with nothing pending, but the exact
  bytes were not traced.
- **`ReferHardTimer` (`timrman` ordinal 5) and `FreeHardTimer` (ordinal 6)**
  are structurally sketched (§1/§2) but not independently disassembled past
  confirming their existence and shape — neither is imported by `THREADMAN`
  or any other module this project has read.
- **Whether real ps2sdk-linked client code (a title, not this firmware) ever
  calls the public `GetSystemTime()` the documented zero-argument way, and
  what it would observe if so** (§7) — every call site *inside* this firmware
  uses the pointer-based internal contract instead, so the public path's
  actual behaviour for an external caller is characterised here but its
  intended use was not established.
- **`SetTimerMode`'s bit `0x40` ("Repeat?" per the header, unnamed with
  confidence) and bit `0x80` ("Clears interrupt bit on assertion?")** — bit
  `0x40` is set in `THREADMAN`'s own `0x70` mode value and bit `0x80` is not;
  neither bit's exact hardware effect was independently confirmed beyond what
  the header itself speculates.
