# Specification: The EE Reset Path and Kernel Interface

Derived from `docs/analysis/13-ee-boot-path.md`,
`14-ee-kernel-syscalls.md` and `15-ee-syscall-groups.md`.

`docs/spec/03-boot-chain.md` covers the IOP half of the reset vector. This
covers the other half and the interface the EE kernel publishes: the vector
page, exception dispatch, and the syscall table that everything running on the
EE calls through.

What each individual syscall *does* is not specified here. That is the largest
remaining body of analysis, and it will arrive as its own document once the
groups of `15` are worked through; this fixes the frame those slots live in.

## EE-1: Reset entry

The EE enters at `0xBFC00800` (`spec/03` BOOT-1) and, before anything else:

| Register | Value |
| --- | --- |
| `Config` (CP0 `$16`) | `0x00073003` |
| `Status` (CP0 `$12`) | `0x70400000` |
| `Count` (CP0 `$9`) | `0` |
| `Compare` (CP0 `$11`) | `1` |

then writes `0xFFFFFFFF` to `0xB000F500` before touching the TLB. Every CP0
write is followed by `sync.p`.

**EE-1a:** One TLB entry is written, at index 0, with `EntryHi = 0x70000000`,
`EntryLo0 = 0x80000007`, `EntryLo1 = 0x00000007` and `PageMask = 0`, committed
with `tlbwi`. This maps the **scratchpad**, and it must exist before the next
step because the reset path has no other usable memory.

**EE-1b:** The stack is set to `0x70003FF0` — inside that scratchpad mapping.
Main RAM is not usable until EE-2 has run.

## EE-2: RDRAM is called by address

The reset path calls `0x9FC41000` — ROM offset `0x41000`, which is where the
archive stores the `RDRAM` file. It is reached by **hard-coded address, not by
name**.

**EE-2a:** `RDRAM`'s offset within the archive is therefore load-bearing in a
way no other file's is. A build that lays the archive out differently must
either place `RDRAM` at `0x41000` or change this constant to match; the two
cannot drift.

## EE-3: Locating and loading the kernel

The routine at `0xBFC00BF0`, called through its KSEG0 alias `0x9FC00BF0`,
carries its own copy of the self-locating ROMDIR scan (`ARC-4`) — the fifth
independent implementation in the image and the only one in R5900 code.

**EE-3a:** The table search range is `0x9FC00000..0x9FC10000`. That bounds
where the *table* may be found, not where files may live.

**EE-3b:** It resolves the name **`KERNEL`** and copies that file to
`0xA0000000` — physical address 0 — using `lq`/`sq`, sixteen bytes per
iteration, for a length of the file's size rounded up to 16.

**EE-3c:** Both called routines are entered through KSEG0 (the reset vector
masks the target with `0x9FFFFFFF`) so they run cached, while the reset vector
itself runs uncached.

**EE-3d:** After the copy the instruction and data caches are invalidated, and
control transfers with `jr` to `0x80001000`.

## EE-4: The kernel's placement is its layout

Because `KERNEL` lands at physical 0 and is entered at `0x80001000`, its first
`0x1000` bytes are **the EE exception vector page**, and the entry point is the
first thing after it.

**EE-4a:** The boot's result is passed to the kernel in the scratchpad word at
`0x70003FF0`, not in a register. The kernel entry reads it immediately.

## EE-5: The vector page

Populated offsets, and what must be at each:

| Offset | Architectural vector | Content |
| --- | --- | --- |
| `0x000` | TLB refill | the dispatcher of EE-6 |
| `0x080` | counter | `j` to a common handler |
| `0x100` | debug | `j` to the same handler as `0x080` |
| `0x180` | common exception | the dispatcher of EE-6, byte-identical to `0x000` |
| `0x200` | interrupt | its own entry sequence |

**EE-5a:** `0x000` and `0x180` hold the same instructions. A rebuild may share
one implementation but must populate both offsets.

**EE-5b:** The remainder of the page, from about `0x800`, holds leaf routines
that manipulate `Status` and the interrupt-controller registers, reached
through a jump table at `0x80000AA0`. They are inside the vector page
deliberately — they must remain reachable regardless of what else is mapped —
and a rebuild must keep them there.

## EE-6: Exception dispatch

The common entry does not branch on the cause. It saves `$t9` to a fixed slot,
reads `Cause`, masks it with `0x7C`, and uses the result **directly** as a byte
index into a word table:

```
lui   $k0, 0x8001
sd    $t9, 0x5378($k0)
mfc0  $t9, $13
andi  $t9, $t9, 0x7c        # ExcCode, already scaled by 4
addu  $k0, $k0, $t9
lw    $k0, 0x5340($k0)      # table at 0x80015340
jr    $k0
ld    $t9, 0x5378($t9)      # restored in the delay slot
```

**EE-6a:** The table is at `0x80015340`, fourteen entries for `ExcCode` 0–13.

**EE-6f:** The kernel publishes **four** tables in total, all of which a
rebuild must provide:

| Address | Purpose | Entries |
| --- | --- | --- |
| `0x80014F40` | syscall dispatch | 125 |
| `0x80015340` | exception dispatch | 14 |
| `0x80015380` | interrupt dispatch | 8 |
| `0x800154A8` | per-CP0-register read stubs | 8 |

The last exists because `mfc0`'s register number is an instruction field and
cannot be supplied at run time, so "read CP0 register *n*" needs a stub each.

**EE-6d:** There is a **second** dispatch table at `0x80015380`, eight entries,
read by the interrupt vector at `0x80000200` and indexed by interrupt number.
Indexes 2, 3 and 7 are populated — the interrupt controller, the DMA controller
and the counter. (`docs/analysis/16`)

**EE-6e:** Both tables are **installable at run time**, not static: syscalls
`0x0D` and `0x0E` write the exception table over `ExcCode` 1–3 and 4–13, and
`0x0F` writes the interrupt table over indexes 0–7. So `ExcCode 0` cannot be
replaced through a syscall and `ExcCode 8` — the syscall handler itself — can.
They must live in writable memory.

**EE-6b:** Only `ExcCode 8` (`Sys`) has its own handler. All thirteen others
point at one common routine. A rebuild must not assume a handler per cause.

**EE-6c:** There is no shift between the mask and the index. Masking with
`0x7C` rather than `0x7C >> 2` is what makes that work, and a rebuild that
masks differently must compensate.

## EE-7: The syscall ABI

**EE-7a:** The syscall number is passed in **`$v1`**, not `$v0`.

**EE-7b:** A **negative number is negated**, not rejected: callers use the sign
as a flag and both forms reach the same slot.

**EE-7c:** The handler advances `EPC` by 4 before dispatching, so the eventual
return resumes after the `syscall` instruction rather than re-executing it.

**EE-7g:** That advance is a *default*, not a guarantee. The scheduler group
(`docs/analysis/17`) overwrites `EPC` with its operation's return value and
installs a different stack before `eret`, so a scheduling syscall may resume a
different thread entirely. A rebuild must support both.

**EE-7d:** It switches to a kernel stack near `0x80018E80` before anything that
can nest.

**EE-7e:** Context is saved with `sq` — 128-bit stores — because the R5900's
registers are 128 bits wide and the upper halves must survive the call.

**EE-7f:** Number `0x7C` is special-cased before the table lookup and takes its
own path.

## EE-8: The syscall table

**EE-8a:** The table is at `0x80014F40` and has **125 slots**, numbered `0x00`
to `0x7C`, indexed by the absolute syscall number.

**EE-8b:** **No slot is null.** Every number in range resolves to code.

**EE-8c:** Slots are shared: 125 slots resolve to 98 distinct targets. The
sharing is structured, not incidental, and must be reproduced:

| Block | Duplicate |
| --- | --- |
| `0x14`–`0x19` | `0x1A`–`0x1F` |
| `0x63`–`0x66` | `0x67`–`0x6A` |

plus isolated pairs at `0x30`/`0x31`, `0x35`/`0x36`, `0x37`/`0x38`,
`0x45`/`0x46`, `0x47`/`0x48`.

**EE-8d:** Thirteen slots — `0x00`, `0x03`, `0x08`, `0x3F`, `0x54`–`0x5B`,
`0x7C` — are bound to a handler that **reports the undefined number** rather
than returning quietly. Slots `0x03` and `0x3F` are retired rather than never
defined: the kernel carries a message for each blaming the caller's startup
code. A rebuild keeps them occupied so an old caller is diagnosed instead of
jumping into nothing.

**EE-8e:** Slots `0x60`, `0x61` and `0x62` are published through **KSEG1**
(`0xA0002C00`, `0xA00028C0`, `0xA0002980`) while the other 122 use KSEG0.

This is a hardware constraint, not a convention (`docs/analysis/18`): `0x60`
rewrites the cache mode in the low bits of `Config`, and `0x61`/`0x62` walk the
whole cache with the `cache` instruction. Code doing either cannot be fetched
through the cache it is changing. Publishing them at their KSEG0 addresses
would fail intermittently rather than cleanly, which is why a rebuild must not
normalise the alias away.

**EE-8f:** The handlers for slots `0x0D`–`0x1F` live inside the vector page and
must stay there (EE-5b).

**EE-8h:** Eight operations are published at **two slot numbers with two
distinct handlers** — one that reschedules and one that returns to the caller:
`0x25`/`0x26`, `0x29`/`0x2A`, `0x2B`/`0x2C`, `0x2D`/`0x2E`, `0x33`/`0x34`,
`0x39`/`0x3A`, `0x41`/`0x49`, `0x42`/`0x43`. These are **not** EE-8c aliases:
the table entries differ, and collapsing them onto one handler would silently
make every direct call reschedule or every scheduling call return locally.
(`docs/analysis/17`)

**EE-8g:** Slots `0x14`–`0x17` (with their `0x1A`–`0x1D` aliases) toggle one
bit of a hardware mask: `0x1000F010` bit `n` for the INTC pair, `0x1000E010`
bit **`16 + n`** for the DMAC pair. Each reads the register first and writes
only when the write would change the bit, returning `1` when it acted and `0`
when the bit was already in the wanted state. The registers are
write-to-toggle, so an unconditional write is wrong on the second call.

## EE-9: The boot tail

**EE-9a:** Slot `0x06` is the program loader. It uses the archive file
`EELOAD` as the stub that replaces the running program — `EELOAD` is not staged
by the reset vector and is not named in any boot list.

**EE-9b:** Slot `0x7B` is slot `0x06` with the first argument pinned to
`rom0:OSDSYS`, implemented as a four-instruction wrapper that shifts the
remaining arguments along and tail-calls it.

**EE-9c:** The default boot invokes that path with `argc = 1` and
`argv = { "BootBrowser" }`.

**EE-9d:** `rom0:` is the device the IOP's `ROMDRV` registers with `ioman`
(`docs/analysis/11`), so this call crosses the SIF and is served by the IOP —
which means the IOP boot of `spec/03` must have completed before EE-9c can
succeed.

## EE-10: Hardware initialisation is a syscall

The kernel brings up DMAC, VU0, VU1, VIF0, VIF1, GIF, GS, IPU, INTC, TIMER,
FPU, user memory and the scratchpad. It announces each step, which is how the
list is known; the message text is original expression and a rebuild supplies
its own (`docs/clean-room-policy.md` §3).

**EE-10a:** This is **slot `0x01`**, a callable syscall, not a fixed boot
sequence. `$a0` is a bitmask selecting which subsystems to bring up, and each
bit is tested twice — once to emit the announcement and once to guard the work.
The bit assignment is part of the contract: a caller passing a mask must get
exactly those subsystems. (`docs/analysis/19`)

## EE-11: Other pinned interfaces

**EE-11a:** Slot `0x4C` reads the GS revision from the privileged register at
`0x12001000` with a **64-bit `ld`**, takes bits 16–23, and branches on whether
it is below `0x19`. GS privileged registers are doubleword-wide; two word loads
are not equivalent.

**EE-11b:** Slots `0x4A` and `0x4B` are the **set and get** of one packed
configuration word at `0x80022590`, with near-identical code running in
opposite directions. They must not be merged: doing so would make one direction
silently overwrite the other's fields.

**EE-11c:** Slot `0x02` sign-extends three arguments **from 16 bits** before
use. A rebuild taking them at full width would accept values the reference
truncates.

## Verification

`tools/eeksys.py --check` asserts the statically checkable requirements of
EE-5, EE-6 and EE-8 against a `KERNEL` file and exits non-zero naming any that
fail:

```sh
python3 tools/romdir.py assets/SCPH-50000.bin --extract <outdir>   # outside the repo
python3 tools/eeksys.py <outdir>/KERNEL --check
```

The reference passes. As with `spec/02`, the gate is only worth having if it
bites, so it is tested against mutated kernels — each fails naming its
requirement:

| Mutation | Result |
| --- | --- |
| null slot `0x20` | `EE-8b: null slots: ['0x20']` |
| slot `0x60` republished as KSEG0 | `EE-8e: kseg1 slots ['0x61', '0x62'] != ['0x60', '0x61', '0x62']` |
| slot `0x1A` pointed elsewhere | `EE-8c: slot 0x14 and 0x1a differ` |

The dynamic requirements are executed by `tools/eesim.py`, which boots an
image's EE on a simulated R5900 and judges what happens
(`docs/analysis/22-ee-execution.md`):

```sh
python3 tools/eesim.py assets/SCPH-50000.bin --check
```

EE-1, EE-1a, EE-1b, EE-2, EE-3b, EE-3d, EE-7a, EE-7b, EE-7c, EE-8g and EE-10
are all confirmed by execution, on both reference images, and the gate is
tested against mutated images the same way the static one is.

**EE-10b:** Executing also settles the announcement *order*, which this
document had left open: for a full mask it is GS, INTC, TIMER, DMAC, VU1,
VIF1, GIF, VU0, VIF0, IPU, FPU, user memory, scratchpad — **not** the order the
messages are stored in. A rebuild driven by the stored order would bring the
subsystems up in the wrong sequence.

**What is still not verified.** Three things, all recorded in `analysis/22`:
the memory controller's serial protocol, which `eesim.py` stubs because no
requirement concerns it; EE-9's boot tail, which crosses the SIF to an IOP that
the EE simulator does not have; and the scheduler resuming a *different* thread
(EE-7g), which is executed but never observed switching. The EE side remains
the riskier half, but no longer for want of any execution at all.
