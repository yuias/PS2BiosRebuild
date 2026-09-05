# EE Syscalls: Threads and the Context Switch

`docs/analysis/17` found the rescheduling skeleton and `30` the thread record
as slot `0x3C` uses it. This reads the thread slots themselves — `0x20`–`0x3A`
— the shared prologue and epilogue the skeleton calls, and the scheduler's
pick, so that `spec/04` EE-7g's context switch can be built rather than
inferred. Every claim below is from the reference kernel's code, with the
return values executed under `tools/eesim.py` where a single thread can
observe them.

```sh
python3 tools/romdir.py assets/SCPH-50000.bin --extract <outdir>   # outside the repo
for s in 0x20 0x21 0x22 0x23 0x24 0x25 0x26 0x29 0x2a 0x2b 0x2c 0x2d 0x2e 0x2f 0x30 0x31 0x32 0x33 0x34 0x35 0x36 0x37 0x38 0x39 0x3a; do python3 tools/eeksys.py <outdir>/KERNEL --slot $s; done
python3 tools/romdis.py <outdir>/KERNEL --cpu ee --vma 0x80000000 --range 0x80003680 0x80003a00
python3 tools/romdis.py <outdir>/KERNEL --cpu ee --vma 0x80000000 --range 0x80003bb8 0x80004a00
```

## 1. Data structures

**Thread record**: base `T=0x8001A648`, stride `0x4C`, 256 records,
`T[id]=T+id*0x4C`:

| Off | Width | Field |
| --- | --- | --- |
| `-0x8`/`-0x4` | 32/32 | ready-queue/free-list link node (`next`/`prev`), *before* the record |
| `+0x00` | 32 | state: `0`=free,`1`=RUN,`2`=READY,`4`=WAIT,`8`=SUSPEND,`0xC`=WAIT_SUSPEND,`0x10`=DORMANT |
| `+0x04` | 32 | resume PC — entry point at creation; overwritten with post-syscall EPC each time the thread yields |
| `+0x08` | 32 | context: saved `$sp`, pointing at the thread's 0x2A0-byte frame |
| `+0x0C` | 32 | `$gp` |
| `+0x10`/`+0x12` | 16/16 | initial / current priority (`lh`/`sh`) |
| `+0x14` | 32 | wait-type: `1`=plain delay(Sleep), `2`=in a wait-object queue (refcounted), `0`=not waiting |
| `+0x18` | 32 | wait-id: index into a wait-object table at `0x8001F254`, stride `0x20` |
| `+0x1C` | 32 | wakeup count (pending-wakeup counter) |
| `+0x20`/`+0x24` | 32/32 | attr / option |
| `+0x28` | 32 | saved original entry point (restored to `+0x04` on reset-to-dormant) |
| `+0x2C`/`+0x30` | 32/32 | argc / argv-arg-list ptr (StartThread also stashes its raw `arg` here) |
| `+0x34`/`+0x38` | 32/32 | stack base / stack size |
| `+0x3C` | 32 | root — return address for a returning thread function; `CreateThread` copies the creator's, `0x3bb8` re-primes the frame's `$ra` from it |
| `+0x40` | 32 | heap end — `CreateThread` copies the creator's (§5) |

Confirmed field-for-field by decoding `ReferThreadStatus` (copies `T[id]`
into PS2SDK's 12-field `ee_thread_status_t` offset for offset), cross-
checked against `CreateThread`, `0x80003bb8`, and `ReleaseWaitThread`.

**Ready queue**: `queueHeads[p]=0x8001A230+p*8`, 129 entries (priorities
0..128), a circular doubly-linked sentinel list per priority (empty when
`head.next==&head`); a thread's link node is `T[id]-8/-4`. **The running
thread is one of them.** Nothing takes a thread off its list to run it: a
list holds every thread at that priority that is neither waiting, suspended
nor dormant, and the running one is the head of the lowest-numbered
non-empty list. Only a blocking, suspending, ending or priority-changing
operation unlinks, and each does so itself. 129 not 127:
user code only *sets* `0..127` (`ChangeThreadPriority` rejects `128`), but
the boot thread sits at `128` (`ChangeThreadPriority(0,5)`→old priority
`128`). **`0x800155B0`**: cached lowest ready priority (`min()`'d on
enqueue; the pick loop starts here). **`0x800155B4`**: a "reschedule
needed" flag set by ChangeThreadPriority/ReleaseWaitThread/WakeupThread/
ResumeThread but not read anywhere traced (§5). List helpers (shared by
the ready queue *and* the free-slot list below): `0x80005AF8` pop-front,
`0x80005B38` pop-front+append-tail=rotate, `0x80005B88` unlink-arbitrary,
`0x80005BA8` append-tail, `0x80005A18` dequeue-by-id (iff
`state∈{RUN,READY}`), `0x80005A58` enqueue-ready (`state=2`, `min()`s
priority into `0x800155B0`, appends to the queue's tail — so a thread
`StartThread` makes ready goes *behind* the caller that started it, and a
thread that lowers its own priority through `ChangeThreadPriority`, which
dequeues and re-enqueues whatever its state, goes behind its new peers).

**Free thread-slot list**: `0x8001A228`. `CreateThread` pops a node (same
primitive). `0x4C` has no power-of-two factor, so the index is recovered
via a compiler-style reciprocal multiply, `mult rd,rs,0x286BCA1B`
(`lui $3,0x286b; ori $3,$3,0xca1b`) then `sra rd,rd,2`; the reverse just
does `id*0x4C` with a real `mult`. IDs are plain small integers, `0..255`.

**Other globals**: `0x800155AC` current-thread index (`GetThreadId`
returns it unvalidated); `0x8001A220` a "threads created" counter;
`0x800010C0` the frame pointer most recently saved by the prologue
(self-resuming handlers reload `$sp` from it instead of `$v1`);
`0x800010D0`/`0x800010E0` the vector's stashed original `$ra`/`$at`
(spec/04 EE-7d), consumed by the prologue.

## 2. Context switch: prologue `0x80003680`, epilogue `0x80003800`

**Frame** (0x2A0 bytes): 32 GPRs, 16B/register (`sq`/`lq`, 128-bit — MMI
needs the upper half back). `$zero`/`$k0`/`$k1` are meaningless across a
syscall, repurposed: `$zero` (`+0x0` SA via `mfsa`/`mtsa`, `+0x4` FCR31,
`+0x8` FPU accumulator ACC — extracted via `mtc1 $zero,$f0; madd.s
$f0,$f0,f1` (`fd=ACC+0*ft`), restored via `adda.s $f0,$f0,f1` with
`f1=-0.0`); `$k0`/`$k1` (`+0x1A0/+0x1A8` HI/HI1, `+0x1B0/+0x1B8` LO/LO1).
Then 32 FPRs at `+0x200..+0x27C`; `$gp/$sp/$fp/$ra`=`+0x1C0/+0x1D0/
+0x1E0/+0x1F0` — matches doc30's "0x200 + 0xA0".

**Prologue**: `$k0` starts as `*0x800010C0-0x280`. Every GPR is `sq`'d
live **except `$at`** (from `0x800010E0` — the vector already clobbered
it) and `$ra` (from `0x800010D0`). Last instruction, `sw $k0,0x10c0($1)`,
publishes the new frame as `0x800010C0`.

**Epilogue** runs with `$sp` **already pointing at the frame to
restore**: mirror image, `lq`s replacing `sq`s, **including `$sp`/`$ra`**
— `$ra` saved into `$26` first (`move $26,$ra`) so the frame's own value
can overwrite it, `$sp` restored in the final `jr $26`'s delay slot.
Matches `spec/04` EE-7e2's "`$v1` not restored" only for the *simple*
non-rescheduling path; here it's restored like any GPR.

**How a wrapper decides to switch, and how the next thread is picked** —
two wrapper shapes, confirmed by reading the code, not just doc17's
generic skeleton:

- **Shape A, "always yield"** (confirmed: StartThread 0x22 — `0x80003040`
  is instruction-for-instruction TerminateThread's `0x80003140` —
  TerminateThread 0x25,
  ChangeThreadPriority 0x29, RotateThreadReadyQueue 0x2B; near-certain for
  ReleaseWaitThread/WakeupThread/ResumeThread, same return convention):
  `jal 0x3680`(save)→`jal <op>`→`bltz $2,<no-switch>`(op returned -1)→
  `mfc0 $4,$14`(EPC)→`jal 0x3940($a0=$4,$a1=*0x800010c0)` (READY-tail:
  mark self READY, pick next)→`mtc0 $2,$14`→`jal 0x3800`→`move $sp,$3`→
  `eret`. `<no-switch>` reloads `$sp` from `0x800010C0` and stores `-1` as
  `$v0`. **Checks only `bltz $v0`, never `0x800155B4`** — any success
  yields unconditionally, relying on the pick loop to re-select the
  caller if still best.
- **Shape B, "always exit"** (ExitThread 0x23, ExitDeleteThread 0x24): no
  `bltz` at all — these operations never return to their own caller.

`0x80003940`(park-ready, `$a0`=resumePC,`$a1`=frame) and `0x80003A78`
(park-waiting, `+$a2`=waitType) mark the outgoing thread READY(2)/WAIT(4),
store waitType (0x3A78 only), then: read cursor `c=*0x800155B0`; if
`c>=129`, nothing ready — `0x800073e0`(message at `0x80015B10`) then
`0x80000d80`(fatal, not decoded further); else scan `queueHeads[c..128]`
for the first non-empty list, persist `c`; **read** that list's head,
recover its index (magic-multiply), set `0x800155AC`,
`T[picked].state=1`(RUN); **if
`T[picked]+0x14`(waitType) is nonzero**, clear it and force
`*(T[picked].context+0x20)=-1` (its own saved `$v0` slot) — how a forced
release makes the woken thread's blocking call return `-1`; return
`$v0=T[picked]+0x04`, `$v1=T[picked]+0x08`'s value. **Neither routine
touches a list**: the outgoing thread is left exactly where it is, and the
incoming one is read, not popped — `lw $2,0x0($3) / bne $2,$3` is the
empty test on the sentinel's `+0` (tail) link and its delay slot's
`lw $2,0x4($3)`, an ordinary branch's so always taken, is the `+4` (head)
the code goes on to use. That is why a Shape A slot reselects its own
caller: the caller is still that list's head. It also means a thread that
parks itself WAITING must have been unlinked by the operation that decided
to wait — `0x80005B88` — before the park runs. `SleepThread` calls `0x3a78`
directly (not via a wrapper `bltz`) with `waitType=1`.
`ExitThread`/`ExitDeleteThread` inline their own copy of the pick loop
(an exiting thread has no resume state to save).

Direct-form (`i`-) slots call the bare operation, `jr $ra`. Three pairs
are **true EE-8c aliases** (identical handler address): `0x30`/`0x31`,
`0x35`/`0x36`, `0x37`/`0x38` — none touch the ready queue.

## 3. Per-slot behaviour

**GetThreadId — 0x2F, `0x80004540`**: `(-)->$v0=*0x800155AC`. `eesim`: **0**.
**ReferThreadStatus/i — 0x30/0x31, `0x80004550` (true alias)**: `$a0==0`
means self (`movz $v1,$v0,$v1`); `id<u256` else **-1**; `$a1==0` returns
`T[id].state` only, else fills the block per §1. `(0,0x80100000)→2`
(self=READY, not RUN, §5); `(300,0)→-1`; `(1,0)→0` (never created).
**CreateThread — 0x20, `0x80003c50`**: `($a0=ee_thread_t*)->$v0`. Pops
the free list, **-1** if empty. `state=0x10`(DORMANT);
`waitId,wakeupCount,argc,argv,waitType` zeroed; copies `stack_size(+0xC)`,
`stack(+0x8→+0x34)`, `func(+0x4→`both`+0x04,+0x28)`, `gp_reg(+0x10→
+0x0C)`, `initial_priority(+0x14`,halfword`→`both`+0x10,+0x12)`. Primes a
0x2A0-byte frame at `top-0x2A0`: `$gp`=gp_reg, `$sp`=`$fp`=`top-0x20`.
`$ra`/`root(+0x3C)` seeded from an unresolved kernel value, not the input
struct (§5). **No priority range check**: a garbage struct (priority
decoding to 53345) still succeeds, returns id `1` — validation is
ChangeThreadPriority's job alone.
**DeleteThread — 0x21, `0x80002fc0`, op `0x80003ef8`**: `($a0=id)->$v0`.
`id∈[1,255]`, `id≠current`, `state==0x10` else **-1**. Success: `0x59d8`
(delete-and-free: returns record to free list, decrements live count),
returns `id`. `300→-1`(range); `1→-1`(not DORMANT); `0→-1`(is current).
**StartThread — 0x22, `0x80003040`, op `0x80003f68`**: `($a0=id,$a1=arg)
->$v0`. `id∈[1,255]`, `id≠current`, `state==0x10` else **-1**. Writes
`arg` into the frame's `$a0` slot (`+0x40`) *and* `T[id]+0x30`, so the new
thread's entry function sees `arg` as its `$a0`. `enqueueReady`, i.e. the
**tail** of the new thread's priority queue, behind the caller when the two
share a priority. Shape A. Returns
`id`. `(1,0)→-1` (never created).
**ExitThread — 0x23** (Shape B, no direct pair): `(-)->$v0?`. Dequeues
self, `0x80003bb8` resets to DORMANT, runs the pick loop inline.
`0x80003bb8` (also TerminateThread's finalize): rewrites `+0x04` from
`+0x28`; rebuilds the `+0x08` frame; resets `+0x12` from `+0x10`; primes
`$ra`/`root` from the §5-unresolved source; `state=0x10` — restartable
via StartThread, like a fresh thread.
**ExitDeleteThread — 0x24** (Shape B): same shape, calls `0x59d8`
(DeleteThread's helper) instead of `0x3bb8` — record freed outright.
**TerminateThread/i — 0x25/0x26, op `0x80003df8`**: `($a0=id)->$v0`.
`id∈[1,255]` else **-1**. 17-entry jump table at `0x80015B30` on state:
`0`/`0x10`→**-1**; `1`(RUN)→sets `0x800155B4`, finalize; `2`(READY)→
dequeue, finalize; `4`/`0xC`→if `waitType==2` unlink+decrement wait-
object, finalize; `8`→finalize. Finalize=`0x3bb8`. Returns `id`. Shape A.
`0→-1` (hard-rejected, unlike ChangeThreadPriority's self=0); `1→-1`.
**ChangeThreadPriority/i — 0x29/0x2A, op `0x80004280`**: `($a0=id,
$a1=priority)->$v0`. `id==0` means **current thread**, via
`movz $16,$7,$16` (raw word `0x00f0800a` at `0x800042c8`, SPECIAL funct
`0x0A`: rd=rt=`$16`=id, rs=`$7`=physically `$a3`) — **but two instructions
earlier** (`lui $7,0x8001; lw $7,0x55ac($7)`, `0x800042b8`) `$7`'s register
was already overwritten with `*0x800155AC` (current-thread-index), so the
value substituted for `id==0` is always "current thread", never the
caller's actual `$a3`. This is precisely spec/05's own caveat about
SYS-1a: a static liveness pass sees `$a3`'s register read by the `movz`
and lists it as a live argument, without seeing that the register had
already been reassigned — the caller's `$a3` input is never consulted.
`id<u256`; `0<=priority<128` else **-1**; existence: `state`
not `0`/`0x10` else **-1**. Returns the **previous priority**, not `id`.
If READY: dequeue+re-enqueue at new priority. Shape A. `(0,999,0)
→-1`; `(0,128)→-1`(128 unreachable as target); `(0,5,0)→128`(old priority;
boot starts at 128).
**RotateThreadReadyQueue/i — 0x2B/0x2C, op `0x80004380`**: `($a0=
priority)->$v0`, `<u128` else **-1**. Calls `0x5b38` on
`queueHeads[priority]` — plain round robin, no record fields touched.
Returns `priority`; empty-queue rotate is a no-op. Shape A **confirmed
directly** — every valid rotate yields and re-scans, even if the rotated
thread wasn't running. `999→-1`; `5→5`(valid, empty, still yields).
**ReleaseWaitThread/i — 0x2D/0x2E, op `0x800043c8`**: `($a0=id)->$v0`,
`id∈[1,255]` else **-1**. Jump table at `0x80015C00`: `0`→**-1**; `4`
(WAIT)→if `waitType==2` unlink+decrement, then **unconditionally**
enqueue-ready (WAIT→READY), flag if it beats the cursor; `0xC`→same
gated unlink but `state=8`(SUSPEND) directly (no enqueue — still needs
ResumeThread). Other states→silent no-op, returns `id`. `0→-1`.
**SleepThread — 0x32, `0x800032c0`, op `0x80004650`**: `(-)->$v0`. Reads
`wakeupCount(+0x1C)`; if `>0`, decrement, return **currentIndex** without
sleeping; if `<=0`, dequeue self, return **-1** — wrapper's `bltz` then
calls `0x3a78($a0=`current EPC`,$a1=frame,$a2=1)`. `eesim` (sole thread):
**did not return** — a real block.
**WakeupThread/i — 0x33/0x34, op `0x800046a8`**: `($a0=id)->$v0`,
`id<u256` else **-1**. `4`(WAIT) with `waitType==1`: enqueue-ready, clear
waitType, flag if improved, return `id`. `4` with `waitType≠1`, `2`, or
`8`: **increment** `wakeupCount`, return `id` — the pending-wakeup
counter confirmed directly. `0xC` with `waitType==1`: `state=8`, clear
waitType. `1`/`0`/`0x10`/other: **-1**. `0→0` (boot is READY→counter path).
**CancelWakeupThread/i — 0x35/0x36, `0x80004968`(true alias)**: `id<u256`
else **-1**. Returns the **previous** `wakeupCount`, resets to 0. No
state check. `0→0`.
**SuspendThread/i — 0x37/0x38, `0x80004800`(true alias)**: `id∈[1,255]`
else **-1**. `1`/`2`→dequeue, `state=8`. `4`→`state=0xC`, no dequeue
(link kept). `0,8,0xC,0x10`,other→**-1** (double-suspend, dormant/
nonexistent, and no reschedule path exists so self-suspend also fails).
Returns `id`. `0→-1`.
**ResumeThread/i — 0x39/0x3A, op `0x800048b8`**: `($a0=id)->$v0`,
`id∈[1,255]`, `id≠current` else **-1**. `8`→enqueue-ready, flag if
improved. `0xC`→`state=4` directly, no enqueue, no flag. Other: **-1**.
Returns `id`. `0→-1` (boot never suspended).
## 4. Experiments run under eesim

`python3 tools/eesim.py assets/SCPH-50000.bin --syscall <N> [args...]`:

```
0x2f                    -> 0     0x30 0 0x80100000 -> 2    0x30 300 0 -> -1
0x32                    -> (hangs, did not return)
0x33 0 -> 0   0x35 0 -> 0   0x38 0 -> -1   0x3a 0 -> -1
0x2a 0 128 -> -1   0x2a 0 5 0 -> 128   0x2e 0 -> -1   0x26 0 -> -1
0x2c 999 -> -1   0x2c 5 -> 5   0x21 0 -> -1   0x22 1 0 -> -1
0x20 0x80000000 -> 1  (garbage-struct CreateThread still succeeds)
```

## 5. Surprises / unresolved

- **Boot thread's state is READY (2), not RUN (1)** — `ReferThreadStatus(0)`
  →2, and `WakeupThread(0)` takes the already-ready counter path, not the
  RUN-only `-1`. The pick loop is what normally sets RUN; the boot thread
  was seeded directly by kernel init without going through it.
- **Reschedule wrappers never read `0x800155B4`** — only `bltz $v0`; every
  successful Terminate/ChangeThreadPriority/Rotate yields unconditionally,
  relying on the pick loop to re-select the caller. The flag's reader
  wasn't found; likely the interrupt-return path for `i`-forms that can't
  switch immediately — worth checking spec/04's interrupt-return code.
- **`CreateThread` copies the creator's `root` (`+0x3C`) and heap end
  (`+0x40`) into the new record** (resolved 2026-09-05). The word at
  `0x80003d48` that `romdis` cannot decode is `0x00e93818`, the R5900
  three-operand `mult $7, $7, $9` scaling the current-thread index by
  `0x4C`; `0x80003d9c`/`0x80003da4` then move `T[cur]+0x40` to
  `T[new]+0x40`, and `0x80003dac`/`0x80003dbc`/`0x80003dc4` move
  `T[cur]+0x3C` to the frame's `$ra` slot and `T[new]+0x3C`. The
  reset-to-dormant helper `0x80003bb8` re-primes `$ra` from `T[id]+0x3C`
  (`0x80003c10`/`0x80003c24`), so the root survives exit and restart. For a
  program's threads the root is therefore what its runtime passed to slot
  `0x3C`. Observed on the title: a thread created by the main thread reads
  the main thread's `0x1fc0000` back from `0x3E`.
- `SleepThread`'s reschedule decision is inside its own operation (the
  wakeup-count check), not its wrapper — unlike Terminate/ChangePriority/
  Rotate, where the wrapper's `bltz` is purely an id/range check.
- `0x800073e0`/`0x80000d80` ("no runnable thread" panic path) located but
  not decoded further.

## The dispatcher does not bound the number, and neither does `0x74`

Found on the way, because the SDK's runtime depends on it: its
`InitTLBFunctions` installs handlers at slots `0x7F` and `0x82` through slot
`0x74` and then calls them, although `spec/04` EE-8a's table has 125 entries.

```sh
python3 tools/romdis.py <outdir>/KERNEL --cpu ee --vma 0x80000000 --range 0x800002ec 0x80000300
python3 tools/romdis.py <outdir>/KERNEL --cpu ee --vma 0x80000000 --range 0x800006c4 0x800006d4
```

```
800002ec  sll   $v1, $v1, 0x2       # number x 4, no comparison anywhere
800002f0  lui   $k0, 0x8001
800002f4  addu  $k0, $k0, $v1
800002f8  lw    $k0, 0x4f40($k0)    # table[number]
800002fc  jalr  $k0
    ...
800006c8  addu  $v1, $v1, $a0       # 0x74: table + number x 4
800006cc  jr    $ra
800006d0  sw    $a1, 0x4f40($v1)    # stored, whatever the number
```

A number past `0x7C` indexes whatever follows the table in the kernel image,
and `0x74` writes there. On the reference the words after `0x80015134` are
not consulted by anything the boot needs, so a runtime that installs its own
handlers at `0x7F`/`0x82` and calls them gets exactly what it installed. A
rebuild whose table is exactly 125 entries and whose dispatcher rejects
`0x7D` and above refuses those calls, and one whose `0x74` stores past the
table corrupts whatever it put after it.
