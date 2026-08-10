# EE Syscalls: Arguments and Return Values

`docs/analysis/19-ee-config-syscalls.md` closed the group survey with the
observation that what remained was "the *behaviour* of individual slots within
each group — the arguments and return values one by one". This is that pass,
over all 125 slots at once.

Reading 98 distinct handlers by hand was not the way to do it, so the question
was turned into one a program can answer: **which argument registers does a
handler read before writing, and does it produce a value?** That is ordinary
liveness analysis, and `tools/eeabi.py` does it.

```sh
python3 tools/romdir.py assets/SCPH-50000.bin --extract <outdir>   # outside the repo
python3 tools/eeabi.py <outdir>/KERNEL                # every slot
python3 tools/eeabi.py <outdir>/KERNEL --slot 0x18    # one, with detail
python3 tools/eeabi.py <outdir>/KERNEL --strings      # with what each names
```

## How the argument list is recovered

A register that is read before it is written on some path from the entry point
is a value the caller had to supply. Computing that is a backward liveness pass
over the handler's control flow graph, intersected with `$a0`–`$a3`; a value
returned is `$v0` written anywhere on the way out.

Two details make it work on this kernel rather than merely run:

- **Calls are resolved, not assumed.** A `jal` uses whatever *its* callee
  reads, computed recursively and memoised. Without that, a handler that hands
  its own `$a0` straight to a subroutine would look as though it took no
  arguments at all — and this kernel is full of four-instruction wrappers that
  do exactly that.
- **The R5900 instruction set is decoded directly.** `tools/romdis.py` warns
  that LLVM has no R5900 target; `lq`/`sq` come out as `<unknown>`. Those are
  precisely the instructions the context switch is built from, so `eeabi.py`
  carries its own decoder rather than parsing disassembly.

## The correction that the scheduler group forces

The first run reported every scheduling syscall as taking four arguments. The
cause is the skeleton of `docs/analysis/17-ee-scheduler-syscalls.md`: each of
those handlers opens with `jal 0x80003680`, and that routine saves the whole
user context with `sq`. It reads every register — because that is its job — so
naive liveness concludes that every register is an argument.

So routines that save or restore a context wholesale are recognised and treated
specially. The test is deliberately narrow: **six or more distinct
caller-saved registers moved to or from memory in one straight-line run**. A
compiled function saves `$s0`–`$s7` and `$ra`; only a context switch saves
`$a0`–`$a3` and `$t0`–`$t9`.

Such a routine then contributes *no* argument reads — and, just as important,
**no clobbers**. It preserves the registers it saves, which is exactly what
lets the skeleton hand the caller's own arguments to the operation it wraps:

```
800030c0  jal   0x80003680        # save context -- $a0..$a3 survive it
800030c8  jal   0x80003ff0        # the operation, still holding the caller's arguments
```

## The check that the method works

That last point makes eight independent predictions. `spec/04` EE-8h records
eight operations published at two slot numbers — once through the rescheduling
skeleton, once as a bare handler. The two paths are *different code* of very
different size, so if the analysis is sound they must agree on arguments:

```sh
python3 tools/eeabi.py <outdir>/KERNEL | grep -E '^0x(25|26|29|2a|2b|2c|2d|2e|33|34|39|3a|41|49|42|43) '
```

| Pair | Rescheduling form | Direct form |
| --- | --- | --- |
| `0x25`/`0x26` | `($a0)` in 28 instructions | `($a0)` in 33 |
| `0x29`/`0x2A` | `($a0, $a1, $a3)` in 31 | `($a0, $a1, $a3)` in 63 |
| `0x2B`/`0x2C` | `($a0)` in 28 | `($a0)` in 18 |
| `0x2D`/`0x2E` | `($a0)` in 28 | `($a0)` in 35 |
| `0x33`/`0x34` | `($a0)` in 28 | `($a0)` in 85 |
| `0x39`/`0x3A` | `($a0)` in 28 | `($a0)` in 44 |
| `0x41`/`0x49` | `($a0)` in 28 | `($a0)` in 95 |
| `0x42`/`0x43` | `($a0)` in 28 | `($a0)` in 76 |

Eight for eight, including the odd one — `0x29`/`0x2A` skip `$a2` and read
`$a3` — which is the case a coincidence would be least likely to reproduce.

The pass also agrees with everything the earlier documents established by hand:
`0x0D`–`0x0F` take `(index, handler)` and return a value (`16`), `0x14`–`0x17`
take one source number each (`16`), `0x01` takes the initialisation bitmask and
`0x02` its three narrow arguments (`19`), `0x4A`/`0x4B` take one structure
pointer each (`19`), and `0x60`–`0x62` take one argument and return nothing
(`18`).

## The shape of the interface

Across all 125 slots:

| Signature | Slots |
| --- | --- |
| `($a0) -> $v0` | 52 |
| `($a0, $a1) -> $v0` | 15 |
| `(-) -> -` | 15 |
| `(-) -> $v0` | 14 |
| `($a0) -> -` | 7 |
| `($a0, $a1, $a2) -> $v0` | 6 |
| `($a0, $a1, $a2, $a3) -> $v0` | 6 |
| everything else | 10 |

Nothing reads a fifth argument. That is worth stating explicitly, because it is
not obvious in advance: the syscall handler runs on the kernel stack of
`spec/04` EE-7d, so the caller's stack is not a usable argument channel, and
the table confirms no slot tries.

Where a fifth value is needed the kernel passes it **internally in `$t0`**.
Slots `0x10` and `0x12` are seven-instruction wrappers that differ only in the
constant they load there before tail-calling one shared routine:

```
80001a30  jal   0x800018b0
80001a3c  addiu $t0, $zero, 0x2     # slot 0x10 -- delay slot
80001a58  jal   0x800018b0
80001a5c  move  $t0, $zero          # slot 0x12
```

A caller never supplies that register; the pair is one operation published in
two modes.

## What the signatures identify

Nine slots the group survey left as "kernel data structures" or did not reach
become legible once the argument list, the string references and the hardware
references are read together.

### Slot 0x74 writes the syscall table

```sh
python3 tools/romdis.py <outdir>/KERNEL --cpu ee --vma 0x80000000 \
    --range 0x800006c0 0x800006d4
```

```
800006c0  sll   $a0, $a0, 0x2
800006c4  lui   $v1, 0x8001
800006c8  addu  $v1, $v1, $a0
800006cc  jr    $ra
800006d0  sw    $a1, 0x4f40($v1)     # 0x80014F40 + number * 4
```

That is the syscall table itself (`spec/04` EE-8a). **A program can replace any
syscall at run time**, `(number, handler) -> nothing`. `docs/analysis/16` found
the same for the exception and interrupt tables through slots `0x0D`–`0x0F`;
the pattern is now complete, and all three tables must live in writable memory.

### Slots 0x04 and 0x05 do not return

Slot `0x04` is a single `j 0x800059A0` — the routine `docs/analysis/15` found
sitting immediately after the `0x7B` wrapper, which calls the program loader
with `rom0:OSDSYS` and `argv = { "BootBrowser" }`. So `0x04` is how a running
program *ends*: it takes nothing and re-enters the browser. `--strings`
corroborates it, listing both constants against the slot.

Slot `0x05` clears three bits of `Status` and then leaves through registers it
loads from kernel memory:

```
80002880  mfc0  $at, $12
80002884  addiu $k0, $zero, -0x1c
80002888  and   $at, $at, $k0        # Status &= ~0x1C
8000288c  mtc0  $at, $12
80002898  lw    $ra, -0x5e00($k0)    # 0x8001A200
800028a0  jr    $ra
800028a4  lw    $sp, -0x5df0($k0)    # 0x8001A210, in the delay slot
```

It returns to a *stored* address on a *stored* stack, not to its caller. A
rebuild must keep that pair of kernel words and this exit shape; a plain
`jr $ra` would return to the wrong place.

### Slots 0x18/0x19 are a timer-driven alarm pair

The `--strings` output attaches Timer 3 (`0x10001800`, `0x10001810`,
`0x10001820`) to slots `0x18`/`0x1E` and `0x19`/`0x1F` and nothing else. Their
code says how they use it:

```
800022dc  ld    $a3, -0x6358($a3)    # a 64-bit allocation bitmap at 0x80019CA8
800022e0  andi  $t2, $a0, 0xffff     # the delay is taken as 16 bits
800022ec  move  $a0, $a3
800022f0  dsrlv $v0, $a0, $v1        # scan the bitmap for a free bit
80002308  slti  $v0, $v1, 0x40       #   over 64 slots
80002324  addiu $v0, $zero, -0x1     # none free -> -1
```

`(delay, handler, argument) -> id`, with `-1` when the 64 slots are full. Its
partner takes the id back:

```
80002580  andi  $v0, $v0, 0x1        # is that bit allocated?
80002590  addiu $v0, $zero, -0x1     #   no -> -1
800025b0  sw    $v1, -0x6350($1)     # decrement the live count at 0x80019CB0
```

Both the bitmap and the count are single kernel objects a rebuild must keep
consistent; the count is not derived from the bitmap on demand.

### Slot 0x09 writes a TLB entry

```
80005bc8  srl   $v1, $a1, 0x18
80005bd0  bne   $v1, $v0, 0x80005bfc # reject unless the top byte is 4
80005bd4  addiu $v0, $zero, -0x1     #   delay slot: the rejected result
80005bd8  mfc0  $v0, $1              # Random
80005bdc  mtc0  $v0, $0              # -> Index
80005be0  mtc0  $a0, $5              # PageMask
80005be4  mtc0  $a1, $10             # EntryHi
80005be8  mtc0  $a2, $2              # EntryLo0
80005bec  mtc0  $a3, $3              # EntryLo1
80005bf4  tlbwi
```

`(PageMask, EntryHi, EntryLo0, EntryLo1) -> index`, `-1` if refused. Three
things are contract rather than detail: the index comes from `Random`, so the
caller does not choose it and *must* read the result; the guard rejects any
`EntryHi` whose top byte is not `4`; and `spec/04` EE-1a's scratchpad entry at
index 0 is therefore never at risk from this call.

### Slots 0x70/0x71 are a shadowed hardware register

```
80000d40  lui   $at, 0xb200
80000d48  move  $v0, $a0             # the return value is the new value
80000d4c  sd    $a0, 0x1010($at)     # 0x12001010, a GS privileged register
80000d54  sd    $a0, 0x6e48($v1)     # and a shadow copy at 0x80016E48
```

Slot `0x70` takes nothing and returns that shadow with `ld`. The register is
write-only in practice, so the kernel keeps the copy; a rebuild that dropped it
would have nothing to answer `0x70` with. Note the 64-bit accesses, for the
same reason `spec/04` EE-11a gives.

### Slots 0x6B and 0x76–0x78 are the SIF channels

`--strings` puts DMA channel registers under four slots and nowhere else:

| Slots | Registers | Shape |
| --- | --- | --- |
| `0x6B`, `0x78` | `0x1000C000`, `0x1000C020` | `(-) -> $v0` |
| `0x76` | `0x1000C400`, `0x1000C430` | `($a0) -> $v0` |
| `0x77` | `0x1000C400`, `0x1000F520`, `0x1000F590` | `($a0, $a1) -> $v0` |

The two channels are the pair the IOP is on the other end of
(`docs/analysis/11-sif-and-rom-driver.md`). `0x6B` and `0x78` differ by one
instruction — both zero the channel's control and count, and `0x78` then starts
it:

```
80006360  sw    $a0, 0x0($v1)        # 0x184 into the control register
80006364  lw    $v0, 0x0($v1)        # and read it back as the result
```

So `0x6B` stops that channel and `0x78` restarts it, both reporting what the
register then read. A rebuild must keep the read-back: the value returned is
the hardware's, not the value written.

### Slot 0x75 is deliberately empty

```
800074c8  jr    $ra
800074cc  nop
```

Two instructions, no arguments, no result. It is *not* one of the thirteen
slots bound to the undefined reporter (`spec/04` EE-8d) — it succeeds silently.
A rebuild must reproduce the distinction: `0x75` is a call that does nothing,
while `0x03` and `0x3F` are calls that complain.

### The undefined reporter reads `$v1` still scaled

```
80001564  srl   $a1, $v1, 0x2        # the syscall number, divided by four
8000156c  addiu $a0, $a0, 0x5489     # "# Syscall: undefined (%d)\n"
80001570  jal   0x800073e0
```

The number reaches the handler in `$v1` **multiplied by four** — the dispatcher
leaves it as the byte index it used, and the reporter divides it back down to
print it. That refines `spec/04` EE-7a: `$v1` carries the number on entry to
the *dispatcher*, and the scaled form on entry to a *handler*.

## Where the answer is honestly "unknown"

The tool marks these rather than guessing, and so does this document:

| Slots | Reported | Why |
| --- | --- | --- |
| `0x63`, `0x67` | `($a0) -> ?` | they leave through the CP0 jump table of `docs/analysis/18`; the pass does not follow indirect jumps |
| `0x07`, `0x10`, `0x12`, `0x23`, `0x24`, `0x29`, `0x3B` | `-> $v0?` | the value comes back from a callee rather than being written in the handler |

`$v0?` still means a value is returned; `?` means the analysis stopped. Neither
is a claim that the slot returns nothing.

Two further limits apply to the whole table. Liveness is a *may* analysis: a
register read on only one path counts as an argument, so an argument used only
in an error branch is still listed. And nothing here establishes what an
argument *means* — only that the caller must supply one.

## What this pins for the rebuild

- The per-slot argument count and return convention, all 125 of them, now have
  a recorded value; they are the contract callers were compiled against.
- **No syscall takes more than four arguments**, and none reads the caller's
  stack. A fifth value, where one is needed, is an internal `$t0` mode passed
  by a wrapper.
- Slot `0x74` installs a syscall handler, so the syscall table joins the
  exception and interrupt tables in having to be writable at run time.
- Slots `0x04` and `0x05` do not return to their caller; `0x05` in particular
  resumes from two stored kernel words.
- The alarm pair `0x18`/`0x19` owns a 64-entry allocation bitmap and a separate
  live count, and reports `-1` on exhaustion or on releasing a free slot.
- Slot `0x09` writes the TLB at a `Random` index and returns it, rejecting any
  `EntryHi` whose top byte is not `4`.
- Slots `0x70`/`0x71` need a kernel shadow of a write-only GS register.
- `0x6B`/`0x78` and `0x76`/`0x77` drive the two SIF DMA channels and return
  what the control register reads back afterwards.
- Slot `0x75` must exist and do nothing quietly.

## Verification

`tools/eeabi.py --check` re-derives every signature from a `KERNEL` image and
compares it against the recorded table, which is address-independent and so
judges a rebuild as readily as the reference:

```sh
python3 tools/eeabi.py <outdir>/KERNEL --check
# ...: ok -- 125 syscall signatures as specified
```

Both reference images pass — `KERNEL` is byte-identical between them (`01`).
The gate is tested in the failing direction too; see
`docs/spec/05-ee-syscall-abi.md`.

Next: the EE side is now specified as far as static reading can take it. What
it still lacks is execution — `docs/project-state.md` §5 — and the
implementation itself.
