# EE Interrupt Delivery and the SIF Syscalls

`docs/analysis/34` ends with the fact that decides the shape of the next
work: the SDK's RPC client is completed by the EE's **DMAC channel-5
interrupt**, through a handler it installs with slot `0x12`. So the interrupt
path — vector to handler and back — is on M1's critical path, as are the five
SIF slots `0x76`–`0x7A` the client drives the bus with. `docs/analysis/16`
found the two dispatch tables and the enable/disable slots; this reads what
happens between them, executes the handler-installing slots and the SIF slots
on the reference under `eesim`, and reads what the client's handler needs.

```sh
python3 tools/romdir.py assets/SCPH-50000.bin --extract <outdir>   # outside the repo
python3 tools/romdis.py <outdir>/KERNEL --cpu ee --vma 0x80000000 --range 0x80000200 0x80000700
python3 tools/romdis.py <outdir>/KERNEL --cpu ee --vma 0x80000000 --range 0x800018b0 0x80001b80
python3 tools/romdis.py <outdir>/KERNEL --cpu ee --vma 0x80000000 --range 0x80006340 0x80006d40
for s in 0x10 0x11 0x12 0x13 0x76 0x77 0x78 0x79 0x7a; do python3 tools/eeksys.py <outdir>/KERNEL --slot $s; done
```

## 1. Interrupt path, vector to handler and back

**Vector at `0x80000200`** (populated per `spec/04` EE-5): saves `$sp/$ra/$at`
with `sq` into fixed scratch cells `0x800010C0/D0/E0` (same cells the `Sys`
exception entry at `0x8000029c` uses — confirmed by hand-decoding the
R5900-only opcodes: `sq $sp,0x10c0($k0)` etc., raw bytes `04 20 c0 7f` class).
Reads `Cause`, ANDs with `Status` (pending & enabled), shifts IP0-7 into a
byte, and uses a PLZCW-class MMI op (raw `0x7020d004`, funct 0x4) to get a
leading-zero count, converting it to an index `30 - lzc`; that indexes the
8-entry table at `0x80015380` (`spec/04` EE-6d) and jumps to it (`jr`, not
`jal` — no return address kept at this level).

**INTC path — table[2] = `0x80000380`:**
```
lui $1,0xb000; ori $1,$1,0xf000     # INTC_STAT  (0x1000F000)
lwu $26,0($1); lwu $1,0x10($1)      # INTC_MASK  (0x1000F010)
and $1,$1,$26                       # pending & enabled
andi $26,$1,0xc0                    # bits 6/7 special-cased, jumps to 0x140c0 (not decoded)
```
Normal path: `Status &= ~0x1c`; `$sp = 0x80018E80` (**matches EE-7d's kernel
stack exactly**); save EPC to that stack; `jal 0x80001300` (a save routine,
separate from the thread-context prologue at `0x80003680` — smaller, not
fully decoded, called on every interrupt not just reschedules); compute the
cause bit index (same 0x1e-lzc trick); **acknowledge by writing the single
bit back to INTC_STAT** (`sw $3,0(0xB000F000)`, write-1-to-clear, done
*before* the handler runs); **clear the reschedule flag `0x800155B4`**
(`sw $zero,0x55b4($26)`); call `table[cause]` at `0x800153C0 + cause*4` with
**`$a0 = cause`** (only one argument — `$a1` is not set by this dispatch
layer); `jal 0x800013c0` (restore, pair of `0x1300`); restore EPC;
**re-read `0x800155B4`**: if set, `$sp = 0x80018E80` again and
`j 0x8000363C` (near the thread prologue at `0x80003680`, not the exact
address — 0x44 bytes earlier) instead of returning, clearing the flag first;
if clear, `Status |= 0x13`, `eret`.

**How the handler is entered, and why the tail cannot be interrupted.** The
trampoline does not `jalr` the installed handler. It calls `0x80002840`
with `$a0` = the word at `0x800154A4` (a stub the program registers; the
SDK's is four instructions: set `$sp`, `jalr` the handler, then
`syscall` with `$v0 = -5`), `$a1..$a3` = handler, argument, cause, and the
interrupted EPC in `$t0`. That routine stashes `$ra`/`$sp` in kernel cells,
writes the stub's address to EPC, sets `Status |= 0x12` and `eret`s -- so
the handler runs with `EXL` clear and `IE`/`EIE` exactly as the interrupted
code left them; the dispatcher masks nothing itself (the `~0x1c` above
clears `ERL` and `KSU` only). The stub's syscall brings control back to
`0x80002880`, which clears the same `0x1c` and returns to the trampoline
with `EXL` still set from the syscall entry. Nothing clears `EXL` between
that return and the `eret` at `0x80000488`, so the re-read of `0x800155B4`
and the exit are one uninterruptible unit -- by `EXL`, not by any `di`;
the exit path contains none (the words at `0x80000448..50` are the `lq`s
restoring `$sp`/`$ra`/`$at`).

**This answers `docs/analysis/33`'s open question**: the reschedule flag's
reader is exactly here, in the interrupt-return path, read right after the
per-cause handler returns and before `eret`. A handler that makes a thread
ready (e.g. by releasing a semaphore a higher-priority thread is waiting on)
sets `0x800155B4` through the same scheduler primitives threads use, and the
interrupt return detects it and reschedules instead of resuming the
interrupted thread.

**DMAC path — table[3] = `0x800004C0`:** structurally identical, but reads
the combined `0x1000E010` (`D_STAT` low 16 bits | `D_MASK` high 16, forced
`|0x8000`), bit 7 special-cased (jumps to `0x141b0`, not decoded), acks by
writing the channel bit back to `0x1000E010` (write-1-to-clear), clears
`0x800155B4`, calls `table[channel]` at `0x80015400 + channel*4` with
**`$a0 = channel`**, then the same restore/reschedule-check/`eret` tail.

**Timer path — table[7] = `0x80000600`:** acks by writing `Count=0` then
restoring `Compare`; calls a **single** handler at `0x80015440` (no
per-cause table, `$a0` not set); same tail.

**Return-value handling: the raw dispatch discards it.** `jalr $3` at
`0x8000042c`/`0x5b0c` is immediately followed by the restore call with no
test of `$v0`. `table[cause]` is **not** the installed user handler directly
— reading it back after installing one (below) shows a fixed address
(`0x80001630` for INTC cause 2, `0x80001798` for DMAC channel 5) that does
**not** change across install/remove. So `table[cause]` is a per-cause
trampoline, populated once, that must be what walks the handler chain and
(presumably) interprets each handler's return value — **its own code was
not decoded** (unresolved, §5).

**Status EIE and `ei`/`di`:** the SDK's `EIntr`/`DIntr` (`m1.dis`
`0x111230`/`0x1111f0`) operate on **`Status` bit 16** (`lui $3,1; and
$2,$2,$3`), the R5900's EIE "master interrupt enable", separate from
architectural `IE` (bit 0). Both use the true `ei`/`di` COP0 instructions
(raw `0x42000038`=`ei` funct 0x38, `0x42000039`=`di` funct 0x39 — decoded by
hand, `<unknown>` in `romdis.py` output). **`_SifCmdIntHandler`
(`0x1128e0`) calls `EIntr()` as its very first action and `di` (raw
`38 00 00 42` at `0x11299c`) as its last, right before returning 0** — the
SDK's own DMAC-channel-5 handler explicitly re-enables interrupts on entry
and disables them again on exit; the kernel does not do this for it.

**Context saved/restored**: not the full 0x2A0-byte thread frame (that's
only entered via `jal 0x8000363c` on the reschedule path) — the interrupt
path's own `0x80001300`/`0x800013c0` pair is smaller and not fully decoded
(§5); `spec/04` EE-7e's frame shape is confirmed only for the syscall/
reschedule path.

## 2. Slots 0x10-0x13 — Add/RemoveIntcHandler, Add/RemoveDmacHandler

Executed via eesim (`eeint_experiments2.py`, `_3.py`):

```
AddIntcHandler(cause=2, handler, next=-1, arg) -> 1        (small sequential id, NOT a pointer)
AddDmacHandler(channel=5, handler, next=-1, arg) -> 2      (same id counter, shared across INTC/DMAC)
AddIntcHandler(cause=0, ...) -> 3
AddIntcHandler(cause=1, ...) -> -1   AddIntcHandler(cause=15,...) -> -1   (INTC: 0..14 except 1)
AddDmacHandler(channel=-1) -> -1  ...(0)->3 (9)->4 (14)->5 (15)->6 (16)->-1   (DMAC: 0..15, 16 slots)
RemoveIntcHandler(2, bogus_id=9999) -> -1
RemoveIntcHandler(2, id1) -> 1        (returns the id back on success)
RemoveIntcHandler(2, id1) again -> -1 (double-remove rejected)
```
**IDs are opaque small integers**, not addresses — a caller must keep the id
`AddIntcHandler`/`AddDmacHandler` returned to pass to Remove; nothing else
identifies a registration. Both `0x10` and `0x12` tail-call one shared
routine at `0x800018b0` (`$t0=2` for INTC, `$t0=0` for DMAC — `spec/05`
SYS-1b), which allocates a node, fills `node+0x08=handler,
+0x0C=$gp (caller's, so the handler runs with the installer's gp),
+0x10=arg, +0x14=mode`, and links it into a **per-cause sentinel doubly
linked list** at `0x80018E98 + cause*0xC` (`+0=head/next, +4=tail/prev,
+8=count`), confirmed by reading that memory before/after install/remove —
new handlers insert at the **head** when `next=-1` (both list pointers equal
the sole node; a second install moves `head` to the new node while `tail`
stays at the first — youngest first). `0x11`/`0x13` are separate,
non-shared routines (`0x80001a70` INTC, `0x80001b10` DMAC) that unlink by id
and decrement the per-cause count. The DMAC-side per-channel bookkeeping
struct's base address (the `0x80018E98` analogue) was **not** pinned down
(§5) — only that it exists (the disassembly of the DMAC add-path references
stack-relative constants `-0x7160`/`-0x70b0`/`-0x6ff0` off a `0x8002xxxx`
base, distinct from INTC's).

Range checks (`0x800018e0`-`0x80001940`): INTC additionally rejects
`cause==1` specifically (checked before the general `<15` bound) — 0 and
2-14 are accepted, 1 is not. DMAC has no such carve-out, just `<16`.

`table[0x800153C0]`/`table[0x80015400]` (§1) are **untouched** by
Add/RemoveIntcHandler/DmacHandler — they must be populated once at kernel
init to a fixed per-cause trampoline that reads the `0x80018E98`-style list.

## 3. Slots 0x76-0x7A

**`0x78` SifSetDChain — `0x80006340`, `(-) -> $v0`:** zero `D5_CHCR`
(`0x1000C000`) and `D5_QWC` (`0x1000C020`), write `CHCR=0x184`
(MOD=chain,STR=1,TIE=1), return the read-back CHCR. `0x6B` is the same code
minus the `0x184` write (stop, not start) — `spec/05` SYS-7d, confirmed.
D5/`0x1000C000` is the EE's **destination** channel, SIF0, IOP-to-EE
(`docs/analysis/24`).

**`0x77` SifSetDma — `0x80006798`, `(list, count) -> $v0`:** touches
`0xB000F520`/`0x1000F590` first (read F520, OR bit16, write to F590 — a
lock/generation bookkeeping pair, not decoded further) and stops `D6_CHCR`
(`0x1000C400`, SIF1, EE-to-IOP — `spec/05` SYS-7d's second half). Then walks
the `count`-entry transfer list (stride 0x10: `src,dest,size,attr` per the
task's struct), for each entry testing `attr & 0x20` and `size < 0x71`
(113), accumulating a small flags/count word `$18`. If that word exceeds a
limit read back from `0xB000F520`, it **aborts and returns 0** without
touching the hardware further. Otherwise it calls a tag-building helper at
`0x80006410` once per entry (`src,dest,size,attr` as `$a0-$a3`, plus an
internal `$t0`-like constant `0x3000` for every entry except the **last**,
which gets `0`) — this builds the source-chain tag list `docs/analysis/24`
already found (BOOT-11a: id 0 `refe` / id 1 `cnt`), the per-entry constant
plausibly selecting the tag id/flags, **not fully decoded** (§5). Finally
restarts `D6_CHCR = 0x184` and **returns the packed validation word
computed during the scan** (bits from the last entry's attr, shifted count,
and the flags accumulator) **— not a monotonic transfer id**, refining the
task's assumption; `0x76`'s `id` argument is this packed word, not a serial
number.

**`0x76` SifDmaStat — `0x80006950`, `(id) -> $v0`:** reads `D6_CHCR`
(`0x1000C400`); **if `STR` (bit 8) is clear, returns -1** (confirmed:
`eesim --syscall 0x76 0` → `-1` on a channel that was never started).
Otherwise decodes `id` into begin/end byte fields, reads `D6_TADR`
(`0x1000C430`) relative to a kernel tag-list base at `0x80021380` (the same
base `docs/analysis/24`'s BOOT-11a walk used), and compares the decoded
range against the channel's current tag position to report whether the
transfer `id` denotes has been reached — exact 0/1/other encoding **not
pinned down** (§5); only the "-1 while idle" edge is executed-confirmed.

**`0x79` SifSetReg — `0x80006c00`, `(reg, val) -> $v0`:** `reg==1/3/4` are
hardware hits: `1`→`0xB000F200` (**MSCOM**), `3`→`0xB000F220` (**MSFLG**,
also calls an internal helper `0x80005e18`), `4`→`0xB000F230` (**SMFLG**,
helper `0x80005ef8`); each **returns the value it just wrote**, confirmed:
`SetReg(1,0x1001)->0x1001`, `SetReg(3,..)->val`, `SetReg(4,..)->val`. `reg==2`
(SMCOM) and `reg==0` are **not settable** — return `0` (SMCOM is the IOP's
to write, per `docs/analysis/24`). `reg` with the sign bit set — a caller
must pass it **sign-extended to 64 bits** (`lui`/`ori` naturally does this;
a naive 32-bit `0x80000000|n` zero-extended into a 64-bit register is
*not* recognised, confirmed by two eesim runs that differ only in
sign-extension) — is a **software register**: index `= reg & 0x7fffffff`,
must be `<32`, stored at **`0x800212C0 + index*4`** (32-entry `SREG` array).
Confirmed: `SetReg(sign_ext(0x80000000|5), 0x2005)` → memory at
`0x800212C0+0x14` reads back `0x2005`. **But the syscall's own return value
for the software-register path is `$a2` passed through unset** — observed
`0xa0000000` (leftover register content) in testing, never the value
written; a rebuild's SDK-facing behaviour must not assume `0x79` on a
software register returns anything meaningful.

**`0x7A` SifGetReg — `0x80006cb8`, `(reg) -> $v0`:** mirrors `0x79`:
`1`→read MSCOM, `2`→read SMCOM, `3`/`4`→read MSFLG/SMFLG **through the same
internal helpers** (`0x80006ad8`→`0x5e18`, `0x80006b20`→`0x5ef8` — no direct
hardware `lw` for 3/4 on the *read* side, only via the helper, unlike the
*write* side which does both `sw` and call the helper). Software regs
(`reg` sign-extended, `<32` after masking) read `SREG[index]` directly —
confirmed round-trip: after `SetReg(sreg(5), 0x2005)`, `GetReg(sreg(5))` →
`0x2005`. Out-of-range software index (`>=32`) returns `0`.

**Kernel-internal SIF sender, not one of the 5 syscalls**: a block at
`0x800063a8-0x80006400` (right after `0x78`'s two tiny routines) builds a
header and arms **D6** (`TADR` at `0x1000C430`, `QWC` zeroed at `0x1000C420`)
directly — this is the kernel's own outbound SIF-cmd path used during boot
(e.g. by the `rom0:` file loader), separate from `0x77`.

## 4. What `_SifCmdIntHandler` needs (from `m1.dis`)

`sceSifInitCmd` (`0x111c10`) installs it: `AddDmacHandler($a0=5,
$a1=&_SifCmdIntHandler, $a2=0, ...)` (`0x111d58`, syscall `0x12`), then
`EnableDmac(5)` (syscall `0x16`). Before that it polls `D5_CHCR & 0x100`
(busy) and calls `sceSifSetDChain` (syscall `0x78`) if the channel isn't
already armed as a destination chain — **the SDK, not the kernel, arms
channel 5** before registering the handler.

`_SifCmdIntHandler` (`0x1128e0`), run as the DMAC-channel-5 handler:
1. `EIntr()` — re-enables interrupts immediately (see §1).
2. Reads a one-byte flag at a static receive-queue struct (`0x126400`-area
   in the SDK's own BSS, not the kernel's `SREG`/list tables); if zero,
   nothing arrived, skip to the tail.
3. Copies the newly-landed packet(s) out of a ring buffer into a stack
   buffer (16 bytes at a time, `ld`/`sd` pairs).
4. **Calls `isceSifSetDChain()` itself** — syscall number **`-0x78`**
   (negated, `spec/04` EE-7b's "both forms reach the same slot": the
   dispatcher `negu`s it back to `0x78` and runs the identical handler —
   there is *no* kernel-side behavioural difference between `sceSifSetDChain`
   and `isceSifSetDChain`; the `i`-prefix is an SDK-only "safe to call with
   interrupts already touched" convention, not a distinct kernel path).
   This **re-arms channel 5 for the next packet**, from inside the handler.
5. Decodes a command id from the copied packet and dispatches: negative ids
   go through a "system" table at `queue+0x0C` (size at `+0x10`), positive
   ids through a "user" table at `queue+0x14` (size at `+0x18`) — RPC-style
   registered callbacks, called with `$a0=`the stack buffer.
6. `sync`, `di` (see §1), restore `$ra`, **`$v0 = 0` always**, return.

So for the SDK client to work, the kernel side must: (a) let `0x12` install
a DMAC-channel-5 handler that is later invoked with `$a0=5`; (b) ack D5's
`D_STAT` bit for the SDK *before* calling the handler (already true, §1),
since the handler's own `isceSifSetDChain` call re-arms `CHCR`/`QWC` but
does not touch `D_STAT`; (c) not require any particular return value from
the handler (the SDK always returns 0, and §1 found the raw dispatch
ignores it — only the un-decoded per-cause trampoline could care, §5).

## 5. Unresolved (marked)

- **The per-cause trampoline** at `table[cause]`/`table[channel]`
  (`0x800153C0+cause*4`, `0x80015400+channel*4`) — populated once at kernel
  init to a fixed address that does not change across install/remove; its
  own code (presumably the chain walker that reads the `0x80018E98`-style
  list) was not located/decoded.
- **`0x80001300`/`0x800013c0`**, the interrupt path's own small
  save/restore pair (distinct from the thread prologue/epilogue at
  `0x80003680`/`0x80003800`) — not decoded.
- **`0x8000363C`** — the reschedule-flag jump target near, but not equal
  to, the thread prologue `0x80003680`; relationship not established.
- **DMAC's per-channel list-head/count base** (INTC's is `0x80018E98`,
  stride `0xC`) — exists (referenced by stack-relative offsets in the add
  routine) but its address was not pinned down empirically.
- **`0x77`'s tag-building helper `0x80006410`** and the meaning of its
  `0x3000`-vs-`0` fifth value per entry — not decoded past "non-last vs
  last entry get different flags/ids".
- **`0x76`'s exact id-range-vs-TADR comparison outcome encoding** (0/1/
  other) — only the "-1 while `D6_CHCR.STR` clear" edge was executed.
- **`0x79`/`0x7A`'s MSFLG/SMFLG helpers `0x80005e18`/`0x80005ef8`** — called
  on both set and get for regs 3/4; internal effect (e.g. clearing bits,
  waking something) not decoded.
- **INTC bits 6/7 and DMAC bit 7 special-case jumps** (`0x140c0`/`0x141b0`,
  seen in §1's dispatch) — not followed.
- **The 64-bit-sign-extension requirement on `$a0` for `0x79`/`0x7A`**
  (`bgezl $a0` tests the full 64-bit register, so a caller must present a
  properly sign-extended negative value, which real R5900 code does
  automatically via `lui`/`addiu` but a naive test harness must do by hand)
  is confirmed behaviourally but the `bgezl` semantics themselves (why a
  not-taken likely-branch's effect matched what a *taken* branch would have
  done in the failing test) were not fully reconciled against the R5900
  spec — flagged rather than asserted as understood.
