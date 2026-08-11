# Specification: The EE Syscall Interface, Slot by Slot

Derived from `docs/analysis/21-ee-syscall-abi.md`, with the group-level
observations of `16`–`19` behind it.

`docs/spec/04-ee-kernel.md` fixes the frame the syscall slots live in — the
table, its aliasing, its dispatch — and says explicitly that "what each
individual syscall *does* is not specified here". This specifies the part of
that which is a compiled-in contract: **how many arguments each slot takes and
whether it returns a value**, together with the individual slots whose
behaviour is pinned by something a rebuild could not guess.

Everything here is address-independent. A rebuild may place its handlers
anywhere; it may not change a slot's arity or return convention, because
callers were compiled against them.

## SYS-1: The signature table

Arguments are passed in `$a0`–`$a3` and the result comes back in `$v0`. Each
slot must take exactly the registers listed and return a value where one is
listed. Two notations mark how well the evidence supports the return column,
and SYS-1c says what they mean: `$v0?` is a value the handler gets from a
callee rather than writing itself, and `?` is a slot whose exit the static
analysis could not follow. Both do return a value.

| Slot | Signature | Slot | Signature | Slot | Signature |
| --- | --- | --- | --- | --- | --- |
| `0x00` | `(-) -> -` | `0x01` | `($a0) -> $v0` | `0x02` | `($a0, $a1, $a2) -> $v0` |
| `0x03` | `(-) -> -` | `0x04` | `(-) -> $v0` | `0x05` | `(-) -> -` |
| `0x06` | `($a0, $a1, $a2) -> $v0` | `0x07` | `($a0, $a1, $a2, $a3) -> $v0?` | `0x08` | `(-) -> -` |
| `0x09` | `($a0, $a1, $a2, $a3) -> $v0` | `0x0A` | `($a0, $a1) -> $v0` | `0x0B` | `($a0) -> $v0` |
| `0x0C` | `($a0) -> $v0` | `0x0D` | `($a0, $a1) -> $v0` | `0x0E` | `($a0, $a1) -> $v0` |
| `0x0F` | `($a0, $a1) -> $v0` | `0x10` | `($a0, $a1, $a2, $a3) -> $v0?` | `0x11` | `($a0, $a1, $a2, $a3) -> $v0` |
| `0x12` | `($a0, $a1, $a2, $a3) -> $v0?` | `0x13` | `($a0, $a1, $a2, $a3) -> $v0` | `0x14` | `($a0) -> $v0` |
| `0x15` | `($a0) -> $v0` | `0x16` | `($a0) -> $v0` | `0x17` | `($a0) -> $v0` |
| `0x18` | `($a0, $a1, $a2) -> $v0` | `0x19` | `($a0) -> $v0` | `0x1A` | `($a0) -> $v0` |
| `0x1B` | `($a0) -> $v0` | `0x1C` | `($a0) -> $v0` | `0x1D` | `($a0) -> $v0` |
| `0x1E` | `($a0, $a1, $a2) -> $v0` | `0x1F` | `($a0) -> $v0` | `0x20` | `($a0) -> $v0` |
| `0x21` | `($a0) -> $v0` | `0x22` | `($a0, $a1) -> $v0` | `0x23` | `(-) -> $v0?` |
| `0x24` | `(-) -> $v0?` | `0x25` | `($a0) -> $v0` | `0x26` | `($a0) -> $v0` |
| `0x27` | `(-) -> $v0` | `0x28` | `(-) -> $v0` | `0x29` | `($a0, $a1, $a3) -> $v0?` |
| `0x2A` | `($a0, $a1, $a3) -> $v0` | `0x2B` | `($a0) -> $v0` | `0x2C` | `($a0) -> $v0` |
| `0x2D` | `($a0) -> $v0` | `0x2E` | `($a0) -> $v0` | `0x2F` | `(-) -> $v0` |
| `0x30` | `($a0, $a1) -> $v0` | `0x31` | `($a0, $a1) -> $v0` | `0x32` | `(-) -> $v0` |
| `0x33` | `($a0) -> $v0` | `0x34` | `($a0) -> $v0` | `0x35` | `($a0) -> $v0` |
| `0x36` | `($a0) -> $v0` | `0x37` | `($a0) -> $v0` | `0x38` | `($a0) -> $v0` |
| `0x39` | `($a0) -> $v0` | `0x3A` | `($a0) -> $v0` | `0x3B` | `(-) -> $v0?` |
| `0x3C` | `($a0, $a1, $a2, $a3) -> $v0` | `0x3D` | `($a0, $a1) -> $v0` | `0x3E` | `(-) -> $v0` |
| `0x3F` | `(-) -> -` | `0x40` | `($a0) -> $v0` | `0x41` | `($a0) -> $v0` |
| `0x42` | `($a0) -> $v0` | `0x43` | `($a0) -> $v0` | `0x44` | `($a0) -> $v0` |
| `0x45` | `($a0) -> $v0` | `0x46` | `($a0) -> $v0` | `0x47` | `($a0, $a1) -> $v0` |
| `0x48` | `($a0, $a1) -> $v0` | `0x49` | `($a0) -> $v0` | `0x4A` | `($a0) -> $v0` |
| `0x4B` | `($a0) -> $v0` | `0x4C` | `($a0, $a1, $a2, $a3) -> $v0` | `0x4D` | `(-) -> $v0` |
| `0x4E` | `($a0, $a1, $a2, $a3) -> $v0` | `0x4F` | `($a0) -> $v0` | `0x50` | `(-) -> $v0` |
| `0x51` | `(-) -> $v0` | `0x52` | `(-) -> $v0` | `0x53` | `(-) -> $v0` |
| `0x54`–`0x5B` | `(-) -> -` | `0x5C` | `($a0) -> $v0` | `0x5D` | `($a0) -> $v0` |
| `0x5E` | `($a0) -> $v0` | `0x5F` | `($a0) -> $v0` | `0x60` | `($a0) -> -` |
| `0x61` | `($a0) -> -` | `0x62` | `($a0) -> -` | `0x63` | `($a0) -> ?` |
| `0x64` | `($a0) -> $v0` | `0x65` | `($a0, $a1) -> $v0` | `0x66` | `($a0) -> $v0` |
| `0x67` | `($a0) -> ?` | `0x68` | `($a0) -> $v0` | `0x69` | `($a0, $a1) -> $v0` |
| `0x6A` | `($a0) -> $v0` | `0x6B` | `(-) -> $v0` | `0x6C` | `($a0) -> -` |
| `0x6D` | `($a0) -> $v0` | `0x6E` | `($a0, $a1, $a2) -> $v0` | `0x6F` | `($a0, $a1, $a2) -> $v0` |
| `0x70` | `(-) -> $v0` | `0x71` | `($a0) -> $v0` | `0x72` | `($a0) -> -` |
| `0x73` | `($a0, $a1) -> -` | `0x74` | `($a0, $a1) -> -` | `0x75` | `(-) -> -` |
| `0x76` | `($a0) -> $v0` | `0x77` | `($a0, $a1) -> $v0` | `0x78` | `(-) -> $v0` |
| `0x79` | `($a0, $a1) -> $v0` | `0x7A` | `($a0) -> $v0` | `0x7B` | `($a0, $a1) -> $v0` |
| `0x7C` | `(-) -> -` | | | | |

**SYS-1a:** `0x29`, `0x2A`: the gap is not a typo. These read `$a0`, `$a1` and
`$a3` and **not** `$a2`. A rebuild that packs the arguments densely would take
the third argument from the wrong register.

**SYS-1b:** **No slot takes a fifth argument, and none reads the caller's
stack.** The handler runs on the kernel stack of `spec/04` EE-7d, so the
caller's stack is not addressable as an argument channel. Where the kernel
needs a fifth value it is an internal mode in `$t0`, written by a wrapper
(slots `0x10` and `0x12` are one operation in two modes); a caller never
supplies it.

**SYS-1c:** Seven slots — `0x07`, `0x10`, `0x12`, `0x23`, `0x24`, `0x29`,
`0x3B` — return a value produced by a callee rather than written in the handler
itself, and `0x63`/`0x67` leave through the CP0 jump table so the analysis
cannot see where their value comes from. All of them do return one. This
requirement records the evidence's limit, not an exemption.

## SYS-2: Paired operations agree on their arguments

The eight operations of `spec/04` EE-8h are published twice, once through the
rescheduling skeleton and once as a bare handler. The two entry paths are
different code, and both must present the same argument list:

`0x25`/`0x26`, `0x29`/`0x2A`, `0x2B`/`0x2C`, `0x2D`/`0x2E`, `0x33`/`0x34`,
`0x39`/`0x3A`, `0x41`/`0x49`, `0x42`/`0x43`.

**SYS-2a:** The rescheduling skeleton must **preserve** `$a0`–`$a3` across its
context-saving prologue, because the operation it then calls is handed the
caller's arguments untouched. A prologue that clobbered them would break every
one of these eight pairs in the rescheduling direction only.

## SYS-3: The undefined slots take nothing

The thirteen slots of `spec/04` EE-8d — `0x00`, `0x03`, `0x08`, `0x3F`,
`0x54`–`0x5B`, `0x7C` — take no arguments and return no value. They report and
return.

**SYS-3a:** The reporter reads the syscall number from `$v1` **multiplied by
four**: the dispatcher leaves the value as the byte index it used, and the
reporter divides it back down. This refines `spec/04` EE-7a — `$v1` holds the
number at the dispatcher, the scaled form at the handler — and a rebuild that
un-scales it earlier must adjust the reporter to match.

**SYS-3b:** Slot `0x75` is **not** one of them. It is an empty handler that
returns immediately, taking nothing and reporting nothing. A rebuild must keep
the distinction between a call that does nothing quietly and a call that
diagnoses its caller.

## SYS-4: The cache trio returns nothing

Slots `0x60`, `0x61` and `0x62` — the KSEG1-published cache operations of
`spec/04` EE-8e — each take one argument and produce no value. A rebuild that
returned a status from them would be harmless to a caller that ignores it and
wrong for one that does not; they are specified as returning nothing because
that is what the reference does.

The argument names caches in its low two bits in all three.

**SYS-4a:** Slot `0x60` clears the low three bits of `Config` — the cache mode
— and then **ANDs** the argument into the result rather than ORing it, so what
it writes back is **zero, unconditionally**. This is a defect in the shipped
ROM (`docs/analysis/18`, which quotes the instruction word), and reproducing it
is deliberate: it is what every retail machine does, so it is what software
written for the platform encountered.

**SYS-4b:** Slots `0x61` and `0x62` are a matched pair over `Config`'s two
cache-enable bits, at 16 and 17. `0x61` **sets** the named bits and `0x62`
**clears** them, and each sweeps a cache first only if the sweep is needed —
`0x61` invalidates one that is currently off, before letting stale tags answer;
`0x62` writes back one that is currently on, whose dirty lines are the only
copy of what they hold. Both leave every other bit of `Config` alone, and both
are therefore safe to repeat.

## SYS-5: The syscall table is installable

**SYS-5a:** Slot `0x74` takes `(number, handler)` and writes the handler into
the syscall table at `spec/04` EE-8a's address, indexed by the number. It
returns nothing.

**SYS-5b:** All three dispatch tables are therefore run-time installable: the
exception and interrupt tables through `0x0D`–`0x0F` (`spec/04` EE-6e) and the
syscall table through `0x74`. All three must live in writable memory, and none
may be placed in ROM or in a read-only mapping.

## SYS-6: Slots that do not return to their caller

**SYS-6a:** Slot `0x04` takes no arguments and does not come back: it jumps to
the routine that runs `rom0:OSDSYS` with `argv = { "BootBrowser" }` (`spec/04`
EE-9c). It is how a program terminates.

**SYS-6b:** Slot `0x05` clears `Status` bits `0x1C`, then loads `$ra` and `$sp`
from two fixed kernel words and returns through them. A rebuild must keep both
words and this exit shape; returning normally would resume the caller instead
of the stored context.

**SYS-6c:** Any slot in the rescheduling group may resume a different thread
entirely, as `spec/04` EE-7g already requires. SYS-1's return convention
describes the value the operation produces, not a promise about which context
observes it.

## SYS-7: Individually pinned behaviour

These are the slots where the reference does something a reasonable rebuild
would otherwise get wrong.

**SYS-7a — the alarm pair (`0x18`/`0x1E`, `0x19`/`0x1F`).** `0x18` takes
`(delay, handler, argument)`, masks the delay to **16 bits**, allocates from a
**64-entry bitmap**, and returns the entry's index or `-1` when all 64 are in
use. It drives Timer 3 (`0x10001800`, `0x10001810`, `0x10001820`). `0x19`
takes the index back, returns `-1` if that entry was not allocated, and
decrements a **separate live count** kept alongside the bitmap. The count is a
stored value, not derived on demand, and both must stay consistent.

**SYS-7b — the TLB write (`0x09`).** Takes
`(PageMask, EntryHi, EntryLo0, EntryLo1)`. It **rejects, returning `-1`, unless
the top byte of `EntryHi` is `4`**; otherwise it takes the index from `Random`,
writes with `tlbwi`, and returns the index used. The caller does not choose the
index and must read the result. Whether this can overwrite the scratchpad
mapping of `spec/04` EE-1a is **not** settled here: that depends on `Wired`,
which the reset path is not observed to set. A rebuild must arrange that
protection deliberately rather than assume the index avoids entry 0.

**SYS-7c — the GS interrupt-mask pair (`0x70`/`0x71`).** `0x71` writes its
argument to the GS privileged register at `0x12001010` with a **64-bit `sd`**,
keeps a **shadow copy in kernel memory**, and returns the value it just set —
not the previous one. `0x70` takes nothing and returns that shadow with `ld`.
The shadow is not optional: the register cannot be read back, so a rebuild
without it has no answer for `0x70`.

**SYS-7d — the SIF channels (`0x6B`, `0x78`, `0x76`, `0x77`).** `0x6B` and
`0x78` operate the channel at `0x1000C000`: both zero its control and count
registers, and `0x78` then starts it by writing `0x184` to the control
register. `0x76` and `0x77` operate the channel at `0x1000C400`, `0x77` also
touching `0x1000F520`/`0x1000F590`. Each returns **what the control register
reads back afterwards**, not the value written; a rebuild must perform the
read-back rather than echo its own write.

**SYS-7e — range guards are part of the contract.** Where the reference
validates an argument it returns `-1` rather than failing, and the bound is
observable: `0x0A`–`0x0C` accept an index below `16` (a *signed* comparison, so
negative values pass the guard); `0x2A` accepts `$a0` below `256` and `$a1` in
`0..127`. A rebuild widening a bound accepts calls the reference rejects; one
narrowing it rejects calls that were expected to work.

## Verification

`tools/eeabi.py --check` re-derives every signature from a `KERNEL` image by
liveness analysis and compares it against SYS-1, then checks SYS-2, SYS-3 and
SYS-4 structurally. It is address-independent, so it judges a rebuilt kernel on
the same terms as the reference:

```sh
python3 tools/romdir.py assets/SCPH-50000.bin --extract <outdir>   # outside the repo
python3 tools/eeabi.py <outdir>/KERNEL --check
```

Both reference images pass (their `KERNEL` files are byte-identical). As with
`spec/02` and `spec/04`, the gate is only worth having if it bites, so it is
tested against mutated kernels — each fails naming its requirement:

| Mutation | Result |
| --- | --- |
| slot `0x14` pointed at the alarm handler | `SYS-1: slot 0x14 is ($a0, $a1, $a2) -> $v0, want ($a0) -> $v0` |
| the direct form `0x2A` repointed at another operation | `SYS-2: slot 0x29 takes ($a0, $a1, $a3) but its direct form 0x2a takes ($a0)` |
| a retired slot given a live handler | `SYS-3: undefined slot 0x54 is ($a0) -> $v0, want (-) -> -` |
| cache slot `0x60` given a value-returning handler | `SYS-4: cache slot 0x60 is ($a0) -> $v0, want ($a0) -> -` |

Several requirements here are additionally **executed** by `tools/eesim.py`,
which boots the kernel on a simulated R5900 and then calls its syscalls
(`docs/analysis/22-ee-execution.md`):

```sh
python3 tools/eesim.py assets/SCPH-50000.bin --check
python3 tools/eesim.py assets/SCPH-50000.bin --syscall 0x14 3
```

SYS-3b, SYS-5a, SYS-7a, SYS-7b and SYS-7c are confirmed that way, as is the
`$v1` convention every call depends on. SYS-7b's open question is closed from
the other direction too: the kernel announces its own TLB layout at boot, and
the scratchpad mapping of `spec/04` EE-1a is entry 0 while the allocatable
range starts at 13.

**What is still not verified.** SYS-1 remains a *static* result. Liveness is a
`may` pass, so an argument read only on an error path counts as an argument,
and it establishes that a register is read, never what the value means —
executing a syscall confirms it returns, not that it consumed every argument
listed. SYS-7d's SIF channels are not exercised, because that traffic ends at
an IOP the EE simulator does not have.
