# Specification: The IOP Kernel

Derived from `docs/analysis/37-iop-interrupts.md` (EXCEPMAN and INTRMAN),
`38-iop-threads.md` (THREADMAN), `39-iop-file-layer.md` (IOMAN, ROMDRV,
MODLOAD, LOADFILE), `40-sio2man.md` (SIO2MAN's start-up), `54-padman.md`
(the controller driver above it) and `55-mcman.md` (the card driver beside
it).

This covers the IOP-side kernel services above `SYSMEM`/`LOADCORE`
(`docs/spec/02-module-abi.md`): exception and interrupt delivery, the thread
scheduler, the file-driver framework and the ROM device it serves, the module
loader that answers `SifLoadModule`, and the device drivers analysed in enough
depth to specify -- the serial interface (`SIO2MAN`) and the controller driver
that speaks through it. `spec/02`
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

Ordinals 1, 2 and 26 are reserved stubs. Ordinals 10, 11, 21 and 22 are the
bare `syscall 4`/`8`/`0x10`/`0x14` trampolines; on `INTRMANP` they back
ordinals 8/9/17/18, and on the resident `INTRMANI` only ordinal 9 reaches one
of them (IOP-2k). Ordinals 12 and 13 are `INTRMANI`'s two `I_CTRL` helpers, a
raw read of `0xBF801078` and a write of `1` to it, called by ordinals 8 and 9
respectively; they are unnamed in `INTRMANP`'s table. Ordinal 27 sets the
fourth word of the internals record ordinal 3 returns, the mask gating the
bank-2 handler bracket IOP-2i describes.

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
position `16 + (irq - 0x20)`, i.e. bits 16–22), sets `DICR` **bit 23** (the
controller's own master enable, and the master for both banks) and sets
`I_MASK` bit 3 (`IOP_IRQ_DMA`, the umbrella cause) — all three are required
before a channel's own `DICR` flag can ever reach `I_STAT`. Bit 31 is a
separate flag, the OR of the enabled channels' own flags, and neither
variant's `EnableIntr` writes it. `DisableIntr` clears the channel's `DICR`
enable bit and reports through `*res` whether that channel's own flag bit was
also set.

Above `0x26` the two variants part. `INTRMANP`'s `EnableIntr` does nothing
and returns success (a silent no-op) for `irq >= 0x27` while its
`DisableIntr` returns `-0x65` — an intentional asymmetry, not a typo. The
resident `INTRMANI` carries both calls across the second bank instead
(IOP-2i), which leaves `irq == 0x27` in the gap between the two banks'
ranges: it satisfies neither range check and both calls reject it with
`-0x65`.

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

**IOP-2e2 — the context frame INTRMAN builds.** Read out of `INTRMANI` 1.01's
dispatcher and confirmed against `THREADMAN` 2.03's thread-start path, which
primes frames at these offsets. It is `0x98` bytes on the interrupted stack,
and it is **ABI, not a rebuild's choice**: a thread manager out of a title's
own image hands frames back at this layout.

| offset | contents |
| --- | --- |
| `+0x00` | the save-state tag, below — **not `$0`** |
| `+0x04`–`+0x7c` | `$1`–`$31`, in register order; `+0x74` (`$sp`) holds the pointer the frame was pushed from |
| `+0x80`, `+0x84` | `hi`, `lo` |
| `+0x88` | `Status`, as the exception left it (the pre-exception enable one level down, in `IEp`) |
| `+0x8c` | `EPC`; the syscall handler's own return advances it past the trap |
| `+0x90` | the `I_CTRL` (`0xBF801078`) gate the exception found |
| `+0x94` | unused; the allocation is `0x98` and the writes end at `+0x90` |

The tag at word 0 is how IOP-2e's deferred preservation is recorded, and the
restore path branches on it: `0xAC0000FE` — the mode-0 baseline, so only
`$1`–`$7`, `hi`, `lo`, `Status` and `EPC` are present; `0xFF00FFFE` — mode ≥ 1,
adding `$8`–`$15`, `$24`, `$25`, `$gp`, `$fp`; `0xFFFFFFFE` — mode ≥ 2, adding
`$16`–`$23`, hence a complete frame. Before the reschedule hooks (IOP-2j) are
consulted **the interrupt path promotes whatever frame it holds to
`0xFFFFFFFE`**, so a frame reaching a thread manager that way is a complete
one. The reschedule syscall (IOP-2k2) does not: it enters the hook call below
the promotion and hands its own frame over as it built it, tagged
`0xF0FF000C` — a voluntary switch saves no caller-saved register and no
`hi`/`lo`, and the restore skips those groups. `CpuInvokeInKmode`
(`syscall 0xc`) builds no frame at all: it calls `$a0` with `$a1`–`$a3` on the
exception's own stack and returns to `EPC + 4`.

**IOP-2k2 — the reschedule syscall's three arguments.** `syscall 0x20` is
every voluntary switch, and its handler installs three caller registers into
the frame it builds before calling `NewCtxCb`. **`ShouldPreemptCb` is skipped
and so is the tag promotion**, because the caller has already decided: the
handler's tail is a jump straight to the `NewCtxCb` call, past both. Read at
the addresses — `SetNewCtxCb` (ordinal 28) stores its callback at `+0x15a0`
and `SetShouldPreemptCb` (ordinal 30) at `+0x15a4`; the interrupt path calls
`+0x15a4` first and returns early if it answers `0`, then promotes, then calls
`+0x15a0` and takes the frame it returns; the syscall's `+0x1400` ends `j` to
that last call alone. The three arguments:

| register | where it goes | meaning |
| --- | --- | --- |
| `$a0` | the frame's `$v0` slot (`+0x08`) | what the blocked call answers when the thread is resumed |
| `$a1` | the frame's `$v1` slot (`+0x0c`) | its second return register |
| `$a2` | the interrupt state to resume under | the value `CpuSuspendIntr` (ordinal 17) reported |

Read from `INTRMANI` `+0x1400` and confirmed at the call site: `THREADMAN`
2.03's own trap wrapper (`+0x6640`) sets none of them, so they pass through
from its callers, and `DelayThread` (`+0x2cb4`) loads `$a0 = 0` and
`$a2` = the word ordinal 17 gave it — the **same word** its error path
(`+0x2c7c`) hands to `CpuResumeIntr`. `$a2` and `CpuResumeIntr`'s argument are
therefore one thing, and a rebuild that drops `$a2` resumes the thread under
whatever interrupt state the trap left, for ever.

Where `$a2` lands depends on which interrupt manager is resident, and `rom0`
ships two that both register `intrman` 1.02 (IOP-2k): `INTRMANI` stores it at
`+0x90`, the `I_CTRL` gate, matching its own ordinals 17/18; `INTRMANP`
(`+0x1090`) merges it into the frame's `Status` — `(Status & ~0x414) | $a2` —
matching its Status-based ones. **Each module is self-consistent and a client
cannot tell them apart**, because it only ever passes the value back. What a
rebuild must not do is mix them: ordinal 17, ordinal 18 and `$a2` are one
mechanism, and all three have to name the same state.

**The state's encoding is the saved frame's, not running code's.** `Status`
holds the enable three deep — `IEc`, `IEp`, `IEo` — and an exception pushes
that stack, so the caller's live `IEc` is `IEp` by the time a handler reads it.
The reference's ordinals 17 and 18 *are* the bodies of `syscall 0x10` and
`0x14`, so they read and write a frame's `Status` and the value they exchange
is `Status & 0x414` (`IEp`, `IEo`, `Im2`). A rebuild whose 17 and 18 are plain
functions reading the live `Status` sees the same three bits one level up, at
`0x405`, and **must still report them in the saved places** — because the value
does not stay in the interrupt manager: `$a2` carries it into a frame, where
`0x414` is what the bits mean.

`I_CTRL` reads as its value and closes itself on the read, so the entry stores
it in the frame and re-arms the gate to `1` at once; the **return installs the
resumed frame's copy**, not the interrupted one's, which is the only rule a
context switch can honour — a thread that blocked with the gate shut gets it
back, and a thread primed by `THREADMAN` 2.03 starts with the `1` that path
writes at `+0x90`.

**IOP-2c2 — `EnableIntr`/`DisableIntr` take a line *and flags*.** Both mask
their argument with `0xFF` before anything else (`INTRMANI` `+0x478` and
`+0x658`), so the line is the low byte and the upper bits are a separate
field. Only two are read, and only on the DMA paths: `0x100` also sets DICR's
own low bit for the channel, `0x200` DICR2's -- at the channel index in bank 1
and at index + 7 in bank 2. `SIFCMD` 2.08 enables SIF1 as **`0x22b`**, so an
implementation that compares the whole word against the line ranges matches
nothing and enables nothing, silently.

`DisableIntr`'s out-parameter is **not** a pending flag: the reference
reconstructs the argument that would re-enable the line -- the line plus
whichever of those two low bits it found set -- and leaves `-0x67` there when
it refuses. It answers `-0x67` for a line that was not enabled and `-0x65` for
one out of range; both write the parameter.

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

**IOP-2h1 — the force bit and its handler.** Bit 15 is read through the same
bank-1 shadow mask as the channel flags. When set, the handler clears it --
writing the register back with bit 15 zero and **zeroes in the acknowledge
bits**, so no channel flag is acknowledged along with it -- and then calls
`table[0x27]`, the one index between the two banks' ranges. That is why
`EnableIntr`/`DisableIntr` reject `irq == 0x27` (IOP-2c) while
`RegisterIntrHandler` accepts it: the slot names no channel and has no enable
bit, but the dispatcher reads it. A build with nothing registered there leaves
the slot null and the force bit still gets cleared.

**IOP-2h2 — the scan repeats until nothing is pending.** After serving every
flagged channel of both banks, the handler re-reads both registers and starts
again, and only leaves when the two banks' masked flags and the force bit are
all clear. A channel that re-asserts while a sibling's handler is running is
therefore served before this dispatch returns, rather than waiting for the
next hardware edge. Flags that the shadow mask gates out do not count towards
"pending", so a gated channel cannot hold the loop.

**IOP-2h3 — the master enable is pulsed on the way out.** Before returning,
the handler writes `DICR` with bit 23 clear, reads the register back until
that bit reads clear, and then writes it with bit 23 set again -- each write
with zeroes in the acknowledge bits. The wait is for the handler's own clear
to become visible and not for any flag to drain, so nothing a channel does can
extend it. The pulse is insurance: every acknowledge in the loop above writes
the whole register, and a master enable lost by one of them would silently
stop every later DMA interrupt with no other symptom.

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

The `0x1578` register is acquired and released again around each handler the
`irq`-3 dispatch calls, and that bracket is gated by the fourth word of the
internals record ordinal 3 returns — the word ordinal 27 sets. Which module
arms it is unread: nothing in `SIFMAN`, `SIFCMD` or `DMACMAN` imports ordinal
27 (`docs/analysis/37` §5).

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
the `Status` `IEp`/`IEo` stack.** On `INTRMANP` the public ordinals are thin
wrappers that trap via `syscall 4` (`CpuDisableIntr`), `syscall 8`
(`CpuEnableIntr`), `syscall 0x10` (`CpuSuspendIntr`) and `syscall 0x14`
(`CpuResumeIntr`); the resident `INTRMANI` traps from ordinal 9 alone, as the
end of this section says. In both variants the
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

**The resident `INTRMANI` reaches that handler from one ordinal only.** Its
ordinals 8, 17 and 18 call no trampoline at all: both 8 and 17 read `I_CTRL`
and return `-0x66` if what they read was already `0` — 8 through ordinal 12,
17 by a route `docs/analysis/37` does not name — with 17 also storing the
value through `*state`, and 18 writes back the value it is passed.
Neither 8 nor 17 writes `I_CTRL`, and neither touches `Status`. Only ordinal
9 traps, and it writes both gates — the same `syscall 8` above, then ordinal
13's `I_CTRL = 1`. So on this variant the state ordinal 17 hands to ordinal 18
is an `I_CTRL` word rather than a `Status & 0x414` one, which is the same
split IOP-2k2 records for `syscall 0x20`'s `$a2`. **Open:** how disabling
takes effect at all on `INTRMANI`, given that neither of its disabling
ordinals writes either gate — whether through some path `docs/analysis/37`
did not find, or by relying on the dispatcher's own save and force-disable at
return (`37` §5).

## IOP-3: Threads (THREADMAN)

Derived from `docs/analysis/38`.

**IOP-3a — ordinal tables.** `thbase`, `thevent` and `thsemap`. Seven
`thbase` exports are present in the table (so binding against them succeeds)
but are two-instruction stubs that unconditionally return `KE_ERROR`
(`-1`) [header]: `ExitDeleteThread`, `DisableDispatchThread`,
`EnableDispatchThread`, `SuspendThread`, `iSuspendThread`, `ResumeThread`,
`iResumeThread`. `thmsgbx` is IOP-3l. `thfpool`, `thvpool` and `thrdman` (a fourth,
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

**IOP-3l — message boxes (`thmsgbx`).** Derived from `docs/analysis/38`
§3.3. A fourth library out of the same file, needed the moment a title loads
its own SIF-RPC bridge. Thirteen ordinals: 4 `CreateMbx(iop_mbx_t*)`,
5 `DeleteMbx(id)`, 6 `SendMbx(id, msg)`, 7 `iSendMbx(id, msg)`,
8 **`ReceiveMbx(void **recvmsg, id)`** — the out-pointer is the *first*
argument, unlike every other id-taking entry in this file — 9
`PollMbx(void **recvmsg, id)`, 11 `ReferMbxStatus(id, info)`,
12 `iReferMbxStatus(id, info)`; 0–3 and 10 reserved. Every named ordinal has
a real body; there is nothing to stub.

`iop_mbx_t { attr, option }` [header], record `0x28` bytes, pool tag
`0x7F04`. `attr` must have `~0x5 == 0`, else `KE_ILLEGAL_ATTR`; bit 0
(`MBA_THPRI` [header]) orders **waiting threads** by current priority
instead of FIFO, bit 2 (`MBA_MSPRI` [header]) orders **queued messages** by
an unsigned byte the client puts at message `+4`. Ties go behind in both.

**The kernel must not copy a message.** A message is the client's own
memory, and the queue is a list threaded through the messages' own first
words, so the header a client provides is `{ void *next; uint8_t priority; }`
(`iop_message_t` [header]). `ReceiveMbx` hands back the sender's original
pointer. A rebuild that copies bytes into a buffer of its own breaks every
client that reads its payload back through the pointer it sent.

A box has either waiting threads or queued messages, never both. Send with a
waiter present takes the first waiter, readies it, and writes the message
through the out-pointer that thread parked when it blocked; send with none
queues the message. `ReceiveMbx` with a message present dequeues and returns
`0`; with none it blocks. The three ways a blocked `ReceiveMbx` ends are
`0` with `*recvmsg` written (a send), `KE_WAIT_DELETE` (`-425`) and
`KE_RELEASE_WAIT` (`-418`) — and in the last two **`*recvmsg` is left
alone**. `PollMbx` answers `KE_MBX_NOMSG` (`-424`) instead of blocking.
`KE_UNKNOWN_MBXID` is `-410`.

`iSendMbx` and `iReferMbxStatus` invert the context gate their siblings use:
they return `KE_ILLEGAL_CONTEXT` (`-100`) when called from *thread* context.
A rebuild whose `i`-forms merely skip the interrupt-suspend bracket, without
the inverted check, is permissible only if nothing depends on the rejection;
the reference rejects.

**IOP-3h — the dispatcher protocol.** Two globals decide everything: a
"current" thread pointer and a "pending-next" thread pointer.
`ShouldPreemptCb` (INTRMAN ordinal 30, IOP-2j) is exactly `pending != current`
— every operation above that decides a switch is warranted writes into
"pending-next" (or leaves it equal to "current"). `NewCtxCb` (INTRMAN
ordinal 28) resolves a mismatch: stores the outgoing thread's just-finished
saved-frame pointer into its own record (the only point that pointer is kept
current), falls back to a ready-queue pick if nothing was pre-selected, and
returns the target thread's own saved-frame pointer, which INTRMAN then loads
as the new stack pointer. The frame both hooks pass is IOP-2e2's, and a thread
manager primes a fresh one at those offsets: it is the interface between the
two modules, and they need not come from the same image. Every voluntary block/yield point funnels through
one dedicated reschedule syscall, whose three caller arguments are IOP-2k2's.
Its handler does **not** reproduce the interrupt-return tail: it neither tops
up the saved registers nor asks `ShouldPreemptCb`, and calls `NewCtxCb`
directly — a caller that traps deliberately has already made the decision the
predicate exists to make, and has no caller-saved register worth keeping.

**The syscall number is in `$v0`, and the `syscall` instruction carries no
code.** This is not a rebuild's choice: every caller in the reference loads
`$v0` and traps bare, so a handler that reads the instruction's 20-bit code
field sees `0` from all of them and returns having done nothing — silently,
because a `syscall` that changes no register looks exactly like one that
worked. `INTRMAN` ordinal 14 is `addiu $v0, $zero, 0xc` then `syscall`, and a
thread manager reschedules with `$v0 = 0x20`; both numbers are ABI, because a
thread manager and an interrupt manager from different images have to agree
on them. `0x20` reschedules and `0xc` is `CpuInvokeInKmode`. Two further invocation shapes exist: **involuntary preemption**
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

**One `fno` past the range check is not a no-op.** `fno 0xff` is a version
query, and a rebuild has to answer it: four ASCII digits naming the release
of the IOP kernel serving the call, into a four-byte reply, for a request
that carries no arguments at all. A title asks it once, immediately after it
binds `sid 0x80000006`, and compares the answer against the release its own
libraries were built against **before** it sends a single `fno 0`; on a
mismatch its `sceSifLoadModule` fails locally and no module is ever loaded
(`docs/analysis/39` §4). The failure is silent — the RPC completes, the
title simply stops asking — which is why the version query belongs in the
required surface and `fno` 2–5 do not. Every other out-of-range `fno` still
produces no reply.

The version query is absent from the `LOADFILE` in `rom0`, which is one
revision older; on retail hardware the title has replaced that module with
its own before it ever asks. A rebuild whose reboot does not perform the
`UDNL` merge (`docs/analysis/45`) keeps serving its own `LOADFILE` across the
reset, so it must carry the newer module's obligations.

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

**IOP-7f — the ordinals a title's driver needs past 16.** Derived from
`docs/analysis/49`. rom0's table stops at 16, but a title's own modules bind
against the `timrman` **v1.03** its `IOPRP` image carries, which has 28
entries. `SLPS-25918`'s `EZMIDI` imports four of them and checks every
return, so a table that stops at 16 makes its timer set-up fail silently —
IRX-9 binds a missing ordinal to `jr $ra` and `$v0` keeps whatever the caller
had. A rebuild that does not perform the `UDNL` merge must serve them itself:

| Ord | Signature | Answers |
| --- | --- | --- |
| 20 | `SetTimerHandler(id, compare, handler, arg)` | `0`, or `-151` for a bad id, `-154` for a running timer |
| 22 | `SetupHardTimer(id, source, mode, prescale)` | `0`, or `-100` from interrupt context, `-151`, `-154`, `-152` for a source the timer lacks, `-153` for a prescale it cannot reach, `-405` for `mode >= 8` |
| 23 | `StartHardTimer(id)` | `0`, or `-151`, `-154`, `-155` when 22 has not run |
| 24 | `StopHardTimer(id)` | `0`, or `-151`, `-156` when it is not running |

**IOP-7g — what the four do.** `SetTimerHandler` records `compare`, the
handler and its argument against the timer, and arms the compare interrupt —
MODE `0x58`, reset-on-compare with the interrupt and repeat — or disarms it
when `handler` is null. `SetupHardTimer` validates the source against the
timer's own mask and the prescale against its maximum, and is where the
library registers **its own** interrupt handler on that timer's IRQ, once per
timer. What it settles is MODE bits: `mode` itself in the low bits, with 2, 4
and 6 refused as `-405`; `0x100` for a PIXEL or HLINE source; and, on
SYSCLOCK, `0x200`/`0x2000` for a prescale of 8 on a 16-/32-bit timer,
`0x4000` for 16 and `0x6000` for 256, nothing for 1, and `-153` for anything
else. `StartHardTimer` writes MODE `0` first so the hardware is quiet, then
the compare — a halfword for RTC0–2 and a word for RTC3–5, per IOP-7d — then
those bits together with the handlers', which is the MODE that starts it. `StopHardTimer` is its inverse.

The interrupt handler is the library's, not the caller's: it reads the MODE
register once (which is also the acknowledgement, IOP-7d) and calls the
recorded handler when the compare flag `0x800` is set, and the separately
recorded overflow handler when `0x1000` is. A caller that registered only one
of the two leaves the other silent.

**IOP-7h — a timer id is opaque.** No caller builds or inspects one: it comes
from ordinal 4 and goes back unread. The `v1.03` library encodes it
differently from rom0's (`docs/analysis/49` §8), and a rebuild is free to keep
one encoding across its whole table rather than reproduce both.

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
S-command registers of IOP-8i:

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
| 26 | `sceCdReadNVM(address, u16 *data, u8 *status)` | IOP-8j |
| 27 | `sceCdWriteNVM(address, data, u8 *status)` | IOP-8j |
| 28 | `sceCdStatus()` | raw `0xBF40200A` |
| 31 | open the configuration session | IOP-8k |
| 32 | close it | IOP-8k |
| 33 | read `count` blocks | IOP-8k |
| 34 | write `count` blocks | IOP-8k |
| 39 | `sceCdBreak()` | writes `1` to `0xBF402007` |
| 46 | `sceCdNop()` | no-op |

**IOP-8h — what is out of scope for this minimal driver.** The S-commands
of IOP-8i to IOP-8k are **in** scope and were not when this section was
first written; what stays out is the rest of that group, the disc keys
among them. No CD streaming API (`sceCdSt*`, ordinals 56–61), and no
`CDVDFSV` RPC surface beyond what IOP-13 now
specifies — an EE client reaches only what a `CDVDFSV` build forwards, and
IOP-13d/13e say what that is for the configuration record. `sceCdRead`'s `mode`
argument (trycount/spindlectrl/datapattern) is accepted but not
interpreted: every read is issued as the plain 2048-byte, datapattern-0
case IOP-8d describes, matching what `LOADFILE`'s own ELF-loading path
needs and nothing beyond it.

**IOP-8i — the S-command register block and its sender.** Three byte-wide
registers, and one routine that drives all of them:

| Addr | Use |
| --- | --- |
| `0xBF402016` | write: the command number, which starts the command |
| `0xBF402017` | read: status — bit `0x80` busy, bit `0x40` **result FIFO empty**; write: one parameter byte, looped |
| `0xBF402018` | read: one result byte |

Only those two status bits are used. The sender takes a command number, a
parameter buffer with its length, and a result buffer with its length, and
runs a **fixed order** a rebuild has to keep:

1. Take the S-command semaphore with `PollSema`, not `WaitSema`. A `-419`
   (`KE_SEMA_ZERO`) is not a wait — the call is **declined**, the same
   shape IOP-8b records for the N-command channel.
2. Read status. **If `0x80` is set, release and decline**; a busy mechacon
   is never waited on.
3. Drain: while `0x40` is clear, read `0xBF402018` and discard. A previous
   command's unread bytes are the caller's problem otherwise.
4. Write the parameter bytes one at a time to `0xBF402017`.
5. Write the command number to `0xBF402016`.
6. Spin while `0x80` is set.
7. While `0x40` is clear, read result bytes from `0xBF402018` and count
   them; copy `min(counted, expected)` to the caller. **A short reply is
   truncated, not an error.**
8. Release the semaphore.

Steps 3 and 7 both terminate only on `0x40` becoming set, so an emulator
that never raises it hangs the driver at step 7 rather than at step 3.

**The sender's own answer says "sent", never "succeeded"**: non-zero when
the command went out, zero when step 1 or step 2 declined. What the
mechacon thought of it is in the result bytes, and every ordinal below
reports that separately.

**IOP-8j — NVM is word-addressed and big-endian on the wire.** Ordinal 26
is S-command `0x0A` with two parameter bytes and three result bytes;
ordinal 27 is `0x0B` with four and one.

The `address` argument is a **16-bit word index, not a byte offset**, and
one call moves exactly one halfword. Both put the address out
most-significant byte first, and the data with it:

| | parameters | results |
| --- | --- | --- |
| read | `addr >> 8`, `addr & 0xFF` | `status`, `data >> 8`, `data & 0xFF` |
| write | `addr >> 8`, `addr & 0xFF`, `data >> 8`, `data & 0xFF` | `status` |

The status byte goes to the caller's `u8 *` in both. What the ordinals
themselves return was not read (`docs/analysis/26`), so a rebuild picks a
convention and states it rather than claiming one.

**IOP-8k — the configuration record is a session of 15-byte blocks with a
sum byte.** Four ordinals, each one S-command:

| Ord | S-cmd | Parameters | Results |
| --- | --- | --- | --- |
| 31 open | `0x40` | 3 bytes | 1 |
| 32 close | `0x43` | none | 1 |
| 33 read | `0x41`, once per block | none | 16 |
| 34 write | `0x42`, once per block | 16 bytes | 1 |

Open takes `(a, b, count, u32 *status)` and sends **`[b, a, count]`** — the
first two arguments are reversed on the wire, which is the kind of thing a
rebuild gets wrong silently. It zeroes `*status` before sending, and
`count` is the session's only state. It also delays before sending
(`DelayThread(16000)`); `26` records the delay without a reason for it, so
a rebuild keeps it and says the same.

Close sends its command and clears the stored count. **A read or write
outside a session is not an error**: it completes zero blocks.

Read and write loop `count` times, advancing the caller's buffer **15**
bytes per block, stop at the first failure, and answer with the number of
blocks completed — so zero means no session and fewer than `count` means a
fault partway.

**The wire block is 16 bytes and the caller's is 15.** Byte 15 is the sum
of bytes 0 to 14, modulo 256. Read verifies it and strips it; write
computes and appends it. The status word the two report is **not the same
kind of thing**: on read it is the local checksum verdict (0 match, 1
mismatch), on write it is the device's own reply byte.

**`CDVDMAN` never interprets the fifteen bytes.** No field decoding, no
version check, no "is this console configured" test — those live in the
OSD, which is why `docs/analysis/26` could establish the transport without
settling any field.

The OSD's own record is **two blocks, thirty bytes**, opened as
`open(1, 0, 2)` — `[0, 1, 2]` on the wire (`docs/analysis/27`). The OSD
retries that open while the returned status has either of bits `0x01` and
`0x80` set, so a successful open must leave both clear.

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

## IOP-10: Vertical blank (VBLANK)

Derived from `docs/analysis/47`.

**IOP-10a — what the module is.** `VBLANK` owns the IOP's two vertical-blank
interrupt lines — `I_STAT`/`I_MASK` bit **0** for the start of vertical blank
and bit **11** for its end — and multiplexes each into a priority-ordered list
of callbacks, plus one event flag. It exports one library, `vblank` v1.01, of
ten ordinals: 0 the entry, 1 and 2 reserved, 3 the `.bss` base, 4–7 the four
waits, 8 `RegisterVblankHandler`, 9 `ReleaseVblankHandler` [header]. It
creates no thread. On the boot list it sits after `THREADMAN`, whose event
flags and system status flag it uses.

**IOP-10b — the two lists.** `RegisterVblankHandler(startend, priority,
handler, arg)` puts a callback on the start list when `startend` is 0 and on
the end list for **any** other value. Insertion is by ascending priority with
ties behind, so a lower number runs first. The duplicate test is on **the
handler pointer alone**, per list — `arg` and `priority` play no part — and a
match answers `KE_FOUND_HANDLER` (`-104`). Both lists draw from one pool of 16
records, of which the module spends two on itself, and exhaustion answers
`KE_NO_MEMORY` (`-400`). Both ordinals refuse interrupt context with
`KE_ILLEGAL_CONTEXT` (`-100`) before touching anything.
`ReleaseVblankHandler(startend, handler)` answers `KE_NOTFOUND_HANDLER`
(`-105`) for a handler that is not on that list, and **exactly 0** otherwise —
a real client retries this call for ever on any other answer.

**IOP-10c — the dispatch.** Each line's handler walks its list, taking a
record's successor **before** calling it, and calls `handler(arg)`. A callback
answering **0 is unregistered on the spot** and its record returned to the
pool; nonzero keeps it. The line handlers themselves always answer nonzero, so
`INTRMAN` re-enables the line after every dispatch (IOP-2) and both stay armed
for good.

**Callbacks run in interrupt context**, on the interrupt stack, so
`QueryIntrContext()` is nonzero inside them. That is not incidental: real
callbacks wake their threads with the `i` forms of the event-flag calls, which
answer `-100` from thread context. A rebuild that delivered these from a
thread would break its clients with no error surfaced anywhere.

**IOP-10d — the event flag and the four waits.** One `EA_MULTI` flag, initial
bits 0, four bits, all of them set and cleared by the module's own two
callbacks — which it registers through its own ordinal 8 at priority `0x80`,
behind any client. Bit `0x1` is a start **pulse**, `0x4` an end pulse, `0x2`
the level "inside vertical blank" and `0x8` the level "outside" it. Each
callback sets its pulse and its level, then clears its own pulse and the other
callback's level. So `WaitVblankStart`/`WaitVblankEnd` (ordinals 4 and 5)
always block until the next edge, while `WaitVblank`/`WaitNonVblank` (6 and 7)
return at once when the level already holds. All four are
`WaitEventFlag(id, bits, WEF_OR, NULL)`, so **a null result pointer must be
accepted**.

The start dispatch also raises bit `0x200` in `THREADMAN`'s system status flag,
once, on the first vertical blank after boot. Nothing in the reference archive
waits on it; a rebuild raises it for fidelity.

## IOP-11: The card-authentication interface (SECRMAN)

Derived from `docs/analysis/48`. `docs/clean-room-policy.md` puts the
authentication *mechanism* out of scope and only its interface in, so this
section specifies an interface and deliberately specifies no exchange.

**IOP-11a — what has to exist.** A library tagged `secrman`, version 1.03,
whose ordinals reach at least 6 — the reference's fourteen entries keep every
later ordinal where a client expects it. A memory-card driver imports 4, 5 and
6 and nothing else; an importer of a tag no one exports is refused at link
time, which is why the library must exist even where the mechanism does not.

**IOP-11b — the two handler slots.** Ordinal 4
`SecrSetMcCommandHandler(handler)` and ordinal 5
`SecrSetMcDevIDHandler(handler)` each store a pointer and answer nothing in
particular. **NULL is a real argument** — a driver passes it on unload — and
neither validates. The command handler is the transport: it is what the
library would call to reach the card, as `handler(port, slot, descriptor)`,
which is why this library names no `sio2man` import of its own.

**IOP-11c — ordinal 6, `SecrAuthCard(port, slot, cnum)`.** **Its answer is
`0` or `1`, and nothing else.** `0` when the command handler slot is null, and
on every failure of the exchange; `1` when the exchange succeeded. A client
branches on zero versus nonzero and reports a failure upward as
`sceMcResFailAuth` (`-90`) [header] — it does not hang, and it does not retry.
So both answers are safe, and a rebuild that does not perform the exchange
must still choose deliberately: **answer `1` where a card driver should
proceed, and `0` where the handler was never registered**, so a driver that
skipped registration is not told a success it never set up.

**IOP-11d — the entry.** Register the library; a failure there ends the entry
non-resident with nothing else done. Clear both handler slots. The reference
also hands `modload` ordinal 12 the three callbacks of its encrypted-module
path and **ignores what that call answers**; a rebuild without that path may
omit the call.

**IOP-3 through IOP-6** are exercised by the M1 program (`docs/project-state.md`
§6) on the two targets: `LOADFILE`'s thread is woken from an interrupt and
switched to (IOP-3h), the program's `SifLoadModule("rom0:SIO2MAN")` goes
through `IOMAN`, `ROMDRV`, `LOADCORE` and `MODLOAD` and answers the
module's id, and `SIO2MAN` reaches IOP-6c's parked state. The simulators
cannot judge these — `tools/eesim.py` delivers no EE interrupt — so the two
emulators are the gate.

**IOP-13's configuration session has no simulator gate either**, for the same
reason and one more: `tools/fsvcheck.py` judges the IOP half against a
modelled mechacon, but the EE half -- a program binding `0x80000593` and
decoding IOP-13g's record -- needs a machine with both processors and an
NVRAM. The two emulators are it, and they answer with different records,
which is what covers IOP-13g1's gate in both directions.

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

## IOP-12: The heap (HEAPLIB)

Derived from `docs/analysis/50`. `SYSMEM` hands out 0x100-granular blocks and
takes back only the last one at each end (IRX-15), so nothing built directly
on it can allocate and free in any order. This library is what supplies that,
and the thread manager a title's `IOPRP` image carries needs it twice over:
its alarm records and every variable pool it creates are heaps.

**IOP-12a — what has to exist.** A library tagged `heaplib`, version 1.01,
whose table reaches at least ordinal 8; the reference's eighteen entries keep
every later ordinal where a client expects it. The module registers its table
through `loadcore` ordinal 6 and returns that result unchanged, and holds no
state of its own: a heap lives entirely in the `SYSMEM` memory the caller was
given a handle to.

**IOP-12b — the two magics.** A heap's first word is `heap + 1`; an arena's is
`arena - 1`. They are self-relative so that a pointer given to the wrong entry
point fails on the arithmetic rather than on a constant that could occur by
chance, and so that a heap and its own first arena — which are 0x10 bytes
apart — cannot be confused. Every entry point tests its argument's magic
before touching anything else.

**IOP-12c — the heap object.** `+0x0` magic, `+0x4` the growth size in the
upper bits with the `SYSMEM` mode in bit 0, `+0x8` and `+0xc` the
`{next, previous}` sentinel of the grown-chunk list, `+0x10` the first arena.
A grown chunk is that node followed by its arena at the chunk's `+0x8`, so
every walk reaches an arena as `node + 8`. Packing the growth size and the
mode into one word makes "may this heap grow" and "how big is a new chunk"
the same test: a non-growable heap reads as a growth size of zero. **New
chunks are inserted directly after the sentinel**, so allocation tries the
newest chunk first and the original one last.

**IOP-12d — the arena.** `+0x0` magic, `+0x4` its size in bytes as given,
`+0x8` the units currently allocated, `+0xc` the rover, `+0x10` the first
block header. The unit is 8 bytes; a block header is one unit,
`{next, size_in_units}`, with `size` counting the header, and the user pointer
is the header plus 8 — so user memory is 8-byte aligned, not 16. **While a
block is allocated its `next` word holds the arena's own address**, and that
is what identifies a pointer's owner; while it is free the word links an
address-ordered circular free list.

**IOP-12e — preparing an arena.** `units = (size - 0x10) >> 3`. The last unit
is an end sentinel of `size` zero, the first block takes `units - 1`, the free
list is the circular pair of the two, the rover starts at the first block. A
`size` below 0x29 leaves the memory untouched and reports nothing, so the
magic never appears and every later call on that arena fails — which is the
behaviour, not an oversight. The sentinel is what makes the search terminate:
`size` zero satisfies no request.

**IOP-12f — allocation is next-fit.** A request below 8 is raised to 8, so a
zero-size request succeeds with 8 usable bytes;
`units = ((n + 7) >> 3) + 1`. The search begins after the rover and takes the
first block that fits; an exact fit is unlinked, a larger one is **split from
its tail**, which leaves the free list's order untouched. The rover then names
the predecessor of what was taken. One full circuit without a fit answers 0.
When no chunk can serve the request and the heap may grow, a new chunk is
taken from `SYSMEM` and the request retried against it.

**IOP-12g — freeing.** The block's bracketing pair in the address-ordered free
list is found, and the free is **refused with -3** if the block starts on a
free header, runs into the following free block, or lies inside the preceding
one. Otherwise the block is coalesced forward unless the next block is the
sentinel, coalesced backward when the preceding free block ends exactly at it,
and the rover is left on the predecessor. A grown chunk whose last block has
just been freed is unlinked and returned to `SYSMEM`.

**IOP-12h — the free size is title-visible.** `HeapChunkSize` answers
`(((size - 0x10) >> 3) - used - 1) << 3` and validates nothing.
`HeapTotalFreeSize` is that summed over every chunk. The thread manager stores
the latter as a variable pool's capacity at creation and reports it from
`ReferVplStatus`, so for a 0x800-byte pool the answer has to be `0x7d8`. This
is the one number in the library that a title can read.

**IOP-12i — the two sizings differ, deliberately.** `CreateHeap` sizes its
first arena from the **request**, leaving up to 0xFF bytes of the block
`SYSMEM` rounded up unused. The growth path sizes a new chunk's arena from
**`SYSMEM` ordinal 10**, recovering that slack. So ordinal 10 must answer
correctly for a block just allocated: an unimplemented `-1` there is not a
failed allocation but an arena of 0xFFFFFFF7 bytes whose end sentinel lands
below its own memory, and the corruption surfaces far from its cause. The new
chunk is at least `n + 0x28` — the node, the arena header, one block header
and the sentinel.

**IOP-12j — failure.** Allocation failure is 0 everywhere. A bad heap is -4, a
pointer no chunk owns is -1, and a double free of a block that has not been
reissued is also -1, because the header's `next` word is a free-list link by
then rather than the arena tag. A double free of a block that has been
reissued succeeds and frees the new owner's memory; nothing can detect it.
`DeleteHeap` frees every chunk whether or not anything is live in it. Nothing
in the library disables interrupts, so serialising a heap is the caller's
business.


## IOP-13: The EE-facing CDVD service (CDVDFSV)

Derived from `docs/analysis/42` §5 and `docs/analysis/27`. `CDVDMAN` is an
IOP-side library; nothing on the EE can call it. `CDVDFSV` is what an EE
client actually reaches, and it is a thin one: almost every entry point takes
a request buffer apart, calls one `CDVDMAN` ordinal, and puts the answer in a
fixed reply area. This section says which entry points must exist and what
travels in each direction. It supersedes IOP-8h's note that the forwarding for
the configuration record "has no requirement here yet".

**IOP-13a — the services and their threads.** Five RPC services over two
threads, both `TH_C`, priority `0x51`, `0x1800`-byte stacks, one RPC queue
each. Thread A serves `0x80000592`, `0x8000059A` and `0x80000593`; thread B
serves `0x80000597` and `0x80000595`. No service installs a client callback.
`0x80000594`, `0x80000596`, `0x80000598`, `0x80000599`, `0x8000059B` and
`0x8000059C` are **not** registered, and a client that binds one of them is
answered with nothing -- which is what the reference does and what a title
relies on to detect a generation older than its own.

**IOP-13b — `0x80000593` is a numbered table, 1 to 25.** The bound is
`(fno - 1) <u 25`, so `fno` 0 wraps and is rejected with everything above 25.
A rejected `fno` is still **acknowledged**: the client gets the same fixed
reply area every served `fno` uses. Dropping the request instead stalls a
client that is waiting on the reply, so **the acknowledgement is required and
the reply's contents are not** -- the reference leaves whatever the last call
put there, and clearing it is equally correct. What a rebuild may not do is
answer nothing.

**IOP-13c — one reply area, one shape.** Every `fno` of `0x80000593` answers
through one fixed area rather than a per-`fno` buffer, and the entries that
forward a `CDVDMAN` ordinal taking a `status` out-pointer all use the same
layout:

| Offset | What |
| --- | --- |
| `+0x0` | what the `CDVDMAN` ordinal returned |
| `+0x4` | the `status` word that ordinal filled in |
| `+0x8` | payload, where the `fno` has one |

The area must hold `8 + 15 * count` bytes for the configuration read of
IOP-13e, which is the largest single reply this service produces.

**IOP-13d — the configuration session's four entries.** `fno` 14, 15, 16 and
17 forward `CDVDMAN` ordinals 31, 32, 33 and 34 (IOP-8k), one each. **`fno` 17
forwards ordinal 34 and nothing else**; it does not also read a disc key.

**IOP-13e — what each of the four carries.**

| `fno` | Request | Reply |
| --- | --- | --- |
| 14, open | one word: **byte 0** the ordinal's second argument, **byte 1** its first, **byte 2** the block count | `+0x0` return, `+0x4` status |
| 15, close | not read | `+0x0` return, `+0x4` status |
| 16, read | not read | `+0x0` blocks completed, `+0x4` status, `+0x8` the blocks, 15 bytes each |
| 17, write | the block data, from `+0x0` | `+0x0` blocks completed, `+0x4` status |

The byte order in 14's word is the one IOP-8k's wire already has one layer
down, so a rebuild that unpacks it in the obvious order sends the arguments
reversed and the session opens on the wrong record. The OSD's own call is
`open(1, 0, 2)`, which is the word `0x00020100`.

Three numbers meet here and have to agree: IOP-8k hands 15 bytes per block up
from `CDVDMAN`, the OSD asks for two blocks, and the 30 bytes it reads are
those two at `+0x8`.

**IOP-13e1 — the NVM pair answers in a different shape.** `fno` 8 and 9
forward `CDVDMAN` ordinals 26 and 27 (IOP-8j), and unlike IOP-13e's four they
hand those ordinals out-pointers **into the request buffer**, then copy the
request's first two words into the reply behind the return:

| | Request | Reply |
| --- | --- | --- |
| 8, read NVM | `+0x0` address; `+0x4` receives the data halfword, `+0x6` the status byte | `+0x0` return, `+0x4` the address word echoed, `+0x8` the word holding data and status |
| 9, write NVM | `+0x0` address, `+0x4` the data halfword; `+0x6` receives the status byte | the same three |

So the client reads the data at `reply + 0x8` low half and the status at its
bit 16. A rebuild cannot serve these two and IOP-13e's four through one
helper, and one that answers the `+0x4`-status shape here returns the address
where the caller expects the data.

**IOP-13g — what the session carries.** Derived from `docs/analysis/27` and
`docs/analysis/28`. The record is **two blocks of fifteen bytes**, and only
block 1 means anything: block 0 reaches the caller untouched and no part of
this chain inspects it. Block 1's bytes map to fields like this, and the map
is not byte-aligned -- two fields straddle byte boundaries and one nibble is
stored back to front:

| Byte | Bits | Field |
| --- | --- | --- |
| `+0` | 7-5 | the generation gate, below |
| `+0` | 4 | the older generation's language: 0 Japanese, 1 English |
| `+0` | 3 | passed to EE syscall `0x4f` |
| `+1` | 4-0 | the newer generation's language index |
| `+2` | 2-0 | the timezone's high three bits |
| `+2` | 3 | add one hour to the timezone |
| `+2` | 4 | 0 a 24-hour clock, 1 a 12-hour clock |
| `+2` | 7 | **the configured flag** |
| `+3` | 7-0 | the timezone's low eight bits |
| `+5`, `+4` bit 0 | | a nine-bit field whose meaning is not settled |

The timezone is an offset in **minutes**, eleven bits, `+3` in the low eight
and `+2`'s low three above them.

**IOP-13g1 — the generation gate, which is the field to get right first.**
When byte `+0`'s **top three bits are zero**, byte `+1` is not read at all and
the language is the single bit `+0` bit 4 -- Japanese or English. When they are
non-zero, the five-bit index in `+1` is used instead. A record that leaves them
zero has a carefully chosen language byte ignored; one that sets them has the
index believed, and **the lookup is unchecked**: an index past the eighth
language reads past the table, and an index of 9 selects a null string table,
so the program draws nothing rather than failing. The eight are, in order,
Japanese, English, French, Spanish, German, Italian, Dutch and Portuguese.

**IOP-13g2 — the configured flag is not a struct field.** Byte `+2` bit 7
reaches none of the fields above. The reference's decoder returns it
**inverted** as its own result, and the OSD runs its first-boot routine when
the bit is clear. A rebuild that synthesises a record and wants the browser
rather than setup sets it.

**IOP-13f — the other entry points a boot needs.** `0x80000592` reads the
request's first word as `sceCdInit`'s mode and answers that ordinal's return.
`0x8000059A` reads a mode word and answers `2` or `6` for ready or not,
calling no `CDVDMAN` ordinal at all. `0x80000597` searches for a file: the
request holds a 0x20-byte result area at `+0x0`, a NUL-terminated path at
`+0x20` and the EE address to deliver the result to at `+0x120`, and the
service DMAs the 0x20 bytes there itself before answering the return code.
A newer generation of this module switches that layout on the request size --
`0x12c` or `0x128` moves the path to `+0x24` and the address to `+0x124` --
and a rebuild serving only the older shape answers an empty path.

## IOP-14: The controller (PADMAN)

Derived from `docs/analysis/54`, with `docs/analysis/40` for the serial
interface underneath and `docs/analysis/47` for the vertical-blank callback.
This is the first driver in the archive whose whole purpose is to move data
*to* the EE without being asked: an EE client opens a port once and then reads
a record that arrives in its own memory every frame. Nothing about that is
visible in an RPC trace, which is why it is written out here.

The scope is one controller on **port 0, slot 0**, answering with digital
buttons. Vibration, pressure-sensitive mode, analogue sticks, multitaps and
the second port are out of scope; where the record carries their bytes it
says so, and a rebuild leaves them zero.

**IOP-14a — one service, and it is not the one a title binds.** The module
registers SIF RPC service **`0x8000010f`**. The ids `0x80000100` and
`0x80000101`, which `docs/analysis/43` records a title binding, belong to a
later generation of this driver; in this one those values are **function
codes carried inside the request**, a different namespace entirely. A rebuild
that registers `0x80000100` here is answering a client this archive has none
of, and leaving `0x8000010f` unserved. The reference also registers a second
id and then never dispatches it -- its loop polls the first queue -- so a
rebuild registering only `0x8000010f` is not missing anything a client can
reach.

**IOP-14b — the function code comes from the request, not from `fno`.** The
dispatcher ignores the `fno` the RPC layer hands it and reads request word 0.
Codes run `0x80000100` to `0x8000010e`; anything else is answered with the
buffer unchanged. **The reply is the request buffer itself**, returned as the
dispatcher's answer and sent back sized by the client's own receive size, so
a rebuild must write its results into the request and return that pointer
rather than composing a separate reply. The buffer is `0x80` bytes and the
service is registered without a size, so a client sending more overruns it;
a rebuild must not rely on the client's restraint for anything it writes.

**IOP-14c — the four entry points a digital read needs.**

| Code | Request words in | Answer | Meaning |
|---|---|---|---|
| `0x80000100` | `[1]` port, `[2]` slot, `[4]` EE address | `[3]` 1 or 0 | open |
| `0x8000010b` | — | `[3]` = 2 | how many ports |
| `0x8000010c` | `[1]` port | `[3]` = 1 | how many slots on it |
| `0x8000010d` | `[1]` port, `[2]` slot | `[3]` 1 or 0 | close |

Word 0 is never rewritten, so the client's own code survives in the reply.
Opening a port that is already open answers 0. Opening rejects a port index
outside `0..1` and nothing else: **the EE address in word 4 is taken as
given, with no check that it is non-zero or aligned**, and a zero there makes
the first push close the port again (IOP-14g). A rebuild may check it; what it
may not do is push to it.

**IOP-14d — one serial batch and one push per vertical blank.** The cycle,
which a rebuild must reach whatever its internal shape:

1. Wake on the start-of-blank callback. The callback only signals once a port
   has been opened; before that the driver is idle.
2. Read `sio2man` ordinal 11 once, and keep its bits 4 and 5 -- one per port.
   Their meaning is not known (`docs/analysis/54` §5); they select a second
   set of register words and a one-byte shift of the reply. **A rebuild takes
   the bit-clear path** and a machine that raises them is out of contract.
3. Build at most one command per open port, then run **one** serial batch for
   all of them: `sio2man` ordinal 23, then ordinal 25. The batch's descriptor
   packs each port's bytes back to back in one block, and its `regdata` slot
   `n` carries the port index in bits 0 and 1.
4. Judge each port from the status word the transfer brought back: the port
   failed if bit 13 is set, or if bit `16 + n` is set for its index `n` in the
   batch. Otherwise its reply bytes are its own share of the output.
5. For a port whose open mask is exactly slot 0, push IOP-14g's record.

**IOP-14e — the frames on the wire, for a digital pad.** Lengths follow the
controller's ID byte: with `id` in hand the frame is `((id & 0xf) << 1) + 3`
bytes each way, which makes a digital pad's `0x41` a five-byte frame and the
configuration mode's `0xf3` a nine-byte one. The steady poll is:

```
transmit  01 42 00 00 00
receive   ff 41 5a <low> <high>
```

with `port_ctrl1 = 0xffc00505`, `port_ctrl2 = 0x00020014` and
`regdata = 0x00140540` (`(tx << 8) | 0x40 | (rx << 18)`, the port index filled
in by the batch). **These three words are carried exactly**, not reconstructed
from what an emulator happens to need: what their fields mean has not been
read, and a model that ignores them proves nothing about hardware.

The **first** ID probe, sent before any ID is known, carries
`port_ctrl2 = 0x0002000a` instead; every frame after it, the re-probe that
follows the configuration mode included, carries `0x00020014`. What the
difference is for has not been read, and it is reproduced rather than
explained.

A reply is good only when the transfer did not fail, `receive[2]` is `0x5a`
and `receive[1]` is the ID last agreed. Otherwise the driver returns to
discovery. Ten consecutive transfer failures also return it to discovery.

**The two button bytes are the wire's own, and their bits are active low** --
a bit is 0 while its button is held. The driver does not invert them and
neither does the record; the EE client does. Bit order in the halfword, low
byte first: select, L3, R3, start, up, right, down, left, L2, R2, L1, R1,
triangle, circle, cross, square.

**IOP-14f — discovery is all of it or none of it.** Before steady polling the
reference probes the controller, and the sequence is not divisible:

```
01 42 00 00 00                      until the ID byte is non-zero and known
01 43 00 01 00                      enter the configuration mode
01 45 00 5a 5a 5a 5a 5a 5a          model and table sizes
01 46 00 00 5a 5a 5a 5a 5a          } tables this scope does not use,
01 47 00 00 5a 5a 5a 5a 5a          } but whose frames must still be sent
01 4c 00 00 5a 5a 5a 5a 5a          }
01 41 00 5a 5a 5a 5a 5a 5a          the button mask, only if the model allows
01 43 00 00 5a 5a 5a 5a 5a          leave the configuration mode
01 42 00 00 00                      re-read the ID, then poll for ever
```

A controller in the configuration mode answers with ID `0xf3`, so every frame
between the two `0x43`s is a nine-byte one and the `0x45` frame is rejected
unless the ID reads `0xf3`. **A rebuild that sends the first `0x43` and then
stops never leaves discovery**, because the controller is still in the mode
and its ID no longer matches. Two endings are correct: send the whole
sequence, or send none of it and poll the pad as found. The first gives the
record's slot-state byte the value 6 and fills its model byte; the second
gives 2 and leaves the model byte 0. Ten failures of the `0x43` frame
themselves take the second ending, which is how a controller that has no
configuration mode is handled.

**IOP-14g — the record pushed to the EE, `0x40` bytes.** Built in IOP memory
and sent with `sifman` ordinal 7 under a suspend/resume bracket, from thread
context. **It alternates between two halves of a `0x80`-byte area** at the
address the open carried: an even frame counter lands at `+0`, an odd one at
`+0x40`. The client owns both halves and picks the one whose counter is
larger.

| Offset | Size | Contents |
|---|---|---|
| `+0x00` | 4 | frame counter, incremented after the record is built |
| `+0x04` | 1 | slot state: 0 nothing answered, 2 stable and not configurable, 5 handshake in progress, 6 stable and configured, 7 the last poll failed |
| `+0x05` | 1 | request state: 0 idle, 1 the last request was refused, 2 one is in flight |
| `+0x06` | 2 | 1 when this frame's reply was good, 0 otherwise |
| `+0x08` | 1 | 0 when the block below is valid, `0xff` when it is not |
| `+0x09` | 1 | the controller's ID byte, `0x41` for a digital pad |
| `+0x0a` | 2 | **the button halfword, exactly as the wire sent it, active low** |
| `+0x0c` | 4 | the stick bytes when the controller is in a mode that sends them; zero for a digital pad |
| `+0x10` | 0x17 | the rest of the reply, zero for a five-byte frame |
| `+0x27` | 1 | the `0x5a` byte from the reply |
| `+0x28` | 4 | `0x20` when the block from `+0x08` is valid, 0 when it is not |
| `+0x2c` | 1 | port state |
| `+0x2d` | 1 | 1 the controller has no configuration mode, 2 it entered one |
| `+0x2e` | 1 | the model byte from the `0x45` frame |
| `+0x2f` | 1 | the status bit sampled in IOP-14d step 2 |
| `+0x30` | 1 | consecutive transfer failures |
| `+0x31` | 0xf | not written; a rebuild leaves it zero |

There is **no single "a controller is connected" field**, and a client that
invents one from any single byte is wrong in one direction or the other. A
digital pad is present and readable this frame exactly when `+0x06` is 1 and
`+0x09` is `0x41`.

The record is also pushed immediately, outside the vertical blank, whenever a
request is accepted, so that a client sees its request go in flight without
waiting a frame.

**IOP-14h — what this section does not pin.** The reference reaches the above
with four module-wide threads and six more per open port, coordinated through
two event flags. **None of that is required.** A rebuild that produces the
same frames in the same order, the same record with the same contents, and
the same per-blank cadence satisfies IOP-14 with as few threads as it likes.
What a client and a serial controller can observe is the contract; the
topology behind it is not, and reproducing it because `docs/analysis/54`
describes it would be copying structure for its own sake.

### Verification

`IOP-14` has **no simulator gate**, for the reason IOP-6 and IOP-13 have
none and one more: `tools/iopsim.py` models no serial interface at all, so
nothing offline can carry a frame. A simulator gate needs a serial-interface
device model and a controller behind it, which is worth building and is not
built.

The two emulators are the gate. A controller can be held down over a cycle
range headless, so the check is: bring the driver up after the boot is
otherwise finished, print the buttons the EE decodes on the frame the press
begins, and confirm the console names exactly the buttons that were held and
nothing else. The press must fall **after** the screen is up; a press during
the boot list is over before any client has opened a port.

## IOP-15: The memory card (MCMAN, MCSERV)

Derived from `docs/analysis/55`, with `docs/analysis/40` for the serial
interface underneath, `48` for the authentication interface, and `55` §0.1 for
the card's own on-image layout, which was read out of real cards rather than
out of any binary.

The scope is **one question asked from the EE: is a formatted card in the
slot, and what is in its root directory?** Writing, creating, deleting,
formatting, the allocation path, PS1-format cards and the error-correction
algorithm are out of scope; where the contract below touches them it says
what a rebuild must leave alone rather than what it must do.

**IOP-15a — two modules, and the split between them.** The driver owns the
card: the serial frames, the authentication, the page reads, the filesystem.
The service is a thin thread over it that answers one SIF RPC id and calls
the driver's exports. Nothing on the EE can reach the driver directly, and
the driver installs no RPC of its own.

**IOP-15b — the driver's port state, and what "which port" means.** A port's
state is one record per slot, and the **serial port index is the caller's
port number's low bit plus two**: the controllers occupy 0 and 1, the cards 2
and 3. Every entry point takes the caller's numbering and converts; nothing
outside the module sees the serial index. The record holds a copy of the
card's superblock page, the card type (none, PS1, PS2, PS1-pocket), a
geometry flag byte, the derived cluster arithmetic, and a **mounted** word
that is the memo the detect ladder short-circuits on.

**IOP-15c — the detect ladder, and every code it can answer.** From nothing
known to mounted, in this order. Each numbered step is one serial batch
unless it says otherwise.

1. **Probe** (`0x11`, four bytes each way), up to five times. A reply byte of
   `0x66` at offset 3, or a failed transfer, is a miss. **Any other value is
   a hit and skips to step 5** -- that short-circuit is what makes a second
   call on the same card cost two batches instead of nine.
2. **Reset** (`0xf3`, five bytes), once. A failed transfer answers **-11**
   and stops. The reply is not examined.
3. **Authenticate**: the card-authentication interface's third entry point,
   with the serial port index, the slot, and a card number that is the port's
   low bit shifted left three. **Zero answers -90 and stops.**
4. **Presence** (`0x28`, five bytes), up to five times; a hit is a successful
   transfer whose reply byte 4 is not `0x66`. Five misses answer **-12**.
5. If reply byte 3 is the terminator value the driver sets in step 6, this is
   a card it has already seen: a mounted record answers **0** and stops, a
   record marked invalid answers **-2**, and an unmounted one falls through
   (which is the unformatted card's path -- it is known, and still not
   mounted).
6. **Terminator** (`0x27` carrying `0x5a`, five bytes), up to five times; a
   hit is a successful transfer whose reply byte 4 is `0x5a`. Five misses
   answer **-13**.
7. **Mount** (IOP-15e). Success answers **-1**, not 0: *the card changed*.
   Its own failures pass through.

**A rebuild must reproduce the masking, not just the ladder.** The entry point
callers use answers this ladder's code only when it is **-1 or above**;
anything below is discarded and replaced by the answer of a separate probe for
the older card format, with the card type set to none. So `-11`, `-12`, `-13`,
`-90` and the mount's own failures never leave the module. A caller sees, in
practice: `0` (the same card, still mounted), `-1` (a card, newly mounted),
`-2` (a card that is not formatted), or the older format's probe code.

**IOP-15d — the serial batch, which is the DMA path and not the byte path.**
This is the first user in the archive of the transfer descriptor's DMA
arguments (`spec/06` IOP-6, `docs/analysis/40` §4). The descriptor is built
once and reused:

- The byte fields are zero throughout. **The command bytes never go through
  the data register**; they arrive by DMA on the sending channel and the
  replies leave by DMA on the receiving one.
- One **block per sub-transfer**, `0x24` words -- 144 bytes -- each, with the
  block count equal to the number of sub-transfers. A sub-transfer's command
  starts at its own block's start, and its reply is read from the matching
  block of the receive area. At most eleven sub-transfers to a batch.
- Each sub-transfer's `regdata` word is
  `serial_port | 0x70 | (length << 8) | (length << 18)`, transmit and receive
  lengths equal, with a **zero word after the last one** to terminate the
  list. Note the `0x70` where the controller driver uses `0x40`; neither
  field's meaning has been read, and both are carried as they are.
- `port_ctrl1` for the two card ports is `0xff020405` and `0xff030405`. The
  second control word is set and never reaches hardware through this
  generation of the serial module at all.
- The verdict on a batch is one test: the status word's bits 12 to 15 must
  read `0x1000`. Nothing else in the read path looks at any status register.

**A command's block is only written as far as its payload**, so the bytes
after it are whatever that block last carried. A rebuild that clears each
block first is producing frames the reference does not; one that does not
clear them must not then *check* those bytes.

**IOP-15e — reading one page, and mounting.** A page read is one batch of
three kinds of sub-transfer, in this order:

```
81 23 p0 p1 p2 p3 X 00 00      set the page; X is p0^p1^p2^p3        (9 bytes)
81 43 80 00 ...                128 bytes of data, four times        (134 bytes)
81 43 10 00 ...                the sixteen bytes after the page      (22 bytes)
81 81 00 00                    end the read                          (4 bytes)
```

A frame's length is its payload plus six. The tail frame goes only when the
card's geometry flags say the card has one. Checked: the set-page reply's
byte 8 is `0x5a`; each data chunk's trailing byte is the exclusive-or of its
128; the end frame's reply carries `0x5a`. The tail's own check byte is not
checked. Up to five attempts, each after the first preceded by the probe of
IOP-15c step 1.

The error-correcting codes in the tail are compared per chunk. A page whose
tail ends in `0xff` counts as never written and is accepted without a check.
A single-bit difference is corrected in place, retried, and **accepted
anyway on the fifth attempt**; a difference the code cannot place answers
**-2** after five. A rebuild may leave the codes unchecked, and must then say
so where it says what it does not do -- it is the difference between reading
a damaged card and reading a damaged card quietly.

The mount then reads, in this order: the card's geometry over the serial
interface (`0x26`, thirteen bytes, its reply's own exclusive-or checked),
**page 0**, the **first two pages of the second of the two spare erase blocks
the superblock names**, and then the root directory's first two entries. It
answers "not formatted" when page 0 is uncorrectable, when page 0 does not
begin with the format identifier, or when the root's first two entries are
not the two conventional names. Everything else about page 0 is copied into
the port record and believed.

**The root is not found through the superblock's root-directory field.** A
path beginning with a separator starts at *relative cluster 0*, and so does
the initial current directory, and the field is read only to decide whether a
listing hides the two conventional entries. A rebuild is free to read the
field; it must not depend on it being anything but zero, and it must add the
first-allocatable-cluster offset when it turns a relative cluster into a page.

**IOP-15f — the allocation table, and the entries.** Both are in
`docs/analysis/55` §0.1 and are properties of the card, not of the driver.
What the driver does with them: a cluster's successor is found by two
indirections from the superblock's list, the high bit of an entry is masked,
and the value with every bit set ends the chain. Entries are 512 bytes, two
to a cluster, and a directory's own length field says how many of its slots
are in use.

**IOP-15g — the smallest export surface.** Three entries carry the whole
question, and a rebuild that implements only these has a driver a service can
use:

| Ordinal | Shape | Answers |
|---|---|---|
| 5 | `detect(port, slot)` | IOP-15c |
| 12 | `getdir(port, slot, path, mode, max, out)` | how many entries it wrote |
| 39 | `type(port)` | 0 none, 1 older format, 2 this one |

Two more are needed beneath them, the page read and the error-correcting
code, and the rest of the forty-three may be the bare return the reference
puts in its own unused slots, or a refusal. `getdir` takes a path whose last
separator splits a directory from a pattern, starts a listing with a mode of
zero and continues it with any other, skips entries whose in-use bit is
clear, skips the two conventional entries when it is listing the root, and
answers zero at the end. Each entry it writes is `0x40` bytes: two
eight-byte timestamps, the length, the mode as a half-word, a half-word
copied from the entry, the attributes, and a 32-byte name.

**IOP-15h — the service, and the two calls a shell needs.** SIF RPC id
**`0x80000400`**, one thread, one queue. Its dispatcher subtracts `0x70` from
the `fno` and refuses seventeen or more; it answers with a fixed four-byte
area holding the driver's return, not with the request buffer.

- **`fno 0x78`** is "what is in this slot": the request's words 1 and 2 are
  the port and the slot, word 3 asks for the type, word 4 for the free-space
  count, and word 7 is the EE address to deliver a `0x40`-byte record to. The
  service calls detect, then the type and the free-space entry points if
  asked, sends the record, and answers detect's code.
- **`fno 0x76`** is "list a directory": words 0 to 4 are port, slot, mode,
  how many entries, and the EE address; the path follows at `+0x14`. The
  reference asks the driver for **one entry per call** and sends each
  `0x40`-byte record separately, advancing the address. It answers the number
  sent.

**IOP-15i — what this section does not pin.** The cluster cache, the
memoised chain, the handle table, the filesystem device the driver registers
with the file manager, and the service's thread count are all *how* the
reference gets there. A rebuild that produces the same frames in the same
order and the same answers satisfies IOP-15 without any of them. The
filesystem device in particular is worth having only if something wants to
reach a card by path rather than through the service.

### Verification

Like IOP-6, IOP-13 and IOP-14, **no simulator gate**: nothing offline models a
serial interface, let alone a card behind one. The emulator that can attach a
card image is the gate, and this path has a witness the others did not --
**two real card images, one freshly formatted and one with a save on it,
whose contents are known independently of any rebuild**. So the check is not
"something came back" but "the entries this rebuild lists are the entries a
reader of the image finds, and the formatted card's root is the two
conventional names and nothing else".

This is also the first thing in the archive to drive the transfer
descriptor's DMA arguments, so a failure here is as likely to be in the
serial module or the transfer controller as in the driver. Judge the batch
before judging the card: the status word's `0x1000`, then the set-page
reply's `0x5a`, then the chunk exclusive-ors.
