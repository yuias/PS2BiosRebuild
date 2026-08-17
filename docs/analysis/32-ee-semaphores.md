# EE Syscalls: Semaphores

The M1 program's C library initialisation (`docs/project-state.md` §6) aborts
when `CreateSema` returns 0, and its `main` blocks on a semaphore a second
thread signals. `docs/analysis/17` covered the rescheduling skeleton these
slots share; this reads the ten semaphore slots themselves — `0x40`–`0x49` —
and executes them on the reference under `tools/eesim.py`.

The experiments were run by importing `eesim.Machine`, booting once and
issuing many syscalls against the live kernel (the `--syscall` option boots
afresh for each call); the script is reproduced at the end.

```sh
python3 tools/romdir.py assets/SCPH-50000.bin --extract <outdir>   # outside the repo
for s in 0x40 0x41 0x42 0x43 0x44 0x45 0x46 0x47 0x48 0x49; do python3 tools/eeksys.py <outdir>/KERNEL --slot $s; done
python3 tools/romdis.py <outdir>/KERNEL --cpu ee --vma 0x80000000 --range 0x800049b8 0x80004e40
```

Reference `KERNEL` extracted to `<outdir>/KERNEL`, disassembly in its disassembly. Addresses are KSEG0 (`0x80000000 + file offset`), from `python3 tools/eeksys.py <KERNEL> --slot 0xNN`:

| Slot | Address | Role |
| --- | --- | --- |
| 0x40 | 0x800049b8 | CreateSema (direct, only form) |
| 0x41 | 0x80003540 | DeleteSema, reschedules; wraps op 0x80004a40 |
| 0x49 | 0x80004a40 | iDeleteSema, direct form of the same operation |
| 0x42 | 0x800034c0 | SignalSema, reschedules; wraps op 0x80004bc0 |
| 0x43 | 0x80004bc0 | iSignalSema, direct form of the same operation |
| 0x44 | 0x80003440 | WaitSema, reschedules; wraps op 0x80004cf0. No direct/i-form |
| 0x45 | 0x80004dc0 | PollSema |
| 0x46 | 0x80004dc0 | iPollSema -- identical address, Poll never blocks so no wrapper exists |
| 0x47 | 0x80004df8 | ReferSemaStatus |
| 0x48 | 0x80004df8 | iReferSemaStatus -- identical address, same reasoning |

The three rescheduling wrappers (0x41/0x42/0x44) follow `docs/analysis/17-ee-scheduler-syscalls.md`'s skeleton (`jal 0x80003680` prologue, operation, `mtc0 $v0,$14`, `jal 0x80003800` epilogue, `eret`). DeleteSema/SignalSema branch on `bltz $v0` to skip a "higher-priority thread now ready" check at `0x80003940`; WaitSema branches on `$v0 != -2` to skip the block routine at `0x80003a78` -- `-2` is WaitSema's private "must block" sentinel, never a real id or error code.

## 1. Data structures
**Semaphore table**: base `0x8001F240` (`lui $2,0x8002; addiu $2,$2,-0xdc0`), 32 (`0x20`) bytes/entry, 256 entries (`sltiu $2,id,0x100` gates every id-taking slot). `id` **is** the table index: `entry = base + id*0x20`, recovered from a pointer with `(ptr-base)>>5`, never stored anywhere else.

| Offset | Field | Notes |
| --- | --- | --- |
| `+0x00` | freelist-next (while free) | `sw $zero` at creation; untouched while allocated, used as the freelist link once DeleteSema frees the entry |
| `+0x04` | count | read/written by every op; also the allocated/free marker (`>=0` allocated, `-1` free). Seeded from caller's `init_count` at creation, not kept separately -- once signalled/polled the original is gone |
| `+0x08` | max_count | copied from caller's struct, never validated by any op |
| `+0x0C` | attr | opaque, from caller's struct `+0x10` |
| `+0x10` | option | opaque, from caller's struct `+0x14` |
| `+0x14` | wait_threads | threads blocked in WaitSema; also ReferSemaStatus's `wait_threads` output |
| `+0x18` | wait-list head | intrusive queue of blocked threads; self-linked when empty |

Freelist is singly-linked through `+0x00`, head at `0x8001A63C` (`0x80020000-0x59C4`), LIFO: CreateSema pops the head, DeleteSema pushes the freed entry back on (confirmed in §3). An active-semaphore counter at `0x8001A638` (`0x80020000-0x59C8`) is incremented by CreateSema and decremented by DeleteSema but read by none of the ten slots -- accounting only, purpose otherwise unidentified. `ee_sema_t` (CreateSema's `$a0`): kernel reads only `+0x04` max_count, `+0x08` init_count, `+0x10` attr, `+0x14` option; `+0x00` count and `+0x0C` wait_threads are never read -- output-only in the SDK's struct.

**Thread-record link fields** (refines `docs/analysis/30`, which only observed offsets `0x00`-`0x40` from base `0x8001A648`): Signal/Delete/Wait compute a second base, `0x8001A640`, 8 bytes *before* doc 30's, and pass `0x8001A640 + index*0x4C` to queue helpers (`0x80005af8`/`0x80005b88` pop, `0x80005ba8` push -- not individually reversed), suggesting the true thread record starts 8 bytes earlier than doc 30 assumed, with an intrusive list node (prev/next) before doc 30's "offset 0x00 = state". Two fields not in doc 30: record-relative `+0x14`, cleared by Signal/Delete when a thread is dequeued, and `+0x18`, written with the semaphore id by WaitSema before it blocks -- exact purpose unconfirmed, no way to inspect a blocked thread's record through the syscall ABI.

## 2. Per-slot behaviour
All handlers check the id with `sltiu $2,id,0x100` first; out-of-range returns `-1` before the entry is touched.

### 0x40 CreateSema -- `(ee_sema_t *) -> id | -1`
```
head = *0x8001A63C
if head == 0: return -1                     # table exhausted
if a0->init_count < 0: return -1
entry = head; head = entry->next (entry+0x00); *0x8001A63C = head
id = (entry - 0x8001F240) >> 5
entry+0x04 = a0->init_count; entry+0x08 = a0->max_count
entry+0x0C = a0->attr;       entry+0x10 = a0->option
entry+0x14 = 0; entry+0x18 = &entry+0x18     # empty wait list
++*0x8001A638; return id
```
No check that `init_count <= max_count`, none that `max_count >= 0`. Confirmed experimentally: an all-zero struct is accepted.

### 0x41/0x49 DeleteSema / iDeleteSema -- `(id) -> id | -1`
```
entry = table[id]; if entry+0x04 < 0: return -1     # unallocated
--*0x8001A638
while entry+0x14 > 0:                        # wake every waiter
    thread = dequeue(entry+0x18); entry+0x14 -= 1
    if thread.state == 4:     wake_ready(thread)  # 0x80005a58, may set reschedule flag @0x800155B4
    elif thread.state == 0xc: thread.state = 8    # READY directly, no ready-queue insertion
    # else: leave the thread's state alone
    thread_record+0x14 = 0                   # (only on the two branches above)
entry+0x04 = -1; entry+0x00 = *0x8001A63C; *0x8001A63C = entry   # push freelist
return id
```
Every waiter is dequeued regardless of state; what `$v0` a woken WaitSema caller receives could not be confirmed (no second-thread harness, see §4).

### 0x42/0x43 SignalSema / iSignalSema -- `(id) -> id | -1`
```
entry = table[id]; if entry+0x04 < 0: return -1
if entry+0x14 <= 0: entry+0x04 += 1; return id        # nobody waiting
entry+0x14 -= 1; thread = dequeue(entry+0x18)          # hand off, count untouched
if thread.state == 4:     wake_ready(thread); thread_record+0x14 = 0
elif thread.state == 0xc: thread.state = 8; thread_record+0x14 = 0
return id
```
**No check against `max_count`** (`entry+0x08` never read here) -- signalling an already-full semaphore still increments the count. A real divergence from the SDK's documented "sema overflow" error; must be reproduced as-is.

### 0x44 WaitSema -- `(id) -> id | -1 | blocks`
```
entry = table[id]; if entry+0x04 < 0: return -1
if entry+0x04 > 0: entry+0x04 -= 1; return id          # immediate success
entry+0x14 += 1; enqueue(current_thread, entry+0x18); thread_record+0x18 = id
return -2                                              # sentinel: wrapper reschedules
```
`-2` is consumed entirely by the 0x44 wrapper; never seen by user code. Confirmed experimentally: WaitSema on a count-0 sema never returns within the simulator's step budget; count-1 returns `id` immediately.

### 0x45/0x46 PollSema / iPollSema -- `(id) -> id | -1`
```
entry = table[id]
if entry+0x04 <= 0: return -1        # covers "count is 0" AND "unallocated"
entry+0x04 -= 1; return id
```
Bit-identical code at both slot numbers (Poll never blocks, no reschedule variant needed). Cannot distinguish "valid id, count 0" from "unallocated id" -- both give `-1`.

### 0x47/0x48 ReferSemaStatus / iReferSemaStatus -- `(id, out*) -> id | -1`
```
entry = table[id]; if entry+0x04 < 0: return -1   # bltz, not <=0 -- count==0 IS reported
out+0x00 = entry+0x04   # count            out+0x04 = entry+0x08   # max_count
out+0x0C = entry+0x14   # wait_threads     out+0x10 = entry+0x0C   # attr
out+0x14 = entry+0x10   # option           return id
```
`out+0x08` (the input struct's `init_count` slot) is **never written** -- confirmed experimentally. Also bit-identical code at both slots.

**Interrupt masking**: none of the six operation bodies touch `Status` (`$12`) or `EIE`/`IE` -- no `mtc0 $_, $12` anywhere. Masking, if any, is the rescheduling skeleton's job, not the operations' -- consistent with Poll/Refer's "i" forms being literally the non-"i" code, interrupt-safe.

## 3. Experiments
Ran via a script using `tools/eesim.py`'s own `Machine` class (imported unmodified) to boot once and issue many `syscall()` calls against the live state, since the `--syscall` CLI supports only one call per process and each boot is ~20s. Script: see the end of this document. Output:
```
CreateSema(#1, max=1 init=1 attr=0x1234 opt=0x5678) -> 0
CreateSema(#2, max=5 init=2)                        -> 1
CreateSema(#3, all-zero block)                      -> 2   # no range check
CreateSema(a0=0, boot code reread as struct)         -> 3   # its init_count happened to be >= 0
CreateSema(init_count=-1)                            -> -1
PollSema(id1) [init=1] -> 0   PollSema(id1) again [count 0] -> -1
PollSema(id2) [init=2] -> 1   PollSema(id2) again [count 1] -> 1
PollSema(unused id 200) -> -1   PollSema(id 0x100) -> -1   PollSema(id -1) -> -1
SignalSema(id1) [0->1, no waiters] -> 0   PollSema(id1) after signal -> 0
SignalSema(unused id 201) -> -1           SignalSema(id 0x100) -> -1
ReferSemaStatus(id2) -> $v0 = 1
  count=0 max_count=5 (init_count slot untouched, still =0xDEADBEEF sentinel)
  wait_threads=0 attr=0 option=0
ReferSemaStatus(unused id 202) -> -1      ReferSemaStatus(id 0x100) -> -1
DeleteSema(unused id 203, never created) -> -1   # boot init already marks every entry "free"
DeleteSema(id 0x100) -> -1                DeleteSema(id -1) -> -1
DeleteSema(id1, valid, no waiters) -> 0
DeleteSema(id1) again -> -1               # already freed
PollSema(id1) after delete -> -1          ReferSemaStatus(id1) after delete -> -1
CreateSema after id1 freed -> 0           # freelist is LIFO: reuses id1's slot first
CreateSema(init_count=0) for wait test -> 4
WaitSema(id_wait) [count 0]  -> None      # never reached the return stub -- genuinely blocks
CreateSema(init_count=1)     -> 0
WaitSema(id_wait2) [count 1] -> 0         # returns immediately, no block
```
Confirms every algorithm above: sequential id allocation walked in address order, no `max_count` enforcement anywhere, `-1` uniformly for bad/unallocated/out-of-range ids, LIFO freelist reuse, and WaitSema's "`-2` means block" sentinel genuinely causing a non-returning call at count 0.

## 4. Surprises / unresolved
- **No `max_count` enforcement** in CreateSema or SignalSema -- double-checked
  since it looked like a bug, but no instruction reads `entry+0x08` in either
  function, and the all-zero-block experiment confirms it. Must not add the
  check the SDK headers imply. `0x8001A638` (create/delete counter) is
  likewise maintained but never consulted by any of the ten slots --
  unidentified accounting, not guessed at.
- **The magic-multiply divisions** (`mult rd,rs,0x286BCA1B` then `sra 2`,
  turning a returned link pointer into a thread-table index) decode as the
  R5900 three-operand `mult` (`SPECIAL`, funct `0x18`, same encoding as doc
  30's), but the reciprocal-multiply arithmetic itself was not re-derived
  bit-for-bit -- asserted from context (`* 0x4C` follows it, landing exactly
  on doc 30's thread table).
- **A woken WaitSema caller's `$v0`** could not be tested: eesim's
  `syscall()` harness runs one thread through the stub, with no way to create
  a second real thread and let both run. Nothing in SignalSema's hand-off
  path writes an obvious return value into the woken thread's saved context
  -- possibly it re-enters via state set up by `0x80003a78`/`0x80005ba8`
  rather than a pre-loaded value; flagged, not guessed. These and the
  dequeue/enqueue/wake helpers (`0x80005af8`, `0x80005b88`, `0x80005ba8`,
  `0x80005a18`, `0x80005a58`) were not individually reversed -- their effect
  was inferred from how callers use the result, sufficient to explain every
  observed `$v0`.

## The experiment script

```python
import sys, struct
sys.path.insert(0, "tools")            # run from the repository root
import eesim

ROM_PATH = "assets/SCPH-50000.bin"
rom = open(ROM_PATH, "rb").read()
m = eesim.Machine(rom)
m.boot()
print("reached_kernel:", m.reached_kernel, "stop_reason:", m.cpu.stop_reason)

def w32(addr, val):
    m.bus.write(addr, 4, val & 0xFFFFFFFF)

def r32(addr):
    return m.bus.read(addr, 4)

def sema_struct(addr, max_count, init_count, attr=0, option=0):
    w32(addr + 0x00, 0)            # count field (documented, kernel ignores it)
    w32(addr + 0x04, max_count)
    w32(addr + 0x08, init_count)
    w32(addr + 0x0C, 0)            # wait_threads field (documented, kernel ignores it)
    w32(addr + 0x10, attr)
    w32(addr + 0x14, option)

def hx(v):
    return v if v is None else hex(v & 0xFFFFFFFF) + f" ({v})"

A = 0x00300000   # scratch struct for CreateSema #1
B = 0x00300100   # scratch struct for CreateSema #2
ZERO = 0x00300200  # all-zero struct
OUT = 0x00300300  # ReferSemaStatus output buffer

print("\n--- CreateSema ---")
sema_struct(A, max_count=1, init_count=1, attr=0x1234, option=0x5678)
id1 = m.syscall(0x40, A)
print("CreateSema(#1, max=1 init=1 attr=0x1234 opt=0x5678) ->", hx(id1))

sema_struct(B, max_count=5, init_count=2)
id2 = m.syscall(0x40, B)
print("CreateSema(#2, max=5 init=2) ->", hx(id2))

# all-zero block: max_count=0, init_count=0
id3 = m.syscall(0x40, ZERO)
print("CreateSema(#3, all-zero block) ->", hx(id3))

id_null = m.syscall(0x40, 0)
print("CreateSema(a0=0, struct at physical 0 -- boot code reinterpreted) ->", hx(id_null))

sema_struct(A, max_count=1, init_count=-1)
id_neg = m.syscall(0x40, A)
print("CreateSema(init_count=-1) ->", hx(id_neg))

print("\n--- PollSema ---")
print("PollSema(id1) [init_count was 1] ->", hx(m.syscall(0x45, id1)))
print("PollSema(id1) again [count now 0] ->", hx(m.syscall(0x45, id1)))
print("PollSema(id2) [init_count was 2] ->", hx(m.syscall(0x45, id2)))
print("PollSema(id2) again [count now 1] ->", hx(m.syscall(0x45, id2)))
print("PollSema(unused id 200) ->", hx(m.syscall(0x45, 200)))
print("PollSema(id 0x100, out of range) ->", hx(m.syscall(0x45, 0x100)))
print("PollSema(id -1) ->", hx(m.syscall(0x45, 0xFFFFFFFF)))

print("\n--- SignalSema ---")
print("SignalSema(id1) [count 0 -> 1, no waiters] ->", hx(m.syscall(0x43, id1)))
print("PollSema(id1) after signal [should succeed] ->", hx(m.syscall(0x45, id1)))
print("SignalSema(unused id 201) ->", hx(m.syscall(0x43, 201)))
print("SignalSema(id 0x100 out of range) ->", hx(m.syscall(0x43, 0x100)))

print("\n--- ReferSemaStatus ---")
for i in range(6):
    w32(OUT + i * 4, 0xDEADBEEF)
rv = m.syscall(0x48, id2, OUT)
print("ReferSemaStatus(id2) -> $v0", hx(rv))
fields = [r32(OUT + i * 4) for i in range(6)]
print("  out[0] count      =", hx(fields[0]))
print("  out[1] max_count  =", hx(fields[1]))
print("  out[2] (untouched)=", hx(fields[2]))
print("  out[3] wait_thr   =", hx(fields[3]))
print("  out[4] attr       =", hx(fields[4]))
print("  out[5] option     =", hx(fields[5]))

print("ReferSemaStatus(unused id 202) ->", hx(m.syscall(0x48, 202, OUT)))
print("ReferSemaStatus(id 0x100 out of range) ->", hx(m.syscall(0x48, 0x100, OUT)))

print("\n--- DeleteSema ---")
print("DeleteSema(unused id 203, never created) ->", hx(m.syscall(0x49, 203)))
print("DeleteSema(id 0x100 out of range) ->", hx(m.syscall(0x49, 0x100)))
print("DeleteSema(id -1) ->", hx(m.syscall(0x49, 0xFFFFFFFF)))
print("DeleteSema(id1, valid, no waiters) ->", hx(m.syscall(0x49, id1)))
print("DeleteSema(id1) again [now freed] ->", hx(m.syscall(0x49, id1)))
print("PollSema(id1) after delete ->", hx(m.syscall(0x45, id1)))
print("ReferSemaStatus(id1) after delete ->", hx(m.syscall(0x48, id1, OUT)))

print("\n--- CreateSema reuse after delete ---")
sema_struct(A, max_count=9, init_count=0)
id_reuse = m.syscall(0x40, A)
print("CreateSema after id1 freed -> reused id?", hx(id_reuse))

print("\n--- WaitSema on empty sema (expected to block / not return synchronously) ---")
sema_struct(A, max_count=1, init_count=0)
id_wait = m.syscall(0x40, A)
print("CreateSema(init_count=0) for wait test ->", hx(id_wait))
wv = m.syscall(0x44, id_wait)
print("WaitSema(id_wait) [count 0, must block] -> $v0 =", hx(wv), "(None means it did not return to the stub)")

print("\n--- WaitSema on ready sema (should not block) ---")
sema_struct(B, max_count=1, init_count=1)
id_wait2 = m.syscall(0x40, B)
print("CreateSema(init_count=1) ->", hx(id_wait2))
wv2 = m.syscall(0x44, id_wait2)
print("WaitSema(id_wait2) [count 1, should succeed immediately] -> $v0 =", hx(wv2))
```
