# Specification: The IOP Kernel

Derived from `docs/analysis/37-iop-interrupts.md` (EXCEPMAN and INTRMAN),
`38-iop-threads.md` (THREADMAN), `39-iop-file-layer.md` (IOMAN, ROMDRV,
MODLOAD, LOADFILE) and `40-sio2man.md` (SIO2MAN's start-up).

This covers the IOP-side kernel services above `SYSMEM`/`LOADCORE`
(`docs/spec/02-module-abi.md`): exception and interrupt delivery, the thread
scheduler, the file-driver framework and the ROM device it serves, the module
loader that answers `SifLoadModule`, and the one device driver (`SIO2MAN`)
whose start-up sequence is analysed in enough depth to specify. `spec/02`
already fixes the module container, the ordinal-binding ABI (IRX-6/-9), the
entry/residency convention (IRX-12) and the `intrman` 17/18 critical-section
aliasing; that is not restated here. `spec/03-boot-chain.md` BOOT-12e already
fixes the shape of `LOADFILE`'s SIF-RPC request and reply; IOP-5 covers only
what happens on the IOP side of that exchange.

Addresses are given in the CPU's KSEG1 (uncached) form, matching how the
reference code itself addresses hardware registers — physical is the same
value with `0xA0000000` cleared.

## IOP-1: Exceptions (EXCEPMAN)

Derived from `docs/analysis/37` §1.

**IOP-1a — the handler structure and its ordinals.** A caller wishing to
handle an exception owns a three-word structure, `{ next@0, info@4,
funccode@8 }`: `next` must be zero (unlinked) before registering, `info` holds
the handler's code pointer with a 2-bit priority packed into its low bits, and
`funccode` is the entry point EXCEPMAN's dispatch jumps to. Ordinals:

| Ord | Signature [header] | Notes |
| --- | --- | --- |
| 3 | `GetExHandlersTable() -> table_addr*` | returns the address of a single cell holding the cause table's own base pointer, so relocating the table needs one patched cell, not a search |
| 4 | `RegisterExceptionHandler(exception, handler) -> result` | wraps ordinal 5 with `priority = 2` |
| 5 | `RegisterPriorityExceptionHandler(exception, priority, handler) -> result` | priority is masked to its low 2 bits before storage |
| 6 | `RegisterDefaultExceptionHandler(handler) -> result` | links onto one cause-independent chain every cause's last node falls through to |
| 7 | `ReleaseExceptionHandler(exception, handler) -> result` | |
| 8 | `ReleaseDefaultExceptionHandler(handler) -> result` | |

**IOP-1b — registration errors and priority order.** Ordinal 5 rejects an
`exception` outside `0..15` with `-0x32`, and a `handler` whose own `next`
field is already non-zero (already linked elsewhere) with `-0x34`. Priority
runs `0..3`; a chain executes **highest priority number first**, falling
through in descending priority to the cause-independent default chain — the
observed data point is a priority-3 handler INTRMAN registers on cause 0
outranking its own priority-2 main dispatcher (IOP-2d). Every insert and
every remove rewrites each affected node's own `next` field to the address of
the *next* handler's `funccode` (its own base address `+8`) — turning
registration into a native, `jr`-through code chain rather than a table a
dispatcher walks by lookup. A rebuild's `EXCEPMAN` may store its bookkeeping
however it likes, but must reproduce this effect: chained, priority-ordered
dispatch that falls through to a shared default chain, driven by the
handler's own leading word.

**IOP-1c — the installed vector and the cause table.** The general-exception
vector occupies the R3000's fixed vector address (destination offset `0x80`
from the base the reset path installs at, per `docs/spec/03-boot-chain.md`
BOOT-4 step 3): it saves `$at`, `EPC`, `Status`, `Cause`, computes `Cause &
0x3C` (`ExcCode` as a byte index, `0`, `4`, ..., `0x3C`) and jumps through a
16-entry, cause-indexed table of chain heads at that index. A second block, at
destination offset `0x40`, saves `$26`, `EPC`, `Status`, `Cause` and COP0
register 7, clears COP0 register 7, and jumps through the **same** table at
the fixed index `15` (`IOP_EXCEPTION_HDB` [header], "Hardware DeBug") —
consistent with a dedicated breakpoint path, though exactly what hardware
condition reaches offset `0x40` rather than `0x80` is **open**: not
established by the analysis.

**IOP-1d — the pool.** Registered-handler bookkeeping nodes come from a
private, module-internal slab (32 nodes of 8 bytes, from one initial
allocation), entirely separate from `SYSMEM`'s general allocator. A rebuild
needs no fixed pool size — the reference's is an implementation detail, not an
ABI — only that registration and release do not depend on `SYSMEM` on every
call.

## IOP-2: Interrupts (INTRMAN)

Derived from `docs/analysis/37` §2–§3, including §3.6 on the second DMA
controller bank.

**IOP-2a — ordinal table.** Cross-checked against every import site the
analysis read (`SIFCMD`, `SIFMAN`, `DMACMAN`, `THREADMAN`, `SIO2MAN` among
others) and ps2sdk's `intrman.h` [header]:

| Ord | Signature [header] | Notes |
| --- | --- | --- |
| 3 | `GetIntrmanInternalData() -> intrman_internals_t*` | |
| 4 | `RegisterIntrHandler(irq, mode, handler, arg) -> result` | IOP-2b |
| 5 | `ReleaseIntrHandler(irq) -> result` | same range checks and storage formula as ordinal 4 |
| 6 | `EnableIntr(irq) -> result` | IOP-2c |
| 7 | `DisableIntr(irq, res) -> result` | IOP-2c |
| 8 | `CpuDisableIntr(state) -> result` | traps via `syscall 4`; IOP-2k |
| 9 | `CpuEnableIntr(state) -> result` | traps via `syscall 8`; IOP-2k |
| 14 | `CpuInvokeInKmode(...) -> ...` | traps via `syscall 0xc`; body not decoded |
| 15 | `DisableDispatchIntr(irq) -> result` | sets a bit in the software dispatch-mask overlay; IOP-2f |
| 16 | `EnableDispatchIntr(irq) -> result` | clears the same bit |
| 17 | `CpuSuspendIntr(state*) -> result` | traps via `syscall 0x10`; IOP-2k |
| 18 | `CpuResumeIntr(state) -> result` | traps via `syscall 0x14`; IOP-2k |
| 19, 20 | aliases of 17, 18 | identical addresses on the reference |
| 23 | `QueryIntrContext() -> bool` | `= QueryIntrStack($sp)`, IOP-2e |
| 24 | `QueryIntrStack(sp) -> bool` | |
| 25 | `iCatchMultiIntr(...)` | not decoded |
| 28 | `SetNewCtxCb(cb) -> void` | IOP-2j |
| 29 | `ResetNewCtxCb() -> void` | restores a built-in default |
| 30 | `SetShouldPreemptCb(cb) -> void` | IOP-2j |
| 31 | `ResetShouldPreemptCb() -> void` | restores a built-in default |

Ordinals 1, 2 and 26 are reserved stubs; ordinals 10, 11, 21 and 22 are the
bare `syscall 4`/`8`/`0x10`/`0x14` trampolines ordinals 8/9/17/18 wrap;
ordinals 12 and 13 are not decoded; ordinal 27 sets an internal DMA-bank-2
handler mask field, described in IOP-2i.

**IOP-2b — the registration record.** `RegisterIntrHandler(irq, mode,
handler, arg)` accepts `irq` in `0..0x2D` (46 values) and the two software
lines `{0x3E, 0x3F}`; any other value returns `-0x65`. A duplicate
registration on an already-occupied slot returns `-0x68` (the main range) or
`-0x69` (the two software-interrupt slots). Calling from interrupt context
(`QueryIntrContext() != 0`) is a silent no-op. Storage is two words per `irq`
(`handler|mode` at `+0`, `arg` at `+4`); **only `irq < 0x20` packs `mode & 3`
into the stored handler word** — every `irq` from `0x20` through `0x2D`
stores the raw handler pointer with no mode bits, so any handler registered in
that range is dispatched as mode 0 regardless of what `mode` its caller
passed. `CpuSuspendIntr`/`CpuResumeIntr` bracket the whole table mutation.

**IOP-2c — `EnableIntr`/`DisableIntr`: which register, and the umbrella
bit.** For `irq < 0x20`: a plain `I_MASK |= 1<<irq` (enable) or `I_MASK &=
~(1<<irq)` (disable). For `0x20 <= irq < 0x27` (the seven DMA-bank-1
channels): `EnableIntr` sets the channel's own enable bit in `DICR` (bit
position `16 + (irq - 0x20)`, i.e. bits 16–22), sets `DICR` bit 31 (the
controller's own master enable) and sets `I_MASK` bit 3 (`IOP_IRQ_DMA`, the
umbrella cause) — all three are required before a channel's own `DICR` flag
can ever reach `I_STAT`. `DisableIntr` clears the channel's `DICR` enable bit
and reports through `*res` whether that channel's own flag bit was also set.
For `irq >= 0x27`, `EnableIntr` does nothing and returns success (a silent
no-op); `DisableIntr` returns `-0x65` instead — an intentional asymmetry, not
a typo.

Register addresses (bank 1, all confirmed by direct disassembly):

| Register | Address | Register | Address |
| --- | --- | --- | --- |
| `I_STAT` | `0xBF801070` | `I_MASK` | `0xBF801074` |
| `DPCR` | `0xBF8010F0` | `DICR` | `0xBF8010F4` |

**IOP-2d — what INTRMAN registers with EXCEPMAN and with itself, at boot.**
Before any of its own exported functions can be called, INTRMAN's entry
makes three `EXCEPMAN` calls and one self-call:

- `RegisterExceptionHandler(exception = 0 /* IOP_EXCEPTION_INT [header] */,
  handler = <main dispatcher>)` — implicit priority 2; this is IOP-2e/f/g's
  entry point.
- `RegisterPriorityExceptionHandler(exception = 0, priority = 3, handler =
  <a second handler>)` — chained onto the same cause at higher priority.
  Its own body is four straight-line instructions restoring `$at`/`Status`
  and jumping to the saved `EPC`, with no branch and no DMA-table
  involvement — an unconditional exception-return sequence. **Open:** when
  control reaches this handler rather than the main dispatcher, and how the
  main dispatcher runs at all afterward given this handler's body never
  chains onward through its own `next` field, is not established. A rebuild
  may omit this second registration until its purpose is understood; nothing
  in the analysis pins it as required for correct interrupt delivery.
- `RegisterExceptionHandler(exception = 8 /* IOP_EXCEPTION_SYS [header] */,
  handler = <the SYSCALL handler>)` — backs IOP-2k.
- `RegisterIntrHandler(irq = 3 /* IOP_IRQ_DMA [header] */, mode = 1, handler
  = <the DMA-bank-1 sub-dispatcher>, arg = 0)` — installs IOP-2h.

**IOP-2e — delivery: entry, save depth, nesting.** The vector (IOP-1c) has
already saved `$at`, `EPC`, `Status`, `Cause` before reaching the main
dispatcher, which saves `$at`, `$2`, `$3`, `$4`–`$7`, `hi`/`lo`, `Status` and
`EPC` unconditionally (the "mode-0 baseline") and calls `QueryIntrContext()`
to decide whether this is a **nested** interrupt (`$sp` already inside the
fixed interrupt stack) or a **fresh** one. A nested interrupt grows the
current stack frame by `0x18` bytes without switching stacks; a fresh one
switches `$sp` to a dedicated interrupt-stack top. The interrupt stack is a
fixed `0x800`-byte range ending at that top; `QueryIntrContext`/
`QueryIntrStack` test only whether a pointer falls in that range — there is
no separate "in interrupt" flag. Register preservation beyond the mode-0
baseline is deferred until a handler is found and its stored `mode` bits
read: mode ≥ 1 additionally saves `$8`–`$15`, `$24`, `$25`, `gp`, `fp`; mode ≥
2 additionally saves `$16`–`$23`. Before the hardware scan, `Cause` bits 8/9
(`IP0`/`IP1`, the two software-interrupt lines) are checked and routed to a
separate path if set.

**IOP-2f — finding the source and acknowledging it.** The hardware path
scans `I_STAT & I_MASK & <software overlay mask>` for its **lowest** set bit
— ascending IRQ-number priority, the opposite convention from the EE's own
INTC/DMAC dispatch, which picks by leading-zero-count from the top. The
overlay mask is never touched by `EnableIntr`/`DisableIntr`; it is a purely
software gate maintained by `DisableDispatchIntr`/`EnableDispatchIntr`
(ordinals 15/16): `irq < 0x20` sets/clears a bit in one word; `0x20 <= irq <
0x28` sets/clears a bit in a bank-1 shadow mask; `0x28 <= irq < 0x2E` sets/
clears a bit in a **second**, bank-2 shadow mask (IOP-2i). Having the index,
the dispatcher **acknowledges before looking up or calling a handler**:
write-0-to-the-target-bit-and-1-elsewhere to `I_STAT` — the opposite polarity
from the EE side's write-1-to-clear — together with a temporary mask-off of
just that bit in `I_MASK`. If no handler is registered for the acknowledged
bit, nothing further happens: no error, no chain call, silently dropped. The
software-interrupt path (`Cause` `IP0`/`IP1`) acknowledges instead by
clearing the bit directly in `Cause` (a MIPS hardware requirement, not a
choice) and reads its handler from the separate two-entry table for
`{0x3E, 0x3F}`.

**IOP-2g — calling the handler and its return value.** The handler is
called as `handler(arg)`, with as much of the register file preserved as its
stored mode requires (IOP-2e). **The handler's return value gates whether
`I_MASK` is restored**: `$v0 == 0` leaves the bit masked off (the interrupt
stays disabled until something else re-enables it); nonzero restores
`I_MASK`, re-enabling the same IRQ. This is load-bearing for handlers
(`SIFCMD`, `DMACMAN`-style drivers) that expect to remain enabled across
calls.

**IOP-2h — DMA bank-1 sub-dispatch (`irq` 3, `IOP_IRQ_DMA`).** The handler
INTRMAN registers on itself at boot (IOP-2d) services the combined DMA cause:
it reads `DICR & <bank-1 shadow mask>`, extracts bit 15 (a force/error-class
condition) and bits 24–30 (seven channel flags), and for each of the up to
seven set bits acknowledges that channel's flag in `DICR` and calls
`table[0x20 + N](arg)` — the **same** per-`irq` table IOP-2b describes,
indices `0x20`–`0x26`. This outer handler always returns 1 (keeping
`IOP_IRQ_DMA` itself enabled regardless of any individual channel handler's
own return). Whether the resident kernel extends this same loop to scan the
second DMA bank is IOP-2i.

**IOP-2i — the second DMA controller bank (`DICR2`).** Register addresses,
established from `DMACMAN`'s own accessor pointer table, not inferred, and
variant-independent — `DMACMAN` is one module, not one of the paired
variants:

| Register | Address | Register | Address |
| --- | --- | --- | --- |
| `DPCR2` | `0xBF801570` | `DICR2` | `0xBF801574` |
| unnamed | `0xBF801578` | unnamed | `0xBF80157C` |
| `DPCR3` | `0xBF8015F0` | | |

`DMACMAN` itself writes `DPCR`/`DPCR2`/`DPCR3` with a fixed per-channel
priority pattern (`0x07777777`/`0x07777777`/`0x777`) and the unnamed
`0x1578` register with `1` at boot, but never touches `DICR2` and never calls
into `INTRMAN` — it is purely a register-poking accessor library with no
opinion on how DMA completion reaches a handler.

**The resident variant is `INTRMANI`** (`docs/analysis/37` §0: the boot
releases `INTRMANP` on this generation of IOP, the one with the second
bank), and it reaches the bank. `EnableIntr` for `0x28 <= irq < 0x2E` sets
the channel's enable bit in `DICR2` (bit `16 + (irq - 0x28)`), sets **`DICR`**
bit 23 — the one master for both banks — and `I_MASK` bit 3; `DisableIntr`
clears the `DICR2` enable bit and reports the flag bit through `*res`, as
for bank 1. The `irq`-3 handler walks **both** banks: `DICR`'s seven flags
for indices `0x20`–`0x26`, then `DICR2`'s for `0x28`–`0x2D`, each flag
acknowledged (a 1 written to it clears it) before its handler runs — which
is the path `SIFMAN`'s `0x2A` and `SIFCMD`'s `0x2B` are delivered by (§3.6).
`INTRMANI` also keeps a second gate, **`I_CTRL`** at `0xBF801078`:
`CpuEnableIntr` writes it `1`, the disabling calls never write it `0`, the
dispatcher reads it on entry (which clears it) and writes the read value
back on exit, and `I_STAT` reaches the CPU only while it is set.
`INTRMANP`'s own sub-dispatch stops at bank 1 and its `EnableIntr` ignores
`irq >= 0x27`; that is the non-resident variant's dead path, not a
requirement.

**IOP-2j — the reschedule hooks, at the tail of a non-nested interrupt.**
After handler dispatch and `I_MASK` handling, INTRMAN checks whether the
**interrupted** context (not the one just serviced) was itself inside the
interrupt stack — i.e., whether this whole delivery was nested. A **nested**
interrupt skips straight to restore, never calling either hook. A
**non-nested** one calls the `ShouldPreemptCb` hook (`SetShouldPreemptCb`,
ordinal 30); if it answers no, restore proceeds normally; if yes, the
interrupted context's saved-register set is topped up to the full set and the
`NewCtxCb` hook (`SetNewCtxCb`, ordinal 28) is called, and execution resumes
from whatever frame it returns. `THREADMAN` is the only module the analysis
found importing ordinals 28/30; a minimal kernel needs at least a
`ShouldPreemptCb` that can safely answer "no" — interrupts must be able to
return to the interrupted thread with zero scheduler support present
(cross-ref IOP-3h).

**IOP-2k — `CpuSuspendIntr`/`CpuResumeIntr`/`CpuEnableIntr`/`CpuDisableIntr`:
the `Status` `IEp`/`IEo` stack.** The public ordinals are thin wrappers that
trap via `syscall 4` (`CpuDisableIntr`), `syscall 8` (`CpuEnableIntr`),
`syscall 0x10` (`CpuSuspendIntr`) and `syscall 0x14` (`CpuResumeIntr`); the
actual `Status` manipulation lives in the SYSCALL-exception handler INTRMAN
registers for cause 8 (IOP-2d), dispatched by syscall number. Every case
first computes an "old-state" result as `Status & 0x414` (bits 2 `IEp`, 4
`IEo`, 10 `Im2` — the mask bit gating hardware interrupt line 2, the line
`I_STAT`'s own umbrella feeds), then edits **`Status`'s previous level**
(`IEp`/`KUp`), never the current `IEc` bit directly, because this code is
itself running inside an exception and the pending promotion on its own
return shifts `IEp` down into `IEc`. Confirmed case bodies: `syscall 4`
clears `Status &= ~0x404` (bits 2 and 10); `syscall 8` sets `Status |= 0x404`
unconditionally; `syscall 0x14` clears `Status &= ~0x414` then ORs in the
caller's own saved state. `CpuSuspendIntr`'s own wrapper additionally
validates the returned old-state against the pattern `0x404` and returns
`-0x66` on mismatch, otherwise writing the raw value through `*state` and
returning success. **Open:** `syscall 0x10`'s own bit-manipulation body, and
whether `CpuDisableIntr`/`CpuEnableIntr` report the previous state through an
out-parameter the same way `CpuSuspendIntr`/`CpuResumeIntr` do, were not
independently confirmed — described in the analysis only as "thinner
wrappers."

## IOP-3: Threads (THREADMAN)

Derived from `docs/analysis/38`.

**IOP-3a — ordinal tables.** `thbase`, `thevent` and `thsemap`. Seven
`thbase` exports are present in the table (so binding against them succeeds)
but are two-instruction stubs that unconditionally return `KE_ERROR`
(`-1`) [header]: `ExitDeleteThread`, `DisableDispatchThread`,
`EnableDispatchThread`, `SuspendThread`, `iSuspendThread`, `ResumeThread`,
`iResumeThread`. `thmsgbx`, `thfpool`, `thvpool` and `thrdman` (a fourth,
fully-unimplemented library whose four ordinals all share one bare `jr $ra`)
also exist; their bodies are out of scope here (analysis §1.4) and a rebuild
targeting `SIF*`/`LOADFILE`/`SIO2MAN` does not need them (IOP-3h imports
list).

**`thbase`:**

| Ord | Signature [header] | Ord | Signature [header] |
| --- | --- | --- | --- |
| 3 | `GetThreadmanData() -> void*` | 4 | `CreateThread(iop_thread_t*) -> id \| KE_*` |
| 5 | `DeleteThread(id) -> 0 \| KE_*` | 6 | `StartThread(id, arg) -> 0 \| KE_*` |
| 7 | `StartThreadArgs(id, argc, argp) -> 0 \| KE_*` | 8 | `ExitThread() ->` (never returns) |
| 9 | `ExitDeleteThread(...) -> KE_ERROR` **(stub)** | 10 | `TerminateThread(id) -> 0 \| KE_*` |
| 11 | `iTerminateThread(id) -> 0 \| KE_*` | 12 | `DisableDispatchThread() -> KE_ERROR` **(stub)** |
| 13 | `EnableDispatchThread() -> KE_ERROR` **(stub)** | 14 | `ChangeThreadPriority(id, priority) -> old \| KE_*` |
| 15 | `iChangeThreadPriority(id, priority) -> old \| KE_*` | 16/17 | `(i)RotateThreadReadyQueue(priority) -> 0 \| KE_*` |
| 18 | `ReleaseWaitThread(id) -> 0 \| KE_*` | 19 | `iReleaseWaitThread(id) -> 0 \| KE_*` |
| 20 | `GetThreadId() -> id \| KE_*` | 21 | `CheckThreadStack() -> bytes` |
| 22/23 | `(i)ReferThreadStatus(id, info*) -> 0 \| KE_*` | 24 | `SleepThread() -> 0` |
| 25/26 | `(i)WakeupThread(id) -> 0 \| KE_*` | 27/28 | `(i)CancelWakeupThread(id) -> prevCount \| KE_*` |
| 29–32 | `(i)SuspendThread`/`(i)ResumeThread(...) -> KE_ERROR` **(stubs)** | 33 | `DelayThread(usec) -> 0 \| KE_*` (IOP-3j) |
| 34 | `GetSystemTime(out*) -> 0` (IOP-3j) | 35/36 | `(i)SetAlarm(clock, cb, arg) -> 0 \| KE_*` (IOP-3k) |
| 37/38 | `(i)CancelAlarm(cb, arg) -> 0 \| KE_*` | 39 | `USec2SysClock(usec, out*) -> void` |
| 40 | `SysClock2USec(clock*, sec*, usec*) -> void` | 41 | `GetSystemStatusFlag() -> u32` |

**`thevent`:** `4 CreateEventFlag(iop_event_t*) -> id | KE_*`, `5
DeleteEventFlag(id) -> 0 | KE_*`, `6/7 (i)SetEventFlag(id, bits) -> 0 | KE_*`,
`8/9 (i)ClearEventFlag(id, bits) -> 0 | KE_*`, `10 WaitEventFlag(id, bits,
mode, result*) -> 0 | KE_*`, `11 PollEventFlag(id, bits, mode, result*) -> 0
| KE_EVF_COND`, `13/14 (i)ReferEventFlagStatus(id, info*) -> 0 | KE_*`.

**`thsemap`:** `4 CreateSema(iop_sema_t*) -> id | KE_*`, `5 DeleteSema(id) ->
0 | KE_*`, `6/7 (i)SignalSema(id) -> 0 | KE_*`, `8 WaitSema(id) -> 0 | KE_*`,
`9 PollSema(id) -> 0 | KE_SEMA_ZERO`, `11/12 (i)ReferSemaStatus(id, info*) ->
0 | KE_*`.

**IOP-3b — the thread parameter block and `CreateThread`'s bounds.**
`iop_thread_t { attr@0x0; option@0x4; thread@0x8; stacksize@0xC;
priority@0x10 }` [header]. Validated, in order, each failure returning
immediately: not interrupt context (else `KE_ILLEGAL_CONTEXT`, `-100`);
`attr` has no bit outside `0xE3000008` (else `KE_ILLEGAL_ATTR`, `-401`) —
this range **accepts** `TH_ASM` (`0x01000000`), `TH_C` (`0x02000000`),
`TH_UMODE` (`8`) [header] plus three unnamed top bits, and **rejects**
`TH_NO_FILLSTACK` (`0x00100000`)/`TH_CLEAR_STACK` (`0x00200000`) [header]
outright — any caller setting either gets `KE_ILLEGAL_ATTR`; `priority` in
`[1, 126]` (else `KE_ILLEGAL_PRIORITY`, `-403`); `thread` 4-byte aligned
(else `KE_ILLEGAL_ENTRY`, `-402`); `stacksize >= 0x130` (304 bytes, else
`KE_ILLEGAL_STACK_SIZE`, `-404`), rounded up to the next `0x100` and written
back into the caller's own `stacksize` field — a real side effect on the
input struct.

**IOP-3c — the record and the id.** A thread record is `0x50` bytes, pool
tag `0x7F01`. An id is **not** an index: `id = (record_ptr << 5) |
((creation_counter & 0x3F) << 1) | 1`. Every id-consuming call reconstructs
`ptr = (id >> 7) << 2`, checks the pool tag at `ptr+8`, and checks the
generation (`(id >> 1) & 0x3F`) against the record's own counter at `ptr+0xA`
— a mismatch returns `KE_UNKNOWN_THID` (`-407`). A rebuild may choose its own
id scheme (a small-index scheme is simpler and equally correct), but **must**
reject a stale or foreign id the same way, since callers compiled against
ps2sdk only ever treat ids as opaque integers.

**IOP-3d — states, priorities, ready queues.** States (a byte at `+0xC`):
`0x10` DORMANT, `2` READY, `1` RUN, `4` WAIT, `8` SUSPEND (unreachable on the
reference, since `Suspend`/`ResumeThread` are stubs), `0xC` WAIT|SUSPEND
(also unreachable) [header]. Priorities `0..127`, numerically lower is
better (`1` = highest creatable, `126` = lowest creatable, `127` reserved for
the idle thread, IOP-3h). One FIFO ready queue per priority plus a parallel
occupancy bitmap; the pick is the lowest set bit.

**IOP-3e — lifecycle, with numeric codes.** `StartThread`/`StartThreadArgs`:
require `state == DORMANT` (else `KE_NOT_DORMANT`, `-414`); prime a
context frame and set the primed return address to `ExitThread`'s own entry,
so a thread function that falls off the end lands there rather than in
undefined behaviour. `ExitThread`: never returns to its caller; sets
DORMANT and reschedules. `TerminateThread`/`iTerminateThread`: id `0`
rejected (`KE_ILLEGAL_THID`, `-406`); a target that is the caller's **own**
current thread is also rejected the same way (distinct from the EE
analogue, which allows self-termination); DORMANT target `KE_DORMANT`
(`-413`); cancels a pending `TSW_DELAY` alarm if the target was so waiting;
unconditional unlink from whatever list held it; returns `0` on success —
**not** the id. `ReleaseWaitThread`/`iReleaseWaitThread`: DORMANT
`KE_DORMANT`; non-WAIT `KE_NOT_WAIT` (`-416`); forces the blocked call's own
return slot to `KE_RELEASE_WAIT` (`-418`); decrements the wait object's
waiter count for the five "real" wait types (semaphore/event-flag/mailbox/
variable-pool/fixed-pool, values `3`–`7`) — `TSW_SLEEP`(`1`)/`TSW_DELAY`(`2`)
have no such object and are excluded. `GetThreadId`: requires ordinary
thread context (`QueryIntrContext() == 0`, else `KE_ILLEGAL_CONTEXT`);
recomputes the id fresh every call, never cached. `ReferThreadStatus`/
`iReferThreadStatus`: id `0` means self; copies status, current/initial
priority, entry, stack base/size, `$gp`, attr, option and (when WAIT)
wait-type and wait-id, always wakeup count. `SleepThread`: if the wakeup
count is positive, decrements it and returns `0` **without blocking** (not
the caller's own id, unlike the EE analogue); otherwise blocks
(`waitType = TSW_SLEEP`, `1`). `WakeupThread`/`iWakeupThread`: id `0`
rejected; DORMANT `KE_DORMANT`; a sleeping target is unlinked and readied; any
other reachable state **increments** the wakeup count and returns `0` — the
counter `SleepThread`'s fast path consumes. `CancelWakeupThread`/
`iCancelWakeupThread`: **does** accept id `0` as self (unlike
`WakeupThread`); returns the previous wakeup count and resets it to `0`, no
state check.

**IOP-3f — event flags.** `iop_event_t { attr, option, bits }` [header],
`0x28` bytes, pool tag `0x7F03`; `attr` accepts only `EA_MULTI` (`2`)
[header], else `KE_ILLEGAL_ATTR`. `SetEventFlag`/`iSetEventFlag`: `bits ==
0` is a no-op; otherwise `currBits |= bits`, then each waiter's condition is
tested — `(currBits & wantBits) != 0` for OR mode (`mode & 1`), `(currBits &
wantBits) == wantBits` for AND — and a satisfied waiter is unlinked and
readied. **`mode & WEF_CLEAR` (`0x10`) resets `currBits` to `0` entirely**,
not just the waited-on bits, and this is identical in `Set*` and in
`WaitEventFlag`/`PollEventFlag`'s own immediate-success path.
`ClearEventFlag`/`iClearEventFlag`: `currBits = currBits & bits` — the
argument is a **keep-mask**, not a clear-mask; never touches the wait list.
`WaitEventFlag(id, bits, mode, &result)`: `mode & ~0x11 != 0` →
`KE_ILLEGAL_MODE` (`-405`); `bits == 0` → `KE_EVF_ILPAT` (`-423`); non-`
EA_MULTI` with an existing waiter → `KE_EVF_MULTI` (`-422`); already
satisfied writes `*result`, applies `WEF_CLEAR` if requested, returns `0` with
no block; otherwise blocks (`waitType = TSW_EVENTFLAG`, `4`). `PollEventFlag`
is identical through the satisfied path but returns `KE_EVF_COND` (`-421`)
instead of blocking. Other confirmed codes: `KE_UNKNOWN_EVFID` (`-409`),
`KE_WAIT_DELETE` (`-425`, forced into every waiter's return slot when the
object is deleted).

**IOP-3g — semaphores.** `iop_sema_t { attr, option, initial, max }`
[header], `0x2C` bytes, pool tag `0x7F02`; `attr` must have `~0x101 == 0`
(`SA_THFIFO`=`0`/`SA_THPRI`=`1`/`SA_IHTHPRI`=`0x100` [header] legal), else
`KE_ILLEGAL_ATTR`. The reference forms the mask as the immediate `-0x102`,
which is `0xfffffefe` — a rebuild that takes `0x102` for the mask instead
rejects `SA_THPRI`, and with it every driver that asks for a
priority-ordered semaphore. `SignalSema`/`iSignalSema`, no waiter: `current < max`
increments it; **`current >= max` returns `0` without incrementing and
without `KE_SEMA_OVF`** — the header names that error, but this firmware
silently caps instead. With a waiter: pops the first (FIFO by default; with
`SA_THPRI` set, a genuine priority-ordered insertion at wait time, comparing
each waiter's current priority — not a no-op despite the flag's obscurity),
decrements, and readies it with its blocked call's return slot zeroed.
`WaitSema`: `current > 0` decrements and returns; `current <= 0` blocks
(`waitType = TSW_SEMA`, `3`). `PollSema`: `current <= 0` returns
`KE_SEMA_ZERO` (`-419`) directly. `KE_UNKNOWN_SEMID` is `-408`.

**IOP-3h — the dispatcher protocol.** Two globals decide everything: a
"current" thread pointer and a "pending-next" thread pointer.
`ShouldPreemptCb` (INTRMAN ordinal 30, IOP-2j) is exactly `pending != current`
— every operation above that decides a switch is warranted writes into
"pending-next" (or leaves it equal to "current"). `NewCtxCb` (INTRMAN
ordinal 28) resolves a mismatch: stores the outgoing thread's just-finished
saved-frame pointer into its own record (the only point that pointer is kept
current), falls back to a ready-queue pick if nothing was pre-selected, and
returns the target thread's own saved-frame pointer, which INTRMAN then loads
as the new stack pointer. Every voluntary block/yield point funnels through
one dedicated reschedule syscall, with up to four caller arguments left in
`$a0`–`$a3` for the handler; the reference's own case body for that syscall
was not independently decoded — a rebuild is free to choose its own syscall
number, but its handler **must** produce the same effect the interrupt-return
tail already produces (top up saved registers, ask `ShouldPreemptCb`, call
`NewCtxCb`). Two further invocation shapes exist: **involuntary preemption**
— a periodic timer interrupt reaching the end of the interrupt-return path
with the current thread still RUN, comparing the lowest ready priority
against the running thread's own; and **immediate hand-off on enqueue** — a
routine called by `StartThread`/`WakeupThread`/`SignalSema`'s wake path/
`ReleaseWaitThread` (never their `i`-suffixed forms) that stages a preemption
for the caller's own return path if the newly-readied thread is strictly
better, or simply enqueues and re-enables interrupts itself otherwise (since
no exception-return path will do it for a syscall-less caller). **The idle
thread**: there is no separate idle library call. Priority `127` — one below
the lowest user-creatable priority, `126` — is reserved by convention for the
one thread THREADMAN's own boot primes with an infinite empty loop as its
entry point (IOP-3i). When the pick finds no ready thread at all, it logs and
returns without selecting anything (not a fatal halt); this never happens
once the idle thread exists.

**IOP-3i — what THREADMAN's entry creates at boot.** In order: register
`thrdman` via the pinned `LOADCORE` API and `thbase`/`thevent`/`thsemap`/
`thmsgbx`/`thfpool`/`thvpool` via the versioned API (`spec/02` IRX-10);
zero the module's own state; initialise the ready-queue sentinels; create a
private internal heap; **create the idle thread inline**, not through
`CreateThread` — a `0x50`-byte record, a `0x200`-byte stack, priority `0x7F`
(`127`), `TH_C` only, entry an infinite empty loop, **state set to READY
(`2`), not RUN** — the same surprise the EE kernel's own boot thread shows,
confirmed here on the IOP side too: a scheduler pick, not direct kernel init,
is normally what sets a thread RUN, and boot bypasses it once; **create a
second thread record for the code already running** — THREADMAN's own entry,
i.e. whatever caller is walking the boot list — reusing the existing stack
(discovered from the current stack pointer) rather than allocating a fresh
one, with initial priority `8` but current priority forced to `1` (highest)
immediately after (an asymmetry the analysis leaves **open** — no reconciling
call was found), state set to RUN directly, and both scheduler globals
pointed at it — this is the thread that goes on to run the rest of the boot
list, since the module loader simply keeps calling each subsequent module's
entry on whatever thread context it is already executing in; register the
two INTRMAN reschedule hooks (`SetNewCtxCb`, `SetShouldPreemptCb`); create an
internal `EA_MULTI` event flag for THREADMAN's own use; and, as one of the
very last steps, call `CpuEnableIntr` — interrupts are turned on only after
every piece of THREADMAN's own state is in place.

**IOP-3j — the clock, and `DelayThread`.** Derived from `docs/analysis/44`.
`THREADMAN`'s entry asks `timrman` for a 32-bit SYSCLOCK timer at prescale 1
(`AllocHardTimer(1, 32, 1)`, ordinal 4), which IOP-7b's scan answers with
RTC5 (`0xBF8014A0`, IRQ 16), and programs it as the reference does, in this
order: `RegisterIntrHandler(16, mode 1, handler)`, `SetTimerCounter(0)`,
`SetTimerCompare(100 us of ticks)`, `SetTimerMode(0x70)` (interrupt on
target and on overflow, no reset on target: the counter free-runs), then
`EnableIntr(16)`. The system clock is that counter with a software high word
advanced whenever the counter is seen to have wrapped — the overflow
interrupt guarantees that at least once per wrap. `USec2SysClock` (ordinal
39) is `usec * 4608 / 125` in 64 bits (36.864 MHz); `SysClock2USec` (40) is
the inverse, split into seconds and the remainder. `GetSystemTime(out*)`
(34) stores the 64-bit clock through its pointer and returns 0 — the
reference's own callers all use the pointer. `DelayThread(usec)` (33)
refuses an interrupt context (`-100`), converts `usec`, sets an alarm on
its own thread record (IOP-3k) and blocks with `waitType = TSW_DELAY` (`2`);
the alarm's callback wakes it from interrupt context and returns 0. A
thread released from a delay by any other path (`TerminateThread`,
`ReleaseWaitThread`) has its alarm cancelled with it.

**IOP-3k — the alarms.** One queue of records `{ deadline (64 bits),
callback, arg }`, sorted ascending by deadline with ties behind the existing
entry. `SetAlarm(clock*, cb, arg)` (35) and `CancelAlarm(cb, arg)` (37) want
a thread context, `iSetAlarm` (36) and `iCancelAlarm` (38) an interrupt
context, `-100` otherwise; the `i` forms do not take the critical section
their caller already holds. `SetAlarm` refuses a second record with the same
`(cb, arg)` (`-104`), answers `-400` when the pool is exhausted, raises a
delay shorter than 100 us to 100 us, computes `deadline = now + delay`,
inserts, and re-arms the timer; `CancelAlarm` unlinks and frees, answers
`-105` for no match, and does not touch the timer — a stale compare value
merely makes the next interrupt find nothing due and re-arm. Arming writes
the compare register with the low word of the head's deadline — or of the
last of a run of heads within 200 us of one another, so they share one
interrupt — and, for a deadline already past, with the counter plus 200 us.
The interrupt handler reads the timer's status (which clears it), advances
the clock, and if the target bit was set walks the queue from the head while
`deadline <= now`: a permanent record, seeded 2 ms overdue at the entry and
never freed, is re-queued one counter period (`2^32` ticks) later and keeps
the clock fresh when nothing else is pending; every other record's callback
runs with its `arg`, a return of 0 frees the record and a nonzero return
re-queues it `min(return, 200 us)` after its old deadline. No periodic
scheduling tick exists: the timer interrupt never preempts by itself beyond
what any interrupt's return path does (IOP-3h).

## IOP-4: Files (IOMAN, ROMDRV)

Derived from `docs/analysis/39` §1–2.

**IOP-4a — the device descriptor and its ops table.** `AddDrv`/`DelDrv`
(ordinals 20/21) take an `iop_device_t*` and keep the **pointer**, not a
struct copy. `AddDrv` validates a driver by calling `ops->init(device)`; a
negative return rolls the registration back. `DelDrv` calls `ops->deinit` on
a name match, clears the slot, and always returns `0` regardless of
`deinit`'s own result. The ops-table slot order (offsets from the `ops`
pointer, confirmed field-for-field against `iop_device_ops_t` [header]):

| Field | `ops+` | Field | `ops+` | Field | `ops+` |
| --- | --- | --- | --- | --- | --- |
| init | `0x00` | write | `0x18` | dclose | `0x34` |
| deinit | `0x04` | lseek | `0x1C` | dread | `0x38` |
| format | `0x08` | ioctl | `0x20` | getstat | `0x3C` |
| open | `0x0C` | remove | `0x24` | chstat | `0x40` |
| close | `0x10` | mkdir | `0x28` | dopen | `0x30` |
| read | `0x14` | rmdir | `0x2C` | | |

**IOP-4b — the driver table.** A small fixed-capacity table of raw
`iop_device_t*` pointers (the reference uses 16 slots); no name-collision
detection is required — the reference has none.

**IOP-4c — the fd table and path parsing.** File descriptors are `iop_
file_t` records, 16 bytes each (`mode`/`unit`/`device`/`privdata` at `0`/`4`/
`8`/`0xC`), allocated from a fixed table (the reference: 16 slots) by "first
record whose `device` field is zero"; the returned fd is the raw table
slot index. `open`'s path parser splits at the first `:`, then walks
**backward** from the colon over trailing decimal digits, parsing that run
into a unit number (default `0` if none) and **truncating those digits out of
the copied device name** before matching the driver table — the mechanism by
which `rom0:`/`rom1:` are the *same* device, split by unit entirely inside
`IOMAN`, before the driver ever sees a digit (IOP-4e).

**IOP-4d — the call into a driver, and error codes.** `open` dispatches as
`ops->open(file, remainder, flags)` — **exactly three arguments**: `file` is
the freshly-allocated `iop_file_t*` with `mode`/`unit`/`device` already
filled in, `remainder` is the tail of the *original* path string past the
colon (not a copy, not the full `"dev:tail"` string), `flags` is passed
through unmodified. `read`/`lseek`/`close` validate the fd (range `< 16`,
`device == 0` meaning "not open") and dispatch through the corresponding ops
slot with the caller's own arguments; `lseek` additionally range-checks
`whence` (`0..2`) before dispatching. An unrecognized device name (no `:`, or
no driver matches) makes `open` return **`-19`** — not a `kerr.h` [header]
constant; it happens to equal POSIX `ENODEV`, noted as an observation. When
the driver's own `open` returns negative, `IOMAN` passes that value back
completely unmodified and releases the fd slot it had provisionally claimed;
the same unmodified-passthrough shape applies to `remove`/`mkdir`/`rmdir`/
`getstat`/`chstat`/`dopen`/`format`.

**IOP-4e — `ROMDRV`'s device.** Registers exactly **one** device, named
`"rom"` (four bytes including the NUL) — never `"rom0"`/`"rom1"`; `type =
0x10` (`IOP_DT_FS` [header]). `open` reads the `iop_file_t.unit` field and
range-checks it `< 4` (else `-6`), indexing a four-entry `RomImg[]` table
(`{ ImageStart, RomdirStart, RomdirEnd }`, 12 bytes each [header]).
`ROMDRV`'s own init self-registers **unit 0** by re-running the ROMDIR
self-locating scan (`spec/01-rom-archive.md`'s `ARC-3`/`ARC-4`); units 1–3
start empty, for a later `romAddDevice` call to fill (nothing in the
reference archive does).

**IOP-4f — `ROMDRV`'s `open`/`read`/`lseek`/`close`.** `open`, after the
unit check, also requires `flags == 1` exactly (else `-13`), linear-scans the
ROMDIR entries of that unit's image for a name match, claims a free slot of
an eight-entry open-handle table (critical-section-guarded); no match
returns `-2`, a full handle table returns `-12`. `read` clamps the requested
length to `filesize − position`, copies, advances position, and returns the
actual (possibly short) byte count — it **never refuses a read past EOF**,
only truncates it. `lseek` implements `SEEK_SET`/`CUR`/`END` (`0`/`1`/`2`,
else `-22`), clamping the result to `[0, filesize]`. `close` validates the
handle index (`< 8`, else `-9`) and requires it currently in-use (else also
`-9`). None of `-2`/`-6`/`-9`/`-12`/`-13`/`-22` match a `kerr.h` [header]
constant; they read as a small file-I/O errno family distinct from the
kernel `KE_*` range, with no canonical name found.

**IOP-4g — stubbed ops, and `romdrv`'s own exports.** `ioctl`, `remove`,
`mkdir`, `rmdir`, `dopen`, `dclose`, `dread`, `getstat` and `chstat` all
share one "return `0`" stub — a flat, read-only device reports no file size
except by opening and seeking. `write` is its own dedicated "return `-5`"
stub, distinct from the shared no-op — writing is a deliberate hard error.
`romdrv`'s own six library exports: ordinal 0 is the internal init routine,
re-exported; ordinals 1–3 are a shared reserved stub; ordinal 4 is
`romAddDevice(unit, image) -> 0 | -160 (ROMDRV_ADD_FAILED) | -162
(ROMDRV_ADD_BAD_IMAGE)` [header, values confirmed exact]; ordinal 5 is
`romDelDevice(unit) -> 0 | -161 (ROMDRV_DEL_FAILED)` [header, exact].
`romGetDevice` (a newer header's ordinal 6) does not exist in this table.

## IOP-5: Module loading (LOADCORE, MODLOAD, LOADFILE)

Derived from `docs/analysis/39` §3–4. `spec/03-boot-chain.md` BOOT-12e
already fixes the shape of the SIF-RPC request/reply `LOADFILE`'s server
(`sid = 0x80000006`) answers; this covers only the IOP side of that answer.

**IOP-5a — `LOADCORE` ordinals `MODLOAD` uses.** `3` `GetLibraryEntryTable`/
`GetLoadcoreInternalData` [header, both names appear across header versions],
`4` `FlushIcache`, `6` `RegisterLibraryEntries`, `8` `LinkLibraryEntries`, `9`
`UnLinkLibraryEntries`, `12` `QueryBootMode`, `16` `RegisterModule`, `17`
`ReleaseModule`, `20` `AddRebootNotifyHandler`, `21` `SetCacheCtrl`, `22`
`ProbeExecutableObject`, `23` `LoadExecutableObject` [header].

**IOP-5b — the probe/allocate/load/link/flush/register sequence.** No ELF
magic check exists in `MODLOAD` itself — that belongs to `LOADCORE`.
`ProbeExecutableObject` (ordinal 22) runs **before any allocation**, on the
raw file buffer, to parse headers and compute a size/type; a type outside
`{1, 2, 3, 4}` is `KE_ILLEGAL_OBJECT` (`-201`). Only then is the destination
allocated (`sysmem` ordinal 4), and `LoadExecutableObject` (ordinal 23) runs
on the same raw image plus the completed struct — the one that actually
copies/relocates sections into their final address. The raw file buffer is
freed immediately after. Order is **link, then flush, then register**:
`LinkLibraryEntries`'s (ordinal 8) return value **is** checked — negative
frees the allocation and returns `KE_LINKERR` (`-200`) — then
`FlushIcache` (ordinal 4), then `RegisterModule` (ordinal 16), whose own
return value is discarded (the caller returns its own locally-computed
record pointer regardless).

**IOP-5c — the module record.** The first `0x30` bytes of the final
allocation are `ModuleInfo_t` [header]; `spec/02` IRX-12c already fixes
`entry` at `+0x10` and `gp` at `+0x14`. This adds: `id`, a 16-bit value, at
`+0xC`, immediately before them, in field order `next, name, version,
newflags, id, flags, entry, gp, text_start, text_size, data_size,
bss_size` [header]. No counter for assigning `id` exists in `MODLOAD`'s own
state; it is assigned somewhere inside `LOADCORE`'s probe/load/register path
(`lc_internals_t.module_index` [header] is the plausible location — **open**,
not disassembled to an instruction).

**IOP-5d — argc/argv, and `*result`.** The entry-call helper walks the
caller-supplied argument bytes twice: once to count NUL-terminated tokens up
to the given length (argc starts at `1`, for the module's own name), once to
copy the module's name as `argv[0]`, `memcpy` the argument bytes immediately
after it, and rebuild an `argv[]` pointer array, NUL-terminating
`argv[argc]`. After the entry call, `*result` receives the entry function's
raw return value **completely unmodified** — the same value `spec/02`
IRX-12a's `return & 3` tests for residency.

**IOP-5e — the error table.**

| Constant | Value | Trigger |
| --- | --- | --- |
| `KE_LINKERR` | `-200` | `LinkLibraryEntries` returned negative |
| `KE_ILLEGAL_OBJECT` | `-201` | `IsIllegalBootDevice` (ordinal 15) rejects the path, **or** `ProbeExecutableObject`'s type is not in `{1,2,3,4}` |
| `KE_UNKNOWN_MODULE` | `-202` | `StartModule`'s by-id lookup found nothing |
| `KE_NOFILE` | `-203` | `IOMAN` `open` (ordinal 4) returned negative |
| `KE_FILEERR` | `-204` | `lseek(fd, 0, SEEK_END)` returned `<= 0`, **or** `read()` returned fewer bytes than requested |
| `KE_MEMINUSE` | `-205` | a fixed-address allocation failed and a follow-up `sysmem` query found the target range already occupied |
| `KE_NO_MEMORY` | `-400` | `sysmem` ordinal-4 allocation failed (raw file buffer or final image) |

**IOP-5f — `LOADFILE`'s thread and RPC registration.** `LOADFILE` exports
nothing; its entry builds an IOP thread (priority `0x58` = `88`, stack
`0x1000` = 4096 bytes) via `thbase`'s `CreateThread`/`StartThread` — the RPC
service, including its own handshake, runs on a **dedicated thread**, never
the boot thread. That thread makes exactly one `InitRpc`, one
`SetRpcQueue(queue, threadid)`, one `RegisterRpc(sd, sid = 0x80000006, func,
buf, cfunc = NULL, cbuf = NULL, qd)` and one `RpcLoop(queue)` call, in that
order; `RpcLoop` never returns. The registered dispatch function receives
`(fno, buf, size)`, drops `size`, range-checks `fno < 6` (out of range
produces **no reply at all**, not an error code), and tail-calls a table
entry keyed by `fno`. `fno 0` (IOP module load) and `fno 1` (an EE ELF,
IOP-5h) are in scope; `fno` 2–5 (a memory peek/poke pair and
`LoadStartKelfModule`) exist for the OSD's own boot path and are not needed
yet.

**IOP-5g — `fno 0`, and where `-201`/`-203` come from.** `BOOT-12e`'s
request layout (`arg_len@+0`, `path@+8`, `args@+0x104`) is read, then
`IsIllegalBootDevice(path)` (`MODLOAD` ordinal 15) runs **first**: rejected
→ the reply's first word is `-201` and `LoadStartModule` is **never called**
(so the reply's second word is left unwritten, since only `LoadStartModule`
writes it). Otherwise `path` is passed to `LoadStartModule` (`MODLOAD`
ordinal 7) **completely unmodified** — no prefix, no rewrite — and its own
return (id or error) becomes the reply's first word, its own `*modres`
out-parameter the reply's second. `IsIllegalBootDevice` runs a **second**
time, identically, inside `LoadStartModule` itself (reachable from other IOP
callers that never go through `LOADFILE`), giving the same `-201` from a
different site. **Open: `IsIllegalBootDevice`'s own rule** — what makes a
path "illegal" — was not established; only its call sites and shared `-201`
answer are pinned. The **`-203`** a rebuild must answer for a real filename
that is simply not present (e.g. `rom0:NOSUCH`) is a *different* code from a
*different* site: `MODLOAD`'s own fixed substitution for any `IOMAN` `open`
failure (IOP-5e) — the underlying `IOMAN`/`ROMDRV` error is discarded, not
passed through.

**IOP-5h — `fno 1`, the EE ELF loader `EELOAD` asks for.** The request is
`{ epc, gp, path[252], secname[252] }` (`0x200` bytes) and the answer
`{ epc | error, gp, 0, 0 }` (`docs/analysis/41` §4). `LOADFILE` opens `path`
through `IOMAN`, reads the ELF and program headers, and puts every `PT_LOAD`
segment in EE memory at its `p_paddr` — file bytes, then zeros to `p_memsz` —
through the `sifman` library's `SetDma`/`DmaStat` (ordinals 7 and 8: a
transfer list of `{ src, dest, size, attr }`, and `-1` once a transfer's
interrupt has come, `0` while it runs). Transfers are quadwords to quadword
addresses, so a segment that starts or ends inside a quadword shares that
block with its neighbour, and the block is sent whole with the neighbour's
bytes in it. `epc` answers the ELF's entry and `gp` is `0`; an open or read
failure answers `-1` and loads nothing further.

## IOP-6: SIO2MAN's start-up contract

Derived from `docs/analysis/40`.

**IOP-6a — the exact sequence of kernel calls.** `SIO2MAN`'s entry, in
order: register its 26-entry export table (`LOADCORE` ordinal 6); check and
set a one-shot residency flag (return `1` — the same value — on either
registration failure or on an already-resident module); write the module's
own initial hardware `CTRL` value once (IOP-6b); `CreateEventFlag({ attr =
EA_MULTI (2), initBits = 0 })` [header]; `CreateThread({ attr = TH_C
(0x02000000), option = 0, stacksize = 0x2000 (8192), priority = 0x18 (24)
})` [header]; `CpuSuspendIntr`; `RegisterIntrHandler(irq = 0x11 (17), mode =
1, handler, arg)`; `EnableIntr(0x11)`; `CpuResumeIntr`; `sceSetDMAPriority(
channel = 11, priority = 3)`; `sceSetDMAPriority(channel = 12, priority =
3)`; `sceEnableDMAChannel(11)`; `sceEnableDMAChannel(12)`; `StartThread`;
return `0`. **`SIO2MAN` installs exactly one interrupt handler, for IRQ 17
— DMA channels 11 and 12 are primed through `DMACMAN` accessors only and
never given their own `RegisterIntrHandler` call.** `_deinit` (ordinal 2) is
the exact mirror: `CpuSuspendIntr`, `DisableIntr(0x11)`,
`ReleaseIntrHandler(0x11)`, `CpuResumeIntr`, `sceDisableDMAChannel(11)`,
`sceDisableDMAChannel(12)` — no `DeleteThread`/`TerminateThread` call.

**IOP-6b — the hardware registers, and what an emulator must do.** `SIO2MAN`
addresses its register block as 33 consecutive words from `0xBF808200` to
`0xBF808280` (physical `0x1F808200`–`0x1F808280`); `CTRL` is the accessor
named `sio2_ctrl_set`/`sio2_ctrl_get`, at index 26, physical `0x1F808268` —
**not** `0x1F808260`, which is `data_out`. `SIO2MAN` writes `CTRL` once at
start with a fixed literal value `0x3BC` (bit-for-bit meaning not decoded),
and per-transfer sets bits `0xC` before pushing register data and bit `0x1`
to start the transfer. `STAT` (index 32, `0x1F808280`) is read and written
**only** inside the IRQ-17 handler: read, then written back with **the same
value just read** — a write-of-1-clears-pending acknowledge, inferred from
the code's own symmetry, documented nowhere else in this binary. **No
polling loop on any SIO2 register exists in `SIO2MAN`'s own code.** Instead,
the driver sets `CTRL` bit 0 once per transfer and blocks the service thread
in `WaitEventFlag` for an event-flag bit only the IRQ-17 handler sets. An
emulator's SIO2 model must therefore, at minimum: accept a `CTRL` bit-0
write as "start a transfer", complete the programmed transfer, and then
raise IOP `I_STAT` bit **17** — `SIO2MAN` supplies no fallback path if it
does not, and this (not a CPU-side spin loop) is the actual mechanism behind
the "SIO2MAN spins" symptom an emulator without this behaviour produces.

**IOP-6c — "resident with its thread waiting."** After `_start` returns
`0`, the module stays loaded and its service thread is parked in
`WaitEventFlag(ef, 0x5, OR)`, blocked for either of two request bits a
pad-mode or memory-card-mode transfer-init call would set — it does no
further work and touches no hardware until one of those calls (or a
`sio2_transfer` call once claimed) wakes it. A rebuild's `LoadStartModule`
of `SIO2MAN` is only faithful once the module reaches exactly this parked
state: registered, its thread running independently of the caller, and
blocked on its own event flag rather than having exited or faulted.

## Verification

**IOP-1 and IOP-2** describe run-time behaviour with no ROM-address-specific
component, unlike the boot chain's fixed addresses — a rebuild's own
`EXCEPMAN`/`INTRMAN` are free to place their tables anywhere, so nothing here
is checkable by disassembling a fixed offset the way `spec/03`'s BOOT-1/
BOOT-5 are. `tools/ps2sim.py` models the IOP's interrupt controller and the
DMA completion interrupts (`docs/analysis/24`), so `python3 tools/ps2sim.py
build/rom.bin` and `ninja -C build check` boot the image only if the
`EESYNC` service registered with `INTRMAN` is reached through the vector and
the chains; the hand-written paths of both modules are checked for the
R3000's load-delay hazard by the same target.

## IOP-7: Timers (TIMRMAN)

Derived from `docs/analysis/44` §1–§3.

**IOP-7a — the six timers.** The library is `timrman` v1.01, 17 ordinals
(0–16, the reference has no 17 or 18): 3 `GetTimersTable`, 4
`AllocHardTimer(source, size, prescale)`, 5 `ReferHardTimer(source, size,
mode, modemask)`, 6 `FreeHardTimer(id)`, 7 `SetTimerMode(id, mode)`, 8
`GetTimerStatus(id)`, 9 `SetTimerCounter(id, count)`, 10
`GetTimerCounter(id)`, 11 `SetTimerCompare(id, compare)`, 12
`GetTimerCompare(id)`, 13 `SetHoldMode(n, mode)`, 14 `GetHoldMode(n)`, 15
`GetHoldReg(n)`, 16 `GetHardTimerIntrCode(id)` [header]. A timer id is its
register base shifted right by two, and every accessor recovers the
register from the id with that shift alone. The table, in the allocator's
scan order, is RTC2 `0xBF801120` (SYSCLOCK, 16 bits, prescale 8, IRQ 6), RTC5
`0xBF8014A0` (SYSCLOCK, 32, 256, IRQ 16), RTC4 `0xBF801490` (SYSCLOCK, 32,
256, IRQ 15), RTC3 `0xBF801480` (SYSCLOCK|HLINE, 32, 1, IRQ 14), RTC0
`0xBF801100` (SYSCLOCK|PIXEL|HOLD, 16, 1, IRQ 4), RTC1 `0xBF801110`
(SYSCLOCK|HLINE|HOLD, 16, 1, IRQ 5); source bits are 1 SYSCLOCK, 2 PIXEL, 4
HLINE, 8 HOLD [header].

**IOP-7b — allocation.** `AllocHardTimer` walks the table in that order and
claims the first timer not in use whose sources intersect the request, whose
size equals it, and whose prescale is **at least** the one asked for — not
equal to it — answering the id, or `-1`. In-use is a count: `FreeHardTimer`
decrements it and answers 0, or `-150` for a timer not held. `ReferHardTimer`
is the same scan without the claim.

**IOP-7c — what the kernel gets.** `AllocHardTimer(1, 32, 1)` therefore
answers RTC5: RTC2 fails on size, RTC5's prescale of 256 satisfies `>= 1`.
That is the timer `THREADMAN` programs (IOP-3j), and IRQ 16 is the line its
handler is registered on.

**IOP-7d — the registers.** Count at `+0`, mode at `+4` (16 bits), compare
at `+8`; count and compare are 16 bits for RTC0–2 and 32 bits for RTC3–5
(every register at or above `0xBF801480`). `GetTimerStatus` reads the mode
register, which the hardware clears of its target and overflow bits on the
read — the reference acknowledges a timer interrupt no other way.

**IOP-7e — the hold block.** Ordinals 13–15 address `0xBF8014C0 + 4n`
(mode) and `0xBF8014B0 + 4n` (value); nothing on the boot path uses them.

## IOP-8: The disc (CDVDMAN)

Derived from `docs/analysis/42-cdvd.md` §0–§4, §7.

**IOP-8a — the `cdrom` device and its ops table.** `CDVDMAN` registers one
`IOMAN` device, named `cdrom` (no digit — the same `IOMAN`-side unit-splitting
`ROMDRV`'s `rom` uses, IOP-4e), `type = 0x10` (`IOP_DT_FS` [header]). Real
`init`/`deinit`/`open`/`close`/`read`/`lseek`; every other slot — `format`,
**`write` included**, `ioctl`, `remove`, `mkdir`, `rmdir`, `dopen`, `dclose`,
`dread`, `getstat`, `chstat` (eleven slots) — shares one "return `0`" stub
(§1). This is the one point `CDVDMAN` differs from `ROMDRV`'s ops table
(IOP-4g): `ROMDRV` gives `write` its own dedicated error stub, `CDVDMAN`
routes it through the same no-op every other unimplemented slot uses, so a
`write` against `cdrom0:` reports success without writing anything.
`AddDrv`'s own contract (IOP-4a) still applies: `init` runs before
registration completes and may veto it.

**IOP-8b — no dedicated service thread.** Every hardware command — the
N-command send, the DMA arm — runs synchronously on whichever thread called
into the driver (`open`/`read`, or an exported `sceCd*` call), not on a
service thread of `CDVDMAN`'s own. Concurrent callers are serialised by
semaphore rather than handed to a queue: one semaphore for the whole
open/read/search sequence and the one-sector cache it shares, a second for
the N-command channel itself. Only the second is polled non-blocking
(`PollSema`, declining immediately rather than queueing) — an unready or
busy channel is a declined call, not a wait.

**IOP-8c — the N-command register block.** A block separate from the
S-command pair `docs/analysis/26-cdvd-nvm-and-config.md` already covers:

| Addr | Use |
| --- | --- |
| `0xBF402004` | write: N-command number, starts the command |
| `0xBF402005` | read: status — bit `0x80` busy, bit `0x40` set means ready, precondition to send is `(status & 0xC0) == 0x40`; write: one parameter byte, looped |
| `0xBF402006` | write: the one-byte submode, set before the command; read: the command's raw result byte |
| `0xBF402007` | write-only: `1` aborts the current command (`sceCdBreak`) |
| `0xBF402008` | read: bit 0, tested by the IRQ-2 handler; write: `1` (done) or `2` (retry) to acknowledge |
| `0xBF40200A` | read: drive state — `0x0A` is "ready with a disc" (`SCECdStatPause` [header]) |
| `0xBF40200F` | read: disk-type byte, returned raw by `sceCdGetDiskType` |

**IOP-8d — a 2048-byte sector read, register by register.** The
datapattern-0 case — the only shape a plain `sceCdRead(lbn, sectors, buf,
NULL)` takes, and the only one this module builds:

1. Arm DMA channel 3 (poked directly; no `dmacman` import, matching the
   reference): `CHCR (0xBF8010B8) = 0`, `MADR (0xBF8010B0) = dest`, `BCR
   (0xBF8010B4) = (sectors << 16) | 0x200`, `CHCR = 0x41000200`. `0x200` is
   the per-block size in words: `2048 / 4 = 512 = 0x200`, one block per
   sector — the value a rebuild derives once it knows the target is a plain
   2048-byte sector, distinct from the multiplier the reference's other
   datapattern branches feed into the command's own length field.
2. Require `(0xBF402005 & 0xC0) == 0x40` — a precondition check, not a spin;
   an unready channel declines the call outright. (`sceCdInit(mode=0)` is
   the one place that actually spins on this same bit, before any command is
   ever sent — IOP-8g.)
3. Write the submode byte to `0xBF402006`. The reference's observed set is
   `{0x40, 0x80, 0x83, 0x85, 0x86, 0x8f}`, chosen by disk type and
   datapattern; a rebuild handling only the plain PS2 CD/DVD, datapattern-0
   case uses the fixed value `0x80`.
4. Write an 11-byte parameter block to `0xBF402005`, one byte at a time:
   `lbn` little-endian at `+0..3`, `sectors` little-endian at `+4..7`,
   `trycount` at `+8` (0 when the caller's mode is defaulted/`NULL`), the
   same submode byte at `+9`, datapattern (`0`) at `+10`.
5. Write `6` to `0xBF402004` — the command starts. The call returns as soon
   as this is accepted; the sector bytes are not yet at `dest`.

**IOP-8e — the completion contract.** `RegisterIntrHandler(irq=2, mode=1,
...)` (`intrman` ordinal 4) and `EnableIntr(2)` (ordinal 6) install the
completion side once, from the module's own init; `DPCR (0xBF8010F0) |=
0x8000` enables channel 3's own DMA bit at the same time. IRQ 2's handler:
reads `0xBF402006` into the byte `sceCdGetError` later returns; reads
`0xBF402008` bit 0 — clear acknowledges with `2` and leaves completion
pending (a retry cycle); set reads `0xBF402005` bit 0 to decide the
completion word (`1` ok, `-1` error) and acknowledges with `1`. No
`SignalSema` happens in the handler and no completion is semaphore-driven:
`sceCdSync(mode)` (ordinal 11) is the caller-facing wait, and it busy-polls
the completion word — mode `0` sleeps 1000 µs (`DelayThread`, `thbase`
ordinal 33) between checks and blocks until it is set; mode `1` checks once
and returns immediately. `sceCdCheckCmd` (ordinal 21) returns the same word
raw, unblocking. This is deliberately not a cleaner semaphore wait: it
reproduces the reference's own call-and-poll shape, which is what a title's
timing assumptions may depend on.

**IOP-8f — ISO9660 path resolution.** A path is `\DIR\FILE;1`-shaped, the
same form `SYSTEM.CNF`'s own `BOOT2` line uses and what `IOMAN` hands the
driver's `open` past the device's colon. Resolution:

1. Read LBA 16 (the Primary Volume Descriptor) and check `"CD001"` at byte
   offset 1; a mismatch fails resolution outright.
2. The PVD's own root directory record, at PVD offset 156 (34 bytes), gives
   the root directory's extent LBA (offset `+2`, little-endian 32-bit) and
   size (offset `+10`).
3. The path must begin with `\`; anything else is a silent "not found."
4. Split the remainder at `\` into components, descending up to 8 levels
   (`CdlMAXLEVEL` [header]) — deeper paths fail.
5. At each level, scan the current directory's extent sector by sector.
   Each ISO9660 directory record: length byte at `+0` (`0` means padding to
   the sector boundary — records never split across sectors), extent LBA at
   `+2` (little-endian 32-bit), data length at `+10`, flags at `+25` (bit 1
   set means a subdirectory), file-identifier length at `+32`, the
   identifier itself at `+33`. A component matches an identifier either
   exactly or, when the component itself carries no `;version` suffix,
   against the identifier's own name with its `;1` suffix stripped.
6. A match that is not the path's last component must be a directory, and
   descent continues into its extent; a match on the last component yields
   the file's own extent LBA and size — exactly what `sceCdRead` needs.

Every metadata and data sector this resolution and the driver's own `read`
touch goes through one shared, one-sector cache keyed by LBA, so a
byte-range request that does not start on a sector boundary (`LOADFILE`'s
own 4 KiB reads at arbitrary offsets) costs one hardware read per sector
actually touched, not one per call.

**IOP-8g — the exports.** Ordinal numbering matches the reference's 62-entry
table (§0); a rebuild's minimal table implements the following and shares
one "return `0`" filler across every other slot, so the table's own size
matches the reference's exactly:

| Ord | Export | Behaviour |
| --- | --- | --- |
| 0 | (module init, re-exported) | registers the `cdrom` device (`AddDrv`) |
| 4 | `sceCdInit(mode)` | mode `0` spins on the N-command ready bit before installing the IRQ/DMA state; every mode installs it |
| 6 | `sceCdRead(lbn, sectors, buf, mode)` | non-blocking: IOP-8d, ignores `mode` (datapattern 0 only) |
| 8 | `sceCdGetError()` | the IRQ-2 handler's stored result byte |
| 10 | `sceCdSearchFile(sceCdlFILE*, path)` | IOP-8f, filling `{lsn, size, name[16], date[8]}` |
| 11 | `sceCdSync(mode)` | IOP-8e |
| 12 | `sceCdGetDiskType()` | raw `0xBF40200F` |
| 13 | `sceCdDiskReady(mode)` | mode `1` a single check; otherwise a bare spin on `0xBF40200A == 0x0A`, no event flag |
| 21 | `sceCdCheckCmd()` | the raw completion word IOP-8e describes |
| 28 | `sceCdStatus()` | raw `0xBF40200A` |
| 39 | `sceCdBreak()` | writes `1` to `0xBF402007` |
| 46 | `sceCdNop()` | no-op |

**IOP-8h — what is out of scope for this minimal driver.** No S-commands
(NVM, OSD configuration, disc keys — `docs/analysis/26`), no CD streaming
API (`sceCdSt*`, ordinals 56–61), no `CDVDFSV` RPC surface (§5) and
therefore no EE-facing SIF service for any of the above — an EE client
reaches only what a later `CDVDFSV` build forwards. `sceCdRead`'s `mode`
argument (trycount/spindlectrl/datapattern) is accepted but not
interpreted: every read is issued as the plain 2048-byte, datapattern-0
case IOP-8d describes, matching what `LOADFILE`'s own ELF-loading path
needs and nothing beyond it.

## IOP-9: The EE's file service (FILEIO)

Derived from `docs/analysis/43-fileio-and-title-boot.md` §1-§5.

**IOP-9a — the service and its thread.** `FILEIO` exports nothing; its
entry creates two IOP threads, both priority `0x60` (96), and starts each in
turn (§1). Thread 1, on a `0x1000`-byte (4 KiB) stack, is the file-serving
RPC: it runs `sceSifCheckInit`, conditionally `sceSifInit`, `sceSifInitRpc`
(`$a0 = 0`, wait mode), `sceSifGetThreadId`, `sceSifSetRpcQueue`,
`sceSifRegisterRpc(sd, sid = 0x80000001, func, buf, cfunc = 0, cbuf = 0,
qd)`, then `sceSifRpcLoop`, which never returns — the same registration
shape `IOP-5f` already fixes for `LOADFILE`'s own thread. Thread 2, on a
`0x800`-byte (2 KiB) stack, registers a second service, `sid = 0x80000003`,
with its own 3-mode dispatch (alloc / free / open-read-whole-file); no
caller or purpose for it was identified in the analysis (§1, §12) and it is
not part of this specification's required surface.

**IOP-9b — the bounce buffer, allocated lazily.** A shared routine probes
for the largest scratch allocation it can get: it seeds a chosen chunk size
with `0x4000` (16 KiB) and calls `sysmem`'s allocator; on failure it halves
the size (plain truncating divide-by-two) and retries, up to eight times,
down to a floor of 128 bytes (§2). This probe is not run at module entry.
`open` and `write` each test whether the allocation has already succeeded
and run the probe themselves if not, answering `-1` if even the smallest
attempt fails; `read` uses the buffer unconditionally, without the same
guard — an apparent unguarded assumption that a real client always `open`s
before it `read`s (§2, §12).

**IOP-9c — the fno dispatch table.** The registered function receives
`(fno, buf, size)`, drops `size`, range-checks `fno < 0x11` (17) — an
out-of-range `fno` gets no reply at all, the same shape `IOP-5f` already
fixes for `LOADFILE` — and jumps through a 17-entry table (§3):

| fno | operation | ioman ordinal |
| --- | --- | --- |
| 0 | open | 4 |
| 1 | close | 5 |
| 2 | read | 6 (chunked, IOP-9e) |
| 3 | write | 7 (chunked, IOP-9e) |
| 4 | lseek | 8 |
| 5 | ioctl | 9 |
| 6 | remove | 10 |
| 7 | mkdir | 11 |
| 8 | rmdir | 12 |
| 9 | dopen | 13 |
| 10 | dclose | 14 |
| 11 | dread | 15 (+ `sceSifSetDma`) |
| 12 | getstat | 16 (+ `sceSifSetDma`) |
| 13 | chstat | 17 |
| 14 | format | 18 |
| 15 | AddDrv | 20 |
| 16 | DelDrv | 21 |

Every handler is called through a common thunk that fixes `$a0` to the
request buffer and `$a2` to a reply buffer 112 bytes before it; the
wrapper's own return value is that same reply pointer, sent back as the
RPC's result body — again the shape `IOP-5f` already fixes. A likely retail
defect is flagged rather than asserted: fno 6 (`remove`)'s thunk is short
five instructions relative to every other entry's, so a `remove` call may
fall through into a `mkdir` call using the same buffer before the real
reply is sent (§3); not exercised under a simulator or emulator in the
analysis, so its reachability is open (§12) and it is not part of this
specification's required behaviour.

**IOP-9d — request and answer layouts.** Offsets are from the fixed request
buffer unless noted; every answer below is a fixed-size reply area (§4).
Field order is per-operation, not a shared convention — `open`'s path
starts at `+4` (not `+0`) while `getstat`'s destination address starts at
`+0` with its path at `+4`:

```
open    (fno 0):  { mode:u32 @0, path:cstr @4 }              -> { fd_or_err:s32 }
close   (fno 1):  { fd:u32 @0 }                                -> { result:s32 }
read    (fno 2):  { fd:u32 @0, dest_ee_addr:u32 @4, length:u32 @8 } -> { count_or_err:s32 }
write   (fno 3):  { fd:u32 @0, ?:u32 @4, length:u32 @8,
                     first_chunk_len:u32 @0xc, first_chunk_data @0x10, ... } -> { count_or_err:s32 }
lseek   (fno 4):  { fd:u32 @0, offset:s32 @4, whence:u32 @8 }  -> { position:s32 }
ioctl   (fno 5):  { fd:u32 @0, cmd:u32 @4, arg @8 }            -> { result:s32 }
remove  (fno 6):  { path:cstr @0 }                              -> { result:s32 }
mkdir   (fno 7):  { path:cstr @0 }                              -> { result:s32 }
rmdir   (fno 8):  { path:cstr @0 }                              -> { result:s32 }
dopen   (fno 9):  { path:cstr @0 }                              -> { fd_or_err:s32 }
dclose  (fno 10): { fd:u32 @0 }                                  -> { result:s32 }
dread   (fno 11): { fd:u32 @0, dest_ee_addr:u32 @4 }            -> { count_or_err:s32 }
                   -- a 0x12c-byte dirent DMA'd straight to dest_ee_addr, not in the reply
getstat (fno 12): { dest_ee_addr:u32 @0, path:cstr @4 }         -> { result:s32 }
                   -- a 0x28-byte (40-byte) stat block DMA'd straight to dest_ee_addr
chstat  (fno 13): { mask:u32 @0, stat:[0x28 bytes] @4, path:cstr @0x2c } -> { result:s32 }
format  (fno 14): { path:cstr @0 }                               -> { result:s32 }
AddDrv  (fno 15): { device_ptr:u32 @0 }                          -> { result:s32 }
DelDrv  (fno 16): { name:cstr @0 }                                -> { result:s32 }
```

`getstat`'s 40-byte transfer and `chstat`'s inline 40-byte stat block agree
with each other on that size (`chstat`'s path begins exactly at
`+4 + 0x28`); `write`'s request layout beyond its first inline chunk, and
the field at `+4`, were not fully resolved (§5, §12). `ioctl`'s `arg` rides
inline in the request buffer rather than being copied elsewhere, so an
`ioctl` needing a larger argument is not served by this path (§4).

**IOP-9e — the `read`/`write` data path: alignment, `SifSetDma`,
`GetOtherData`.** For a request of 16 bytes or more, `read` splits the
transfer into three pieces by the 16-byte alignment of `dest_ee_addr`, not
of the file offset (§5):

- a **head**, `(16 - (dest_ee_addr & 0xf)) & 0xf` bytes, bringing the
  destination onto a 16-byte boundary (zero if already aligned);
- a **middle**, the largest run from there that is itself a multiple of 16
  bytes;
- a **tail**, whatever remains, under 16 bytes.

Head and tail are each read into small fixed scratch buffers via a plain
`ioman` read, then placed at the EE address with `sceSifGetOtherData`
(`sifcmd` ordinal 23, `SIF_CMD_RPC_RDATA`) — the RPC layer's own
arbitrary-alignment primitive, used because the raw DMA engine cannot
target an address of any alignment. The aligned middle goes through the
shared bounce buffer (IOP-9b), one probed chunk at a time: an `ioman` read
into the bounce buffer, then, inside a critical section
(`CpuSuspendIntr`/`CpuResumeIntr`), a raw `sceSifSetDma` call with a
single-entry transfer descriptor `{ src = bounce_buffer, dest =
current_ee_addr, size = chunk, attr = 0 }`; before reusing the bounce
buffer for the next chunk, the handler polls `sceSifDmaStat` in a tight
loop until the previous transfer has drained. The net rule: an EE
destination needs no alignment at all for `read` to serve it correctly — a
request under 16 bytes is served as a single unaligned fragment through
`GetOtherData` alone (`IOP-9c`'s dispatch note). `write` shares the same
lazily-allocated bounce buffer and the same two mechanisms, moving data the
other way through `ioman`'s `write` ordinal; its own request layout beyond
the first inline chunk was not fully resolved (`IOP-9d`).

**IOP-3 through IOP-6** are exercised by the M1 program (`docs/project-state.md`
§6) on the two targets: `LOADFILE`'s thread is woken from an interrupt and
switched to (IOP-3h), the program's `SifLoadModule("rom0:SIO2MAN")` goes
through `IOMAN`, `ROMDRV`, `LOADCORE` and `MODLOAD` and answers the
module's id, and `SIO2MAN` reaches IOP-6c's parked state. The simulators
cannot judge these — `tools/eesim.py` delivers no EE interrupt — so the two
emulators are the gate.

**The gate that matters is the M1 pull**, `docs/project-state.md` §6: an
independently-built program, run under the PS2SDK toolchain, calls
`SifInitRpc` and `SifLoadModule("rom0:SIO2MAN")` and prints its own final
line. It answers `16` — the module after the boot list's fifteen — on PS2e and
PCSX2, with the module parked as IOP-6c says. What the emulators do not show
is the exactness of the individual calls: PS2e's `--debug-iop` (single-step,
watchpoints) is the instrument for that when a later module disagrees.

No gate for IOP-1's Block-A/HDB path (IOP-1c) or IOP-2's priority-3 cause-0
handler (IOP-2d) is anticipated at all: neither affects anything the M1 or M2
pulls exercise, and both are recorded here as open rather than as something a
future tool is expected to close.
