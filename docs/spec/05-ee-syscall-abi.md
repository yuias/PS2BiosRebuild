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

**SYS-1b:** **No slot reads the caller's stack, and one slot takes a fifth
argument.** The handler runs on the kernel stack of `spec/04` EE-7d, so the
caller's stack is not addressable as an argument channel. Where the kernel
needs a fifth value it is in `$t0`: for slots `0x10` and `0x12` an internal
mode written by a wrapper (they are one operation in two modes) that a caller
never supplies; for slot `0x3C` the caller's fifth argument, `root`
(SYS-8a; `docs/analysis/30`). SYS-1's table lists `0x3C` as four arguments
because it was derived from `$a0`–`$a3` alone.

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

**SYS-4c:** Slot `0x63` — aliased at `0x67` — takes a register number and
returns that **CP0** register. It is a dispatcher, not a function, and must be:
`mfc0` encodes its register number as an instruction field, so "read register
*n*" cannot be expressed any other way than a table of stubs, one per register.
The table is `spec/04` EE-6f's. Indices 0 to 6 read their register; index 7
returns without writing `$v0` at all, the R5900 having nothing there. **The
index is not bounds-checked**, so a caller passing more than seven reads a word
past the table and jumps to it.

**SYS-4d:** Slot `0x65` — aliased at `0x69` — is described in
`docs/analysis/18` as a cache operation over an address range, and executing it
shows the mechanism is the other way round: it walks the cache **by index**,
reads each line's tag out of CP0 register 28, forms the address that tag
implies, and operates only on lines whose address falls between `$a0` and
`$a1`. Both endpoints are rounded down to a 64-byte line first. A rebuild that
walked addresses instead would miss lines the reference reaches and touch lines
it does not.

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

## SYS-8: The main thread's setup

Derived from `docs/analysis/30-ee-thread-setup.md`. These are what a program's
runtime calls before `main`, and how its arguments reach it.

**SYS-8a — slot `0x3C`, `(gp, stack, stack_size, args, root)`, root in
`$t0`.** Let `top = stack + stack_size`, except that a `stack` of `-1` —
compared at 64 bits, so a zero-extended `0xFFFFFFFF` is *not* `-1` — means
`top = memory_size - 0x1000` with `memory_size` from EE-2b. The slot returns
`top - 0x2a0` and treats the `0x2a0` bytes above it as a saved context of
EE-7e's shape, priming `$gp` = gp, `$sp` = `$fp` = `top - 0x20` and `$ra` =
root, so a thread resumed through that frame starts with them set and returns
to `root`. It records in the current thread's record the context (the value
returned), `gp`, the stack base and size, `root` and `args`.

**SYS-8b — the argument block.** Before returning, `0x3C` fills the caller's
`args` block from the argument list the launcher stored (SYS-8d): word 0 is
`argc`, words 1–16 are the `argv` pointers, and the strings themselves follow
from byte `0x44` on, each copied with its NUL. A runtime reads `argc` and
`argv` from that layout; a rebuild placing the strings elsewhere, or the
pointers before `argc`, hands it garbage.

**SYS-8c — slots `0x3D` and `0x3E`.** `0x3D(start, size)` stores and returns
`start + size`, or the current thread's stack base (SYS-8a) when `size` is
negative — "the heap runs up to the stack". `0x3E()` returns what was stored,
0 before any `0x3D`.

**SYS-8d — how a program receives its arguments.** A launcher — slot `0x07`,
and the ROM's own boot through EE-9c — packs the argument strings, NUL
included, back to back into a kernel buffer and records that list and its
count in the current thread's record; the program collects them through
SYS-8b. The launcher enters the program with **the registers it had itself at
the `syscall`** — for `0x07(entry, gp, argc, argv)` that is `$a0` = entry,
`$a1` = gp, `$a2` = argc, `$a3` = argv — and `$v0` = the entry point. Nothing
puts `argc`/`argv` in `$a0`/`$a1` for the program: a runtime that finds
`argc = 0` in its block may look at `$a0` as a launcher-supplied pointer, and
one that finds it non-zero does not.

## SYS-9: Semaphores

Derived from `docs/analysis/32-ee-semaphores.md`. Ten slots: `0x40` creates,
`0x41`/`0x49` delete, `0x42`/`0x43` signal, `0x44` waits, `0x45`/`0x46` poll,
`0x47`/`0x48` report. Where a pair exists the lower number reschedules on the
skeleton of `docs/analysis/17` and the higher returns to its caller; `0x45`,
`0x46`, `0x47` and `0x48` are literally the same code twice, since neither
polling nor reporting can block.

**SYS-9a — identity and range.** A semaphore id is its index in a table of
**256**; every id-taking slot returns `-1` for an id outside `0..255` and for
an id that is not allocated. Ids are handed out from a **LIFO free list**, so
the first creation after a deletion reuses the deleted id.

**SYS-9b — `0x40` create, `(block) -> id | -1`.** The block is read at
`+0x04` (max count), `+0x08` (initial count), `+0x10` (attribute) and `+0x14`
(option); `+0x00` and `+0x0C` are not read. A negative initial count returns
`-1`; an exhausted table returns `-1`. **Nothing else is validated**: an
initial count above the max count, a zero max count, an all-zero block are all
accepted. The count starts at the initial count; the wait list starts empty.

**SYS-9c — `0x42`/`0x43` signal, `(id) -> id | -1`.** With no thread
waiting, the count is incremented **without regard to the max count** — the
reference never reads it here, and a full semaphore signalled again counts
higher. With a waiter, the count is left alone and the first waiter is handed
the signal: taken off the wait list and made ready (a waiting thread that had
also been suspended is marked ready without being queued).

**SYS-9d — `0x44` wait, `(id) -> id | -1`, or blocks.** A positive count is
decremented and the call returns `id`. A zero count queues the calling thread
on the semaphore's wait list, records the id in the thread, and reschedules;
the call returns to that thread only when a signal or a deletion hands it
back. There is no non-rescheduling form.

**SYS-9e — `0x45`/`0x46` poll, `(id) -> id | -1`.** A positive count is
decremented and `id` returned; a zero count returns `-1` — indistinguishable
from an unallocated id.

**SYS-9f — `0x47`/`0x48` report, `(id, out) -> id | -1`.** Writes `out+0x00`
count, `+0x04` max count, `+0x0C` waiting threads, `+0x10` attribute, `+0x14`
option; **`out+0x08` is not written** (the SDK's `init_count` slot keeps what
the caller had there). An allocated semaphore with a zero count is reported,
not refused.

**SYS-9g — `0x41`/`0x49` delete, `(id) -> id | -1`.** Every waiting thread is
dequeued and made ready, the entry is marked free and pushed on the free list.
Deleting an unallocated id returns `-1`.

## SYS-10: Threads and the scheduler

Derived from `docs/analysis/33-ee-threads.md`, with `17` (the skeleton) and
`30` (the record). Slots `0x20`–`0x3A`; the reschedule pairs of `17` apply.

**SYS-10a — records and identity.** A thread id is its index in a table of
**256** records; id 0 is the thread the kernel boots on. `0x2F` returns the
current id. A record carries: state, resume PC, context (the address of its
saved frame), `$gp`, initial and current priority, wait type, wait id, a
wakeup count, attribute and option, the entry point, `argc`/arguments,
stack base and size, root, and heap end (SYS-8). States: `0` free, `1`
running, `2` ready, `4` waiting, `8` suspended, `0xC` waiting and suspended,
`0x10` dormant. Ids are handed out from a free list; a garbage creation block
is not validated beyond the free list having an entry.

**SYS-10b — priorities and the ready queues.** Priorities `0..127` are
settable, lower is better; there is one FIFO ready queue per priority and one
more at `128`, where the boot thread sits (`0x2A(0, 5)` answers its previous
priority, **128**). The scheduler picks the head of the lowest-numbered
non-empty queue, keeps a cached lowest-ready priority, and marks the picked
thread running. A thread made ready is appended to its queue.

**SYS-10c — the switch (EE-7g made concrete).** A rescheduling slot saves the
caller's registers into the caller's frame — EE-7e's shape, 16 bytes a
register, `$gp`/`$sp`/`$fp`/`$ra` at `+0x1C0`/`+0x1D0`/`+0x1E0`/`+0x1F0`,
`$v0` at `+0x20`, `$a0` at `+0x40` — records the post-syscall EPC as its
resume PC, marks it ready (or waiting), picks the next thread, restores that
thread's frame and `eret`s to its resume PC. The wrappers of `0x25`, `0x29`,
`0x2B`, `0x2D`, `0x33`, `0x39`, `0x41`, `0x42` switch on **every** success —
they check only the operation's `-1` — and rely on the pick to reselect the
caller when it is still best; `0x23`, `0x24` never come back to their caller;
`0x32` and `0x44` decide inside their operation. A thread woken by a forced
release (a delete or a `0x2D`) resumes with **`$v0 = -1`** in its blocking
call. When nothing is ready the kernel stops with a message; that path is
not specified further.

**SYS-10d — creating and starting.** `0x20(block) -> id | -1`: pops the free
list (`-1` when empty); reads `func` (`+0x04`), `stack` (`+0x08`),
`stack_size` (`+0x0C`), `gp` (`+0x10`), `initial_priority` (`+0x14`,
halfword); state dormant; primes a frame at `top - 0x2A0` with `$gp`,
`$sp = $fp = top - 0x20`, and `$ra` = a kernel address a returning thread
function lands on (SYS-10i). No priority check. `0x22(id, arg) -> id | -1`:
`id` in `1..255`, not the caller, dormant, else `-1`; writes `arg` into the
frame's `$a0` slot and the record, makes the thread ready, and switches.

**SYS-10e — ending.** `0x23` (exit) and `0x24` (exit and delete) do not
return: the caller's record is reset to dormant (`0x23`: resume PC back to
the entry, priority back to initial, frame re-primed — startable again with
`0x22`) or freed (`0x24`), and the next thread is picked. `0x21(id) -> id |
-1`: `id` in `1..255`, not the caller, dormant, else `-1`; frees the record.
`0x25`/`0x26 (id) -> id | -1`: `id` in `1..255` else `-1`; free or dormant
`-1`; otherwise the thread is taken out of whatever it is in — the ready
queue, a wait list — and reset to dormant.

**SYS-10f — priority and rotation.** `0x29`/`0x2A (id, priority) -> previous
priority | -1`: id 0 is the caller (the fourth register the SDK passes is not
consulted); `id` below 256, priority `0..127`, thread neither free nor
dormant, else `-1`; a ready thread is moved to its new queue. `0x2B`/`0x2C
(priority) -> priority | -1`: below 128 else `-1`; rotates that queue, head to
tail; an empty queue is a no-op that still yields.

**SYS-10g — sleeping and waking.** `0x32() ->`: with a positive wakeup count,
decrement it and return the current id without sleeping; otherwise the caller
waits (wait type 1) until `0x33` wakes it, then returns. `0x33`/`0x34 (id) ->
id | -1`: `id` below 256 else `-1`; a thread sleeping (waiting, type 1) is
made ready; a thread that is ready, or waiting on something else, or
suspended, has its wakeup count **incremented** and `id` returned; a running,
free or dormant thread returns `-1`. `0x35`/`0x36 (id) -> previous count |
-1`: resets the wakeup count, no state check beyond the range.

**SYS-10h — suspending and releasing.** `0x37`/`0x38 (id) -> id | -1`: `id`
in `1..255`; running or ready → suspended (dequeued); waiting → waiting and
suspended (kept in its wait list); anything else `-1`. `0x39`/`0x3A (id) ->
id | -1`: `id` in `1..255`, not the caller; suspended → ready; waiting and
suspended → waiting; else `-1`. `0x2D`/`0x2E (id) -> id | -1`: `id` in
`1..255`; a waiting thread is released from its wait object and made ready,
resuming with `-1` (SYS-10c); waiting and suspended → suspended; other states
are a no-op returning `id`; free `-1`.

**SYS-10i — the root of a thread.** A thread function that returns lands, via
the `$ra` its frame was primed with, in kernel code that exits it as `0x23`
does. The reference computes that address from a kernel table; what matters
is the effect: a returning thread function does not fall off into memory.

**SYS-10j — status.** `0x30`/`0x31 (id, out) -> state | -1`: id 0 is the
caller; `id` below 256 else `-1`; with `out` null only the state is returned;
otherwise the SDK's twelve-word status block is filled from the record. The
boot thread reports **ready** (2), not running, on the reference.

**SYS-10k — the boot thread stays.** A program does not run on the boot
thread: the reference enters it on a thread of its own — the SDK's runtime
sees its first `0x20` answer **3** after the runtime's own thread, so the
program's is **1** — and the boot thread, 0, remains ready at 128 for as long
as the program runs. It is what the scheduler picks when every other thread
waits, and a program whose threads all block on an interrupt's answer (an
SDK client's `WaitSema` inside `SifBindRpc`) resumes when that interrupt
makes one ready again. Only when the scan finds **no** thread does the
reference print `# <Thread> No active threads` and reboot into the OSD; a
rebuild must not reach that state while the boot thread exists.
(`docs/analysis/33`; the thread ids and the survival of an all-waiting
program are observed on the reference under PCSX2 with `tests/m1`.)

## SYS-11: The table is indexed without a bound

`docs/analysis/33`: the dispatcher computes `table[number]` for any number
and slot `0x74` stores at `table[number]` for any number; EE-8a's 125 slots
are how many the reference *fills*, not how many can be reached. The SDK's
runtime installs handlers at `0x7F` and `0x82` through `0x74` and calls
them. A rebuild must therefore keep a table that `0x74` can write and the
dispatcher can read at least that far — ours holds 256 slots, `0x7D` and up
initially the reporter of EE-8d — and must not corrupt anything when `0x74`
is given such a number.

## SYS-12: Interrupt handlers and their delivery

Derived from `docs/analysis/35-ee-interrupts-and-sif-slots.md`, with `16`.

**SYS-12a — installing.** `0x10 (cause, handler, next, arg) -> id | -1`
accepts INTC causes `0` and `2..14` — `1` is refused — and `0x12 (channel,
handler, next, arg) -> id | -1` accepts DMAC channels `0..15`. Both draw ids
from one counter starting at 1; an id is the only handle a caller has, and
`0x11 (cause, id)` / `0x13 (channel, id) -> id | -1` remove by it, refusing an
id that is not installed on that cause. A new handler with `next = -1` goes
to the head of its cause's list; the installer's `$gp` is kept with it.

**SYS-12b — delivery.** On an INTC interrupt the kernel reads
`INTC_STAT & INTC_MASK`, takes the lowest pending source, **acknowledges it
in `INTC_STAT` before any handler runs**, and calls each handler installed
for it, youngest first, with the cause number in `$a0` (and the installed
argument in `$a1`; the reference passes only `$a0` at the dispatch it was
read at). DMAC likewise from `D_STAT & D_MASK`, per channel, acknowledged
in `D_STAT` first. Return values are not consulted by the dispatch.

**Each handler is entered on the `$gp` its installer had** -- which is what
SYS-12a keeps that value for, and what the reference's own dispatch does
(`docs/analysis/35`: the record's `+0x0C` is "the caller's, so the handler runs
with the installer's gp"). A handler built with small-data addressing reaches
its globals through it.

**Each handler is entered on a quadword-aligned `$sp`.** This is not
housekeeping: `sq` ignores the low four bits of its address, and a compiled EE
handler saves its frame with `sq` because its registers are that wide. Entered
on a merely doubleword-aligned stack it stores its frame up to eight bytes
below where it believes it put it, reads the frame back at the offsets it
wrote, and acts on the wrong words -- without faulting. A rebuild that hands a
handler whatever `$sp` its own dispatcher happened to have will see a retail
title's SIF handler consume a command packet and then dispatch on the wrong
word of it. Handlers
run in kernel mode with `EXL` clear and interrupts masked by `EIE`, so a
handler may make syscalls — the SDK's SIF handler calls the direct forms and
`0x78` from inside — and may re-enable interrupts with `ei` at its own risk.

**SYS-12c — rescheduling on the way out.** A handler that makes a thread
ready — through a direct-form thread or semaphore slot — sets a flag the
kernel clears before calling handlers and reads after; if it is set on the
way out, the interrupted thread is parked as SYS-10c parks a caller (its
resume address the interrupted instruction) and the pick runs. This is how
an interrupt wakes a higher-priority thread; without it, a woken thread waits
for the interrupted one to yield.

**SYS-12d — `EIE`.** The R5900's `ei`/`di` set and clear `Status` bit 16,
the master enable the SDK's `EIntr`/`DIntr` use; a program enables
interrupts itself, after its runtime is up. The kernel leaves it as the
program set it when it returns from an interrupt.

## SYS-13: The SIF slots

Derived from `docs/analysis/35` (§3) and `24`; refines SYS-7d.

**SYS-13a — registers, `0x79 (reg, value)` and `0x7A (reg)`.** Hardware
registers by number: `1` MSCOM, `2` SMCOM (read only; a write returns 0),
`3` MSFLG, `4` SMFLG — a write returns the value written, a read the
register. A number with bit 31 set (sign-extended, as `lui`/`ori` produce)
names a **software register**, index `reg & 0x7FFFFFFF` below 32, stored in
the kernel: `0x79` stores and `0x7A` reads it back; an index of 32 or more
reads 0. The SDK keeps its receive addresses and its "RPC initialised" mark
in software registers 0..2.

**SYS-13b — `0x78`.** Arms channel 5 (`0x1000C000`, SIF0, IOP→EE) as a
destination chain: `QWC` 0, `CHCR = 0x184`; returns the control register
read back. `0x6B` is the same without the start.

**SYS-13c — `0x77 (list, count) -> word`.** Each list entry is
`{src, dest, size, attr}`, sixteen bytes. For each, the kernel puts on the
wire what BOOT-11a describes: a header quadword naming `dest` in IOP memory
with the transfer's flags, then `size` bytes from `src`; the last entry ends
the chain. `attr` bit `0x02` asks the IOP for an interrupt on arrival, and
is carried in the header's flag bits. Channel 6 (`0x1000C400`, SIF1) is
restarted with `CHCR = 0x184` on the built chain. The return is a non-zero
word describing the transfer, which the caller hands to `0x76`; the reference
packs list positions into it, and 0 means the list was refused.

**SYS-13d — `0x76 (word) -> -1 | 0`.** `-1` once channel 6 has stopped —
the transfer is complete — and a non-negative value while it is still
running. The SDK's `SifDmaStat` loop waits for the `-1`.

## SYS-14: The display slots

Derived from `docs/analysis/46-ee-syscalls-for-a-title.md` §1 and §6. These are
the slots a title calls before it draws anything, and the first of them is the
only place in the interface where the kernel programs the GS's raster.

**SYS-14a — `0x02 (interlace, mode, field)`.** Each of the three arguments is a
**signed 16-bit value in the low half of its register**, and the reference
sign-extends every one of them before use; a rebuild reading the full 32 bits
would accept a caller the reference does not. `mode` is `2` for NTSC and `3`
for PAL — the kernel-level numbering, which is not the SDK's separate display
mode enumeration.

For each of those two modes the kernel writes **six GS privileged registers, in
this order**: SMODE1 (`0x12000010`), SYNCH1 (`0x12000040`), SYNCH2
(`0x12000050`), SYNCHV (`0x12000060`), SMODE2 (`0x12000020`), SRFSH
(`0x12000030`). Every one is a **64-bit `sd`**; a pair of word stores is a
different transaction on this bus and not an equivalent. The values:

| Register | `mode == 2` (NTSC) | `mode == 3` (PAL) |
| --- | --- | --- |
| SMODE1 | `0x0000000740834504` | `0x0000000740836504` |
| SYNCH1 | `0x0007f5b61f06f040` | `0x0007f5c21fc83030` |
| SYNCH2 | `0x000000000033a4d8` | `0x00000000003484bc` |
| SYNCHV | `0x00c7800601a01801` | `0x00a9000502101401` |
| SRFSH | `0x0000000000000008` | `0x0000000000000008` |

SMODE1 additionally carries **bit 0 of the kernel's display-configuration
doubleword at bit 25**. SYNCHV takes an alternate value — `...1802` for NTSC,
`...1404` for PAL — when that same word's bits `8..6` are non-zero **and** its
bit 40 is set. That word is kernel-resident and no caller names its address: it
is reached only through the syscalls that read it, so its **layout** is part of
the interface and its **address is not**.

SMODE2 is the only register the caller's own arguments reach: interlaced
(`interlace != 0`) writes `(field & 1) << 1 | 1` — INT set, FFMD carrying
`field` — and progressive writes a **literal zero**, not a masked argument.

The slot **produces no value**. No path that programs the GS assigns the result
register, so SYS-1's `-> $v0` is satisfied by whatever was already there; a
rebuild inventing a status would be answering a question the interface does not
ask.

**SYS-14b — `0x73 (flagPtr, alarmPtr)`.** Stores the two arguments verbatim, as
raw pointers, into two fixed kernel words. No validation, no dereference, no
hardware access, no return value. That is the entire syscall; the vsync-time
bookkeeping that reads them back belongs to whichever interrupt path installs
the VBLANK handler, not to this slot.

**SYS-14c — what `0x02` does not have to serve.** The reference reaches the
dispatcher above through gates that read boot-configuration state a reset
machine leaves at rest, and at rest every one of them falls through to it
(`docs/analysis/46` §1a). What that dispatcher then reaches **beyond** NTSC and
PAL — the `mode == 0`/`1` "keep the current mode" shorthand, modes `0x72` and
`0x73`, the `0x1a..0x51` range and `0x52`, the alternative timings a GS
revision other than 1 selects, and the settling spin that precedes all of them
— is **not required**, and neither is the second SMODE1-shaped value the
reference hands unstored to a helper below the analysed range. A rebuild that
serves only NTSC and PAL must leave every other mode's registers **as it found
them** rather than program them with a guess.

## SYS-15: The configuration blocks

Derived from `docs/analysis/46` §3 and §5. Three slots that move bytes between
a caller and a kernel-resident block. None touches hardware, and — as in
SYS-14a — the blocks' **addresses are not part of the interface**, only their
sizes, layouts and clamping.

**SYS-15a — `0x4a (addr)` and `0x4b (addr)`.** A matched pair over one 32-bit
kernel word, `0x4a` copying from the caller and `0x4b` to it; the reference is
the same code with its load and store operands swapped. The **low half is
copied a field at a time** through six masks — `0x1`, `0x6`, `0x8`, `0x10`,
`0x1fe0`, `0xe000` — which together account for every bit of it, and the **high
half is copied whole** as one 16-bit halfword at `addr + 2`. What any individual
field configures is not settled. Neither slot produces a value it computed:
the reference leaves whatever its last load put in the result register.

**SYS-15b — `0x6f (config, size, offset)`, the copy.** A byte copy out of a
**128-byte** kernel block: `config[i] = block[offset + i]` for `i` in
`0..size`. The bound is **clamped, not enforced** — a request whose
`size + offset` exceeds `0x80` is *shortened* to what the block has left, and
an `offset` already at or past `0x80` copies nothing rather than failing. The
bound test is **unsigned**, so a `size` or `offset` large enough to look
negative fails it and is clamped rather than read as a negative count. Whether
the clamp's own `offset < 0x80` comparison is signed was not read off the
bytes, so a *pair* of arguments that wraps back inside the bound — `offset`
`-4` with `size` `8` sums to `4` — is **not settled** here; the reference
reads below its block for that call, and a rebuild that guards it would differ.

**SYS-15c — `0x6f`'s result has nothing to do with its arguments.** On every
path, whether or not a byte was copied, the slot returns bits `47..44` of
**SYS-14a's display-configuration doubleword** — or `0` when that word's
mode-select field (bits `8..6`) is clear. It reads no argument to produce this
and touches neither block. A rebuild deriving the result from the copy instead
would agree with the reference only by accident, and only while the
configuration word is at rest.

## SYS-16: The cache flush

Derived from `docs/analysis/46` §4 and `docs/analysis/18`. Slot `0x64`, and
`0x67`–`0x6A`'s doubling of `0x63`–`0x66` publishes it at `0x68` as well; the
two share **one handler**, unlike the rescheduling operations SYS-2 covers,
because there is nothing here for an interrupts-disabled entry point to do
differently.

**SYS-16a — `0x64 (operation)`.** Four behaviours, dispatched on the argument:

| `operation` | Sweep | `cache` op | Span | Ways |
| --- | --- | --- | --- | --- |
| `0` | write back the data cache | `0x14` | `0x1000` | both |
| `1` | invalidate the data cache | `0x16` | `0x1000` | both |
| `2` | invalidate the instruction cache | `0x07` | `0x2000` | both |
| anything else | operation `1`, then operation `2` | | | |

Each walk goes one 64-byte line at a time, over both ways of every line, with a
sync on either side of each `cache` instruction.

**SYS-16b — every unmatched value is the combined form.** `3` is what a caller
names it by, but the reference tests only `0`, `1` and `2` and lets everything
else fall through to the pair. **Write-back is reachable only by asking for
`0` exactly** — it is not part of the combined path.

**SYS-16c — the flush sweeps unconditionally.** Unlike SYS-4b's pair it does
not consult `Config`, does not skip a cache that is in the wanted state, and
does not change any enable bit. A caller reaches it having just written code or
about to hand memory to the IOP, not to reconfigure anything.

The reference's leaf routines are not the same routines SYS-4b's pair calls,
and `docs/analysis/46` did not distinguish which of the two `sync` forms each
uses. A rebuild carrying over the form `docs/analysis/18` read off the bytes
for the same `cache` operation is not contradicted by any evidence in hand.

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
`$v1` convention every call depends on. `tools/imgcheck.py` calls the same
slots on a **rebuilt** image: SYS-7c's doubleword round trip through the shadow,
SYS-14a's two modes and their SMODE2 branches, SYS-14c's "leave an unserved
mode alone", SYS-15a's round trip and SYS-15b's two clamps, and SYS-16a for
every operation it names and one it does not. SYS-7b's open question is closed from
the other direction too: the kernel announces its own TLB layout at boot, and
the scratchpad mapping of `spec/04` EE-1a is entry 0 while the allocatable
range starts at 13.

**What is still not verified.** SYS-1 remains a *static* result. Liveness is a
`may` pass, so an argument read only on an error path counts as an argument,
and it establishes that a register is read, never what the value means —
executing a syscall confirms it returns, not that it consumed every argument
listed. SYS-7d's SIF channels are not exercised, because that traffic ends at
an IOP the EE simulator does not have. SYS-16's sweeps are executed but not
*observed*: the simulator has no cache for a walk to have an effect on, so what
is gated there is that each operation terminates, not that it swept. SYS-15c's
non-zero branch is likewise unexercised, since nothing in the image writes the
configuration word it reads.
