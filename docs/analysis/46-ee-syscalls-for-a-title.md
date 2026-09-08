# EE Syscalls a Retail Title Calls, Slot by Slot

`docs/analysis/43-fileio-and-title-boot.md` §7 and §10 named the EE syscalls
`SLPS-25918`'s own start-up calls that our kernel does not yet serve:
`0x02`, `0x04`, `0x4a`, `0x4b`, `0x64`, `0x68`, `0x6f`, `0x71` (already
specified — `spec/05` SYS-7c), `0x73`, plus whatever `0x74` installs (already
specified — `spec/05` SYS-5a), and flagged `0x3e` with a question mark. This
reads the reference kernel's own handler for each of the unspecified ones —
table entry to handler address to disassembly, the same method
`docs/analysis/14-ee-kernel-syscalls.md` used to find the table itself, with
the group documents `16`–`19` and `docs/analysis/21`'s arity table (`spec/05`
SYS-1) behind it.

`0x3e`, `0x29`/`0x2a` and `0x2b`/`0x2c` were checked against `spec/05` first:
all three are already fully specified there (SYS-8c for `0x3e` —
`EndOfHeap`, `spec/05` SYS-8c; SYS-10f for `0x29`/`0x2a` —
`ChangeThreadPriority`, and `0x2b`/`0x2c` — `RotateThreadReadyQueue`) and are
not repeated here. `0x54`–`0x5b` are `spec/05` SYS-3's undefined slots — the
title's linked binary carrying call sites for them is not evidence they run
on the real boot path (`43` §10 already says so); nothing new is added here.

```sh
python3 tools/romdir.py assets/SCPH-50000.bin --extract <outdir>   # outside the repo
python3 tools/eeksys.py <outdir>/KERNEL --slot 0x02   # ... one per slot below
python3 tools/romdis.py <outdir>/KERNEL --cpu ee --vma 0x80000000 --range <addr> <addr+N>
```

`tools/romdis.py`'s `llvm-objdump` back end elides a run of instruction
words that decode identically to the line before them (visible as `...` in
its output), which hides real code whenever a genuine loop body or a run of
literal `nop`s sits at such a boundary — both occur below. Every range in
this document was therefore cross-decoded a second way: the raw 32-bit words
read directly out of the file and decoded by hand against the standard
MIPS-III/COP0 encoding (opcode, `rs`/`rt`/`rd`/`shamt`/`funct` fields), which
was checked first against `docs/analysis/14`'s already-published syscall
entry disassembly (`0x80000280`–`0x800002fc`) and reproduced it instruction
for instruction before being trusted on new ground. Every address quoted
below is a run-time address in `KERNEL`'s own KSEG0 mapping, so file offset =
address − `0x80000000`, per `docs/analysis/13-ee-boot-path.md`.

Names are taken from ps2sdk's `ee/kernel/include/kernel.h` and
`ee/kernel/include/syscallnr.h`, and — for the GS privileged register
addresses and field layouts quoted for cross-reference only —
`common/include/gs_privileged.h` and `ee/graph/include/graph.h`, all fetched
from `https://raw.githubusercontent.com/ps2dev/ps2sdk/master/...`. Every name
and signature taken from a header is marked `[header]`; the header's own
`GS_SET_*` macros were checked against the reference's raw values and, where
they disagree, the reference's bytes are what is reported (§1). No ps2sdk
`.c`/`.cpp` source was read for this document — headers only.

## 1. `0x02` — `SetGsCrt(s16 interlace, s16 pal_ntsc, s16 field)` `[header]`

`__NR_SetGsCrt` is `2` `[header]`, and the header's three-argument, `void`
prototype matches `spec/05` SYS-1's `0x02: ($a0, $a1, $a2) -> $v0` exactly:
three arguments, and — as this section's last paragraph settles — a return
slot the reference never deliberately writes.

```
0x8000bd58  addiu $sp, $sp, -0x20
0x8000bd5c  sll   $a0, $a0, 0x10       ; sign-extend each 16-bit argument out
0x8000bd6c  sra   $a0, $a0, 0x10       ; of the low half of its 32-bit slot
0x8000bd60  sll   $a1, $a1, 0x10
0x8000bd74  sra   $a1, $a1, 0x10
0x8000bd7c  sra   $t1, $a2, 0x10        ; $t1 = field, sign-extended
```

`$a0` = `interlace`, `$a1` = `pal_ntsc` (called `mode` below), `$t1` holds the
sign-extended `field` for the rest of the routine. `graph.h`'s own comment
"2 = NTSC 3 = PAL" and `GRAPH_CMOD_NTSC 2` / `GRAPH_CMOD_PAL 3` `[header]`
match the reference's own dispatch exactly (below): the raw kernel-level
`mode` argument uses these same two numbers, not `libgraph`'s separate
`GRAPH_MODE_*` enum.

### 1a. Structure (every branch address is in `KERNEL`, `0x8000bd58`–`0x8000cc98`)

1. **`mode == 0` or `mode == 1`** (`0x8000bd78`–`0x8000bddc`): "reuse the
   current mode" shorthand. Both read a persisted 64-bit configuration word
   at `0x80015960` and extract a 3-bit field from it (`mode == 0`: bits
   `5..3`; `mode == 1`: bits `8..6`) through a `movz`/`movn`-shaped
   instruction (raw word `0x0062280a`, `SPECIAL` funct `0xa`) whose exact
   effect on `mode`/`$a1` was not traced further — **open question**, and out
   of scope for the NTSC/PAL values this document exists to pin down.
2. **A settling delay**: `0x8000bde0`–`0x8000be10` reads a persisted byte at
   `0x80022618` into `$v0` for later use, then spins a bare `nop`×7 loop
   `0x270f` (9999) times before touching any GS register.
3. **A nibble gate**: bits `7..4` of that same `$v0` (`0x00`, `0x10`, `0x20`
   or `0x30`) select the "direct" path below; any other value (including
   `0x00` itself once past the `0x30` check — see `0x8000be3c`) instead falls
   through to the "general dispatcher" (§1c).
4. **The direct path also gates on GS_CSR's `REV` field** (`gs_privileged.h`
   `[header]`: bits `23..16` of `0x12000000+0x1000`, the GS's own revision
   ID) — `0x8000be54`–`0x8000be68`: if `REV != 1`, falls to a second,
   differently-valued direct implementation (§1d); if `REV == 1` **and**
   `mode != 2`, the routine does nothing at all and returns — no GS register
   is touched, matching neither NTSC nor PAL.
5. **The general dispatcher** (`0x8000c1f8` onward) is reached whenever the
   nibble/counter gates above are not satisfied and additionally consults two
   more persisted words (`0x80022580`, a counter compared against `0x31`, and
   `0x80022584`, a flag) that can force `mode` to `2` under a condition not
   traced further here (**open question**). It then dispatches on `mode`
   itself: `2` (NTSC, §1e), else `3` (PAL, §1e), else `0x72`, else `0x73`,
   else a range test equivalent to `0x1a <= mode < 0x52`, else `mode == 0x52`
   — every one of those last four outcomes ends in a tail call
   (`j 0x8000b290`) to a routine **below** the range this pass disassembled;
   not decoded here, since it is outside NTSC/PAL (**open question**).

None of the three code paths that *do* write GS registers for `mode == 2`
or `mode == 3` (§1c/1d/1e below) ever assigns `$v0` a value that is not
simply whatever an earlier scratch computation left there — every one of
them exits through the same `0x8000cc90: ld $s0, 0x0($sp); jr $ra`, which
sets nothing. A rebuild only needs *some* value in `$v0` on return to match
the ABI; the reference does not contract a specific one.

### 1b. Register value tables — how to read them

Every row is `sd <value>, 0(<address>)` — a 64-bit store to a GS privileged
register (`gs_privileged.h` addresses `[header]`: SMODE1 `0x12000010`, SMODE2
`0x12000020`, SRFSH `0x12000030`, SYNCH1 `0x12000040`, SYNCH2 `0x12000050`,
SYNCHV `0x12000060`). A `?` in the value column marks a term the routine ORs
in at run time; the note names the source register and the shift that placed
it, exactly as decoded from the bytes at the cited address. Two concrete
cases were traced to a specific bit and cross-checked against
`GS_SET_SMODE2(INT, FFMD, DPMS)` `[header]`'s field layout, which matches
exactly:

- General dispatcher, `mode == 2`, `interlace == 1`, SMODE2 write at
  `0x8000c33c`: value is `($t1 << 48 >> 47) | 1` — i.e. `field`'s bit 0
  placed at bit 1, ORed with a literal `1` at bit 0. This is exactly
  `GS_SET_SMODE2(INT=1, FFMD=field, DPMS=0)` `[header]`, INT hard-wired to 1
  because this is the `interlace == 1` branch.
- General dispatcher, same block, SMODE1 write at `0x8000c2e4`: the ORed term
  is bit 0 of the **persisted word at `0x80015960`** (not the caller's
  `interlace` argument), shifted left 25 bits (`andi $a2,$a2,1; dsll
  $a2,$a2,0x19`).

The other `?` terms below were not individually re-derived to this level;
each is annotated with the register and shift instructions at its address so
it can be reproduced (**open question**: full field-by-field decode of every
remaining `?`).

### 1c. Fast path — `REV == 1`, `mode == 2` (NTSC), `0x8000be70`–`0x8000c03c`

No internal branches in either half — both are unconditional straight-line
code once entered, and neither touches SYNCH2 (`0x50`) or SYNCHV (`0x60`) at
all.

**`interlace == 1`** (`0x8000be74`–`0x8000bf60`):

| Address | Register | Value | Note |
| --- | --- | --- | --- |
| `0x8000bea0` | SMODE1 | `? \| 0x0014020c30801e04` | `?` = `field` (`$t1`), `dsll32 0x10; dsra 0x12` → net left-shift 30 |
| `0x8000bec4` | SMODE1 | `? \| 0x0014020830801e04` | same `?`; differs from the row above only in bit 34 (`VCKSEL`'s low bit, `GS_SET_SMODE1` `[header]`) |
| `0x8000bee8` | SMODE2 | `0x0007e5af1ecc80c8` | fixed; exceeds `GS_SET_SMODE2`'s documented 4-bit field width — see §1f |
| `0x8000befc` | SRFSH | `0x00000000002f141e` | fixed |
| `0x8000bf34` | SYNCH1 | `0x00c7800601a01801` | fixed |
| `0x8000bf54` | SMODE1 | `? \| 0x0014021830801e04` | 3rd SMODE1 write; differs from the 1st in bit 20 |
| `0x8000bf5c` | SMODE1 | `? \| 0x0014021030801e04` | 4th SMODE1 write; differs from the 3rd only in the same `VCKSEL` bit as rows 1–2 |

**`interlace == 0`** (`0x8000bf68`–`0x8000c03c`), same shape:

| Address | Register | Value | Note |
| --- | --- | --- | --- |
| `0x8000bfa0` | SMODE1 | `0x0014020c50801e04` | fixed (no `?` term in this sub-block) |
| `0x8000bfa8` | SMODE1 | `0x0014020850801e04` | 2nd write, `VCKSEL` bit differs |
| `0x8000bfcc` | SMODE2 | `0x0007e5af1ecc80c8` | identical to the `interlace == 1` value |
| `0x8000bfe0` | SRFSH | `0x00000000002f141e` | identical |
| `0x8000c018` | SYNCH1 | `0x00c7800601a01802` | differs from the `interlace == 1` value only in the low byte (`0x1801` → `0x1802`) |
| `0x8000c034` | SMODE1 | `0x0014021850801e04` | 3rd write |
| `0x8000c038` | SMODE1 | `0x0014021050801e04` | 4th write, `VCKSEL` bit differs from the 3rd |

### 1d. Alternative direct path — `REV != 1`, `mode == 2`, `0x8000c050`–`0x8000c1ec`

Unlike §1c this path **does** write SYNCH2 and SYNCHV, and SMODE2 is a fixed
constant (`interlace == 0`: literally zero) rather than field-dependent.

**`interlace == 1`** (`0x8000c050`–`0x8000c124`):

| Address | Register | Value |
| --- | --- | --- |
| `0x8000c070` | SMODE1 | `0x00000004008344f4` |
| `0x8000c094` | SYNCH1 | `0x0007e5af1ecc80c8` |
| `0x8000c0a8` | SYNCH2 | `0x00000000002f141e` |
| `0x8000c0dc` | SYNCHV | `0x00c7800601a01801` |
| `0x8000c0e4` | SMODE1 | `0x00000004008244f4` (2nd write) |
| `0x8000c104` | SMODE1 | `0x00000004004044f4` (3rd write) |
| `0x8000c114` | SMODE2 | `? \| 0x0000000000000001` |
| `0x8000c120` | SRFSH | `0x0000000000000008` |

**`interlace == 0`** (`0x8000c12c`–`0x8000c1ec`):

| Address | Register | Value |
| --- | --- | --- |
| `0x8000c148` | SMODE1 | `0x00000004008344f4` |
| `0x8000c16c` | SYNCH1 | `0x0007e5af1ecc80c8` |
| `0x8000c180` | SYNCH2 | `0x00000000002f141e` |
| `0x8000c1b4` | SYNCHV | `0x00c7800601a01802` |
| `0x8000c1cc` | SMODE1 | `0x00000004008244f4` (2nd write) |
| `0x8000c1d4` | SMODE1 | `0x00000004004044f4` (3rd write) |
| `0x8000c1dc` | SMODE2 | `0x0000000000000000` — written as a **literal zero**, `sd $zero` |
| `0x8000c1ec` | SRFSH | `0x0000000000000008` |

### 1e. General dispatcher — `mode == 2` (NTSC) and `mode == 3` (PAL)

Reached via `0x8000c1f8`; `0x8000c28c` checks `mode == 2` first, else
`0x8000c530` checks `mode == 3` (§1a step 5). Both mode blocks have the same
shape — an `interlace == 1` straight-line block, and an `interlace == 0`
block that further branches on two bits read from the persisted word at
`0x80015960` (`0x8000c3fc`/`0x8000c40c` for NTSC, mirrored for PAL) to choose
between two SYNCHV constants.

**`mode == 2`, `interlace == 1`** (`0x8000c298`–`0x8000c368`):

| Address | Register | Value | Note |
| --- | --- | --- | --- |
| `0x8000c2e4` | SMODE1 | `? \| 0x0000000740834504` | `?` = bit 0 of `*0x80015960`, shifted to bit 25 |
| `0x8000c2ec` | SYNCH1 | `0x0007f5b61f06f040` | fixed |
| `0x8000c300` | SYNCH2 | `0x000000000033a4d8` | fixed |
| `0x8000c330` | SYNCHV | `0x00c7800601a01801` | fixed |
| `0x8000c33c` | SMODE2 | `(field_bit0 << 1) \| 1` | see §1b's worked example |
| `0x8000c360` | SRFSH | `0x0000000000000008` | fixed |

| `0x8000c36c` | SMODE1 | `? \| 0x0000000740814504` | 7th write, in the delay slot of the branch to the shared tail; same `?` as the 1st write, and the constant differs from the 1st only in bit 17 (SINT) |

(The value is built at `0x8000c364` `or $a2, $a2, $a0` and stored by the
`sd $a2, 0($t0)` in the delay slot of `b 0x8000c4b8` — an earlier reading of
this block took it for unstored. The shared tail's `jal 0x80007580` is a
machine-configuration routine — it compares a kernel word against `0x40`,
`0x60`, `0x61`, pulses a byte at `0xBF803218` or a halfword at `0xBA00000A`,
and reads two configuration values through `0x80007840` — and touches no GS
register; the earlier open question about it is closed. So the sequence
**closes with SMODE1 written a second time with SINT cleared**, on both
interlace paths, which is what leaves the sync interrupt running; the
`REV == 1` fast path of §1c reaches the same end through its own 3rd and 4th
SMODE1 writes.)

**`mode == 2`, `interlace == 0`** (`0x8000c36c`–`0x8000c4b8`):

| Address | Register | Value | Note |
| --- | --- | --- | --- |
| `0x8000c3bc` | SMODE1 | `? \| ? \| 0x0000000740834504` | two runtime terms folded in: bit 0 of `*0x80015960` at bit 25 (as the interlaced block), and **bit 1 of the same doubleword at bit 36** (`ld; dsll 0x1f; dsra32 0; andi 1; dsll32 0x4` at `0x8000c488`–`0x8000c4a4`, decoded from the identical construction before the 7th write) |
| `0x8000c3d8` | SYNCH1 | `0x0007f5b61f06f040` | fixed |
| `0x8000c3f4` | SYNCH2 | `0x000000000033a4d8` | fixed |
| `0x8000c450` | SYNCHV | `0x00c7800601a01801` unless **both** bits `8..6` of `*0x80015960` are nonzero (`0x8000c3fc`) **and** its bit 40 is set (`0x8000c40c`), in which case `0x00c7800601a01802` | two chained tests, both against the persisted word |
| `0x8000c464` | SMODE2 | `0x0000000000000000` | literal zero, `sd $zero` |
| `0x8000c46c` | SRFSH | `0x0000000000000008` | fixed |
| `0x8000c4b4` | SMODE1 | `? \| ? \| 0x0000000740814504` | 7th write: same two terms as `0x8000c3bc`, base constant differs only in bit 17 (SINT cleared) |

**`mode == 3`, `interlace == 1`** (`0x8000c538`–`0x8000c60c`) — identical
shape to `mode == 2`'s `interlace == 1` block, different constants:

| Address | Register | Value |
| --- | --- | --- |
| `0x8000c588` | SMODE1 | `? \| 0x0000000740836504` |
| `0x8000c590` | SYNCH1 | `0x0007f5c21fc83030` |
| `0x8000c5a4` | SYNCH2 | `0x00000000003484bc` |
| `0x8000c5d4` | SYNCHV | `0x00a9000502101401` |
| `0x8000c5e0` | SMODE2 | `? \| 0x0000000000000001` |
| `0x8000c604` | SRFSH | `0x0000000000000008` |
| `0x8000c610` | SMODE1 | `? \| 0x0000000740816504` (7th write, SINT cleared, in the delay slot of the branch to the shared tail at `0x8000c75c`) |

**`mode == 3`, `interlace == 0`** (`0x8000c610`–`0x8000c774`ish) — identical
shape to `mode == 2`'s `interlace == 0` block:

| Address | Register | Value |
| --- | --- | --- |
| `0x8000c660` | SMODE1 | `? \| ? \| 0x0000000740836504` |
| `0x8000c67c` | SYNCH1 | `0x0007f5c21fc83030` |
| `0x8000c698` | SYNCH2 | `0x00000000003484bc` |
| `0x8000c6f4` | SYNCHV | `0x00a9000502101401` or `...1404` per the same bit test as NTSC |
| `0x8000c708` | SMODE2 | `0x0000000000000000` (literal zero) |
| `0x8000c710` | SRFSH | `0x0000000000000008` |
| `0x8000c758` | SMODE1 | `? \| ? \| 0x0000000740816504` (7th write, the same two terms as NTSC's `0x8000c4b4`, built at `0x8000c72c`–`0x8000c754`) |
| `0x8000c758` | SMODE1 | `? \| ? \| 0x0000000740816504` |

`mode == 0x72` and `mode == 0x73` (`0x8000c7d4` and `0x8000ca0c`) repeat this
exact same four-block shape (interlace × two SMODE1/SYNCHV variants) with yet
more constants (`0x8000c804`–`0x8000cad0`, `0x8000ca3c`–`0x8000cbd0`) — not
transcribed, since they are outside NTSC/PAL and this document's scope.

### 1f. Open questions specific to `0x02`

- The `movz`/`movn`-shaped instruction at `0x8000bda0`/`0x8000bddc` (mode 0/1
  "keep current") was not decoded to its exact effect.
- What the counter at `0x80022580` and flag at `0x80022584` gate, beyond
  "can force `mode` to 2", was not traced.
- SMODE2's fixed constant in the §1c fast path
  (`0x0007e5af1ecc80c8`) sets bits above `GS_SET_SMODE2`'s documented 4-bit
  field width (`gs_privileged.h` `[header]`); either the GS silicon ignores
  undefined upper bits of that register, or this reading of the address is
  wrong. Not independently confirmed against hardware in this pass.
- Every remaining `?` term in the tables above (i.e. every one not worked
  out in §1b's two examples) is annotated with its source shift but not
  hand-decoded to a named `GS_SET_*` field.
- `mode == 0x72`, `0x73`, the `0x1a..0x51` range, and `mode == 0x52` all end
  in a tail call to `0x8000b290`, below the range this pass disassembled.
  Not identified.

## 2. `0x04` — the browser-return mechanism (`KExit` `[header]` name, `__NR_KExit = 4`)

`spec/05` SYS-6a already establishes that slot `0x04` "jumps to the routine
that runs `rom0:OSDSYS` with `argv = { "BootBrowser" }`" and never returns.
This adds the byte-level mechanism, since ps2sdk's `kernel.h` names this slot
`KExit` `[header]` and declares it `void KExit(s32 exit_code)` — a signature
this analysis shows the reference kernel does **not** honour on this slot
(SYS-1 already lists `0x04` as `(-) -> $v0`, no arguments read):

```
0x80000d80  j  0x800059a0
```

`0x800059a0`–`0x800059d0`:

```
lui  $v0, 0x8001;  addiu $v0, $v0, 0x5d40   ; $v0 = 0x80015d40 = "BootBrowser\0"
lui  $a0, 0x8001;  addiu $a0, $a0, 0x5d30   ; $a0 = 0x80015d30 = "rom0:OSDSYS\0"
sw   $v0, 0x0($sp)                          ; argv[0] = &"BootBrowser"
addiu $a1, $zero, 0x1                       ; $a1 = argc = 1
jal  0x80005598                             ; = slot 0x06's own target (LoadExecPS2)
move $a2, $sp                               ; $a2 = argv = &stack slot just written
```

Both strings were read directly out of the file at those addresses and are
exactly `"rom0:OSDSYS\0"` and `"BootBrowser\0"`, back to back. `0x80005598`
is independently confirmed as slot `0x06`'s own table target
(`tools/eeksys.py <outdir>/KERNEL --slot 0x06`), so this is a **direct call**
into the reference's own `LoadExecPS2` implementation with the same argument
convention SYS-1 gives slot `0x06` — `($a0=path, $a1=argc, $a2=argv) ->
$v0` — not a re-entry through the syscall table. Because `LoadExecPS2` itself
never returns to its caller (`spec/05` SYS-6a), the `ld $ra`/`jr $ra` that
follows the `jal` in `0x800059a0`'s body is unreachable: `0x04` genuinely
never comes back, exactly as specified, and this is *why* — it tail-calls a
function that does not return, rather than doing anything not-returning
itself.

No hardware register is touched by `0x04` itself; everything happens inside
`0x06`'s own routine (out of scope here, already covered by `spec/04` EE-9c
and `docs/analysis/15`).

## 3. `0x4a`/`0x4b` — `SetOsdConfigParam(void *addr)` / `GetOsdConfigParam(void *addr)` `[header]`

`__NR_SetOsdConfigParam = 0x4a`, `__NR_GetOsdConfigParam = 0x4b` `[header]`,
both declared `void` in `kernel.h`. Both copy a fixed 6-byte kernel-resident
structure at `0x80022590` to or from the caller's `addr`, one bitfield group
at a time, byte-for-byte mirror images of each other (`SetOsdConfigParam` at
`0x8000d470`, copying *into* `0x80022590`; `GetOsdConfigParam` at
`0x8000d3c0`, copying *out of* it — same address, opposite direction):

```
0x8000d470  lui  $a1, 0x8002;  addiu $a1, $a1, 0x2590   ; $a1 = 0x80022590
0x8000d478  lw   $v0, 0x0($a0)                          ; caller's word
0x8000d47c  lw   $v1, 0x0($a1)                           ; kernel's word
```

then, one field at a time (`lw` the caller's word again each time, `andi`
the field's own mask out of it, `and`/`or` it into the kernel word, `sw` the
kernel word back — repeated per field rather than assembled once):

| Bits | Mask |
| --- | --- |
| `0` | `0x1` |
| `2..1` | `0x6` |
| `3` | `0x8` |
| `4` | `0x10` |
| `12..5` | `0x1fe0` |
| `15..13` | `0xe000` |

— covering the low 16 bits of the word exactly (`0x1 \| 0x6 \| 0x8 \| 0x10 \|
0x1fe0 \| 0xe000 == 0xffff`), leaving the word's upper 16 bits and the
kernel's own copy's untouched bits alone. A final `lhu`/`sh` copies a
16-bit halfword at `addr+2` in the same direction, whole. `GetOsdConfigParam`
is the exact same code with the load/store operands swapped.

Neither routine deliberately computes a return value: `$v0` on exit is
whatever the last `lhu` happened to load — the caller's own halfword for
`SetOsdConfigParam`, the kernel's for `GetOsdConfigParam` — which is
incidental, not a documented result. SYS-1's `($a0) -> $v0` is therefore
technically satisfied (a value *is* in `$v0`) but not meaningfully so; a
rebuild matching the header's `void` return is behaviourally identical to a
caller that ignores it, which every SDK caller does by construction (the
header declares no return).

No GS or other device register is touched — `0x80022590` is a plain RAM
word, part of the same cluster of boot-configuration globals SetGsCrt reads
(`0x80022580`, `0x80022584`, `0x80022618`, all in §1).

## 4. `0x64`/`0x68` — `FlushCache(s32 operation)` / `iFlushCache(s32 operation)` `[header]`

`__NR_FlushCache = 0x64`; `iFlushCache` is `__NR_iFlushCache = (-0x68)`
`[header]` — called with the number negated, landing (per `docs/analysis/14`'s
negate-and-dispatch rule) on the **same** absolute slot `0x68`, which
`tools/eeksys.py <outdir>/KERNEL --slot 0x68` confirms shares `0x80002a40`
with `0x64`: one implementation serves both the interrupts-enabled and
interrupts-disabled entry points, unlike the rescheduling operations
`spec/05` SYS-2 covers.

`kernel.h`'s own comment names the four modes `[header]`:

```c
#define WRITEBACK_DCACHE  0
#define INVALIDATE_DCACHE 1
#define INVALIDATE_ICACHE 2
#define INVALIDATE_CACHE  3 // both
```

The reference's dispatch at `0x80002a40` matches this exactly:

```
0x80002a44  beq $zero, $a0, 0x80002ac0    ; operation == 0
0x80002a4c  beq $at,   $a0, 0x80002b00    ; operation == 1  ($at was set to 1)
0x80002a54  beq $v0,   $a0, 0x80002a80    ; operation == 2  ($v0 was set to 2)
              ; else (operation == 3, or anything else): call both sub-routines
0x80002a68  jal 0x80002b00                ; = the operation-1 body
0x80002a6c  jal 0x80002a80                ; = the operation-2 body
```

Three leaf routines, each a `cache` loop over both cache ways, `sync`
before and after every `cache` instruction:

| Target | `operation` | `cache` op | Range | Ways |
| --- | --- | --- | --- | --- |
| `0x80002a80` | `2` (`INVALIDATE_ICACHE`) | `0x7` | `0x2000` bytes, step `0x40` | `0`, `1` (2×`0x2000` = 16 KiB total) |
| `0x80002ac0` | `0` (`WRITEBACK_DCACHE`) | `0x14` | `0x1000` bytes, step `0x40` | `0`, `1` (2×`0x1000` = 8 KiB total) |
| `0x80002b00` | `1` (`INVALIDATE_DCACHE`) | `0x16` | `0x1000` bytes, step `0x40` | `0`, `1` (8 KiB total) |

`operation == 3` and every other unmatched value fall to the shared
"call both" path (`0x80002a5c`–`0x80002a7c`), which invokes the `0x1`
(dcache invalidate) body then the `2` (icache invalidate) body in that
order — matching `INVALIDATE_CACHE`'s doc comment exactly. `operation == 0`
(writeback-only) is **not** part of that combined path; nothing calls it
except a caller passing `0` directly.

No path in this function ever assigns `$v0` — a rebuild's `$v0` on return
here is simply whatever the caller's own `$v0` already held going in, since
the reference genuinely never touches it (stronger than §3's "incidental
last value": here it is not written *at all*), consistent with `kernel.h`'s
`void` return for both `FlushCache` and `iFlushCache` `[header]`.

## 5. `0x6f` — `GetOsdConfigParam2(void *config, s32 size, s32 offset)` `[header]`

`__NR_GetOsdConfigParam2 = 0x6f` `[header]`, declared `void` in `kernel.h` —
**the reference does not honour that.** `0x6e` (`SetOsdConfigParam2`,
`0x8000d350`, immediately adjacent, mirror-image write direction — not
individually specified here, out of the requested set, but its presence and
address confirm `0x6f`'s block boundary) copies bytes the other way with no
such write.

At `0x8000d2b8`:

```
$v0 = $a1 + $a2                       ; size + offset
if ($v0 < 0x81):                      ; in range, unsigned
    goto copy                          ;   (no clamp needed)
else:
    if ($a2 < 0x80): $a1 = 0x80 - $a2  ; clamp count to what's left in the block
    else:             $a2 = 0x80; $a1 = 0  ; offset already past the block: nothing to copy
copy:
    $a1 = $a1 + $a2                    ; end offset
    if ($a2 < $a1):                    ; count > 0
        for (i = 0; $a2+i < $a1; i++)
            config[i] = *(u8*)(0x80022598 + $a2 + i)   ; byte copy, kernel -> caller
```

— a 128-byte (`0x80`) kernel-resident block at `0x80022598`, `size`/`offset`
clamped to stay inside it, copied byte-by-byte into the caller's `config`.
This much matches the header's declared parameters and direction (`Get`).

**Then, regardless of whether any bytes were copied**, at `0x8000d320`:

```
$v1 = *(u64*)0x80015960                    ; the SAME persisted word SetGsCrt reads (§1)
$v0 = ($v1 << 26) >> 32) & 0x7             ; bits 8..6 -- the same extraction
                                            ; SetGsCrt's mode==1 case uses (§1a)
if ($v0 == 0):
    return 0
else:
    return ($v1 >> 44) & 0xf               ; bits 47..44
```

**This is a genuine, header-contradicting finding, checked directly against
the bytes**: the reference's `GetOsdConfigParam2` writes a real value to
`$v0` on every path — either `0` or a 4-bit field read out of the same
boot-configuration word `SetGsCrt` consults — despite `kernel.h` declaring
it `void`. It has nothing to do with the byte copy or the `config`/`size`/
`offset` arguments; it reads a completely unrelated global. `spec/05`'s
`0x6f: ($a0, $a1, $a2) -> $v0` already anticipated a return value from static
analysis; this pins down exactly what it is and where it comes from.

**Open question**: whether any real client relies on this value, or whether
it is simply left over from code shared with (or copy-pasted near) the
`SetGsCrt`/mode-persistence logic and never meant to be read.

## 6. `0x73` — `SetVSyncFlag(u32 *flagPtr, u64 *alarmPtr)` `[header]`

`__NR_SetVSyncFlag = 0x73` `[header]`, `void`, two pointer arguments — matches
`spec/05`'s `0x73: ($a0, $a1) -> -` exactly. The entire handler,
`0x80001588`–`0x8000159c`:

```
lui $at, 0x8002;  sw $a0, -0x717c($at)   ; *0x80028e84 = flagPtr
lui $at, 0x8002;  sw $a1, -0x7178($at)   ; *0x80028e88 = alarmPtr
jr  $ra
```

Both arguments are stored verbatim, as raw pointers, into two fixed kernel
words — no validation, no dereference, no return value, no hardware access
of any kind. That is the entire syscall.

The two stored pointers are consumed elsewhere: the routines immediately
following in the file (`0x800015a0` and `0x800015e8`, alongside a counter at
the third word of the same struct, `0x80028e80`) read them back, call a
helper at `0x80005af8`, and increment/decrement the counter — evidently the
vsync-time bookkeeping this flag/alarm pair exists to drive. That machinery
was not traced further here; it belongs to whichever interrupt path installs
the VBLANK handler (`docs/analysis/16`, `35`), not to slot `0x73` itself,
which only ever stores the two pointers.

## 7. Open questions, all of them

- `0x02` (SetGsCrt): the mode-0/1 "keep current" `movz`/`movn` instruction's
  exact effect; what the `0x80022580`/`0x80022584` forcing gate checks;
  every `?` OR-term in §1c–§1e beyond the two worked examples in §1b; the
  routine at `0x8000b290` that `mode == 0x72`/`0x73`/the VESA-ish
  range/`0x52` tail-call into; and whether SMODE2's fixed constants
  (`0x0007e5af1ecc80c8` and siblings) genuinely leave upper bits set on real
  silicon or whether that address's identity needs re-checking against
  hardware.
- `0x6f` (GetOsdConfigParam2): whether the header-contradicting `$v0` value
  (§5) is read by any real client, or is dead weight from shared code.
- `0x73` (SetVSyncFlag): the vsync-time consumer of the two stored pointers
  (`0x800015a0`, `0x800015e8`, helper `0x80005af8`) was located but not
  traced — relevant to whoever implements the VBLANK interrupt handler that
  presumably calls it.
- `0x4a`/`0x4b`: what the caller-visible bit layout at `addr`/`addr+2`
  actually configures (only the copy mechanism and bit masks are pinned
  here, not the semantics of any individual bit).
