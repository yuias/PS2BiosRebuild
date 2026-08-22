# EELOAD

`docs/project-state.md` §6 marks M1 met and M2 as "a retail title booting from
disc on PCSX2." That path starts with `EELOAD`. `spec/04` EE-9a and
`docs/analysis/15` §"The boot tail" already established that the EE kernel's
program-loading syscall (slot `0x06`/`0x7B`) does not load a program itself:
it stages `EELOAD`, an archive file `docs/analysis/01` left as "starts with
zero padding, format TBD", and hands it the caller's path and arguments.
`docs/implementation.md` records that our own kernel skips this stage and
loads programs directly ("There is no `EELOAD`"), which is faithful to the
*observable* contract but not to the mechanism — and PCSX2's fast boot and its
`-elf` path both hook `EELOAD` specifically, so a rebuild aiming at disc boot
needs the real stub, not just its outward behaviour. This reads `EELOAD`
itself and the kernel code that stages it.

```sh
python3 tools/romdir.py assets/SCPH-50000.bin --extract <outdir>
python3 tools/eeksys.py <outdir>/KERNEL --slot 0x06
python3 tools/eeksys.py <outdir>/KERNEL --slot 0x7b
python3 tools/eeksys.py <outdir>/KERNEL --slot 0x07
python3 tools/romdis.py <outdir>/KERNEL --cpu ee --vma 0x80000000 --range 0x80005598 0x800057dc
python3 tools/romdis.py <outdir>/KERNEL --cpu ee --vma 0x80000000 --range 0x800057e0 0x80005988
python3 tools/romdis.py <outdir>/KERNEL --cpu ee --vma 0x80000000 --range 0x8000d810 0x8000dcac
python3 tools/romdis.py <outdir>/EELOAD --cpu ee --vma 0x82000 --range 0x82000 0x83300
```

`EELOAD` and `KERNEL` are both byte-identical between `SCPH-50000.bin` and
`SCPH-70000.bin` (`cmp <outdir50>/EELOAD <outdir70>/EELOAD`), so every finding
below is model-independent for the two reference images this project tracks.

## 1. Format and load address

`EELOAD` carries no container. Its first eight bytes are zero, then real
R5900 code begins immediately (`lui $2,0x9`, `lui $3,0x9`, ...) — not the
"padding" `docs/analysis/01` flagged as format-TBD, just a leading zeroed
word or two before the first instruction; there is no ELF header (`\x7fELF`)
anywhere in the file and no length- or type-prefixed wrapper the way
`docs/analysis/04`'s IOP module format has one. This corrects `01`'s entry:
`EELOAD` is raw R5900 code, the same category as `KERNEL` (`docs/analysis/13`),
not a distinct or unknown format.

`EELOAD` always loads at a **fixed physical/KUSEG address, `0x00082000`**.
The constant is built explicitly in the kernel's slot-`0x06` handler
(`KERNEL 0x800056c4`/`0x800056d4`: `lui $18,0x8; ...; ori $18,$18,0x2000`) and used
both as the copy destination and as the value later written to `EPC` (§2).
`0x00082000` sits just above the boundary `docs/analysis/36`'s TLB table
already established — KUSEG's first `0x80000` bytes are unmapped, so a
program has to live above that line, and `EELOAD` sits `0x2000` bytes into
the mapped region, leaving `0x80000`-`0x81FFF` (8 KiB) below it that the
kernel evidently reserves for itself (§2's memory-clear boundary is drawn at
exactly this address, not at `0x80000`).

`EELOAD`'s own startup, read directly from its first instructions:

```sh
python3 tools/romdis.py <outdir>/EELOAD --cpu ee --vma 0x82000 --range 0x82000 0x82090
```

- **`0x82008`-`0x82030`**: a 16-byte-stride zero-fill loop from `0x00091200`
  to `0x000955A4` — `EELOAD`'s own `.bss`, bounds baked in as link-time
  constants, using the same quadword store `--cpu ee` cannot name
  (`docs/analysis/13`'s already-documented LLVM limitation; the loop shape —
  compare, `<unknown>`, `addiu` by `0x10` — matches the boot block's `KERNEL`
  copy loop exactly, just clearing instead of copying).
- **`0x82034`-`0x8205c`**: sets `$gp = 0x000991F0`, the standard MIPS
  gp-relative-addressing convention for a linked, non-ELF-wrapped binary —
  confirming `EELOAD` is a normally-linked SDK program with its container
  stripped, not hand-assembled code.
- **`0x82064`**: **syscall `0x3C`** — `(gp=0x991F0, stack=0x91200,
  stack_size=0x2000, args=0x93200, root=0x820B8)`, matching `spec/05` SYS-8a's
  signature exactly. `EELOAD` primes its *own* second execution context this
  way (`top = 0x91200+0x2000 = 0x93200`), with `root = 0x820B8` — an address
  inside `EELOAD`'s own code, a few instructions past this point. This is
  `EELOAD` self-priming, not something the kernel does for it.
- **`0x82080`**: **syscall `0x3D`** — `(start=0x955A4, size=0x2000)`, setting
  the heap-end record (SYS-8c) to `0x975A4`, i.e. `EELOAD`'s heap begins right
  above its own zeroed `.bss` and stack.
- **`0x82090`-`0x8209C`**: reads a word at `0x00093200` as `argc` and treats
  `0x00093204` as the base of an `argv` array, then calls `main` at `0x82388`.
  `0x93200` is exactly the `args` pointer just given to syscall `0x3C` — this
  is the packed argument block SYS-8b describes, and `EELOAD` reads it the
  same way any SDK program's runtime would (`docs/analysis/30`).

## 2. Slot `0x06`/`0x7B`: staging EELOAD

`docs/analysis/15` already found slot `0x7B` is `0x06` with the path pinned
to `"rom0:OSDSYS"` (`KERNEL 0x80005988`-`0x80005998`, four instructions, tail
`j 0x80005598`) and that slot `0x06`'s own body (`0x80005598`) holds the
string `"EELOAD"` in a saved register across its whole body. This section
reads that whole body.

**Packing the argument list.** At entry (`$a0`=path, `$a1`=argc, `$a2`=argv),
slot `0x06` builds a new argument list at the fixed buffer `0x800155C8`,
using the same append helper `docs/analysis/30` found slot `0x07` use for its
own `argv` packing (`0x80005558`, "append it, NUL included; returns the new
end"). The list built is **`{"EELOAD", path, argv[0..argc-1]}`** — `EELOAD`'s
own name first, then the caller's path, then the caller's original arguments,
verbatim (`0x800055ec`-`0x8000562c`). This is `EELOAD`'s own invocation
convention, and it means a rebuild's `EELOAD` stand-in must expect `argv[0] =
"EELOAD"`, `argv[1]` = the path to load, `argv[2..]` = the caller's own
arguments — not just the bare path.

**Locating the archive.** Slot `0x06` re-runs, at kernel runtime, the same
self-locating ROMDIR scan `docs/analysis/13` found in the boot block:

```sh
python3 tools/romdis.py <outdir>/KERNEL --cpu ee --vma 0x80000000 --range 0x8000d810 0x8000d8a8
```

`0x8000D810` matches the "RESET"-signature scan byte for byte (`lui
$8,0x4553; ori $8,$8,0x4552` = `0x45534552`, `addiu $10,zero,0x54` = `'T'`) —
the same constants `docs/analysis/13` found at `0xBFC00CE8` in the boot
block, now a **sixth** independent copy of the scan (after the boot block,
`IOPBOOT`, `MODLOAD`, `ROMDRV`, and the EE reset vector). Called with `($a0,
$a1) = (0xBFC00000, 0xBFC10000)` (`0x80005668`-`0x80005678`), it locates the
ROMDIR table; on failure it prints a diagnostic string at `KERNEL+0x5CE8` and
calls a panic-style routine at `0x80000D80` (`j 0x800059A0`, not decoded
further — §5). On success, `0x8000D8A8` resolves an entry **by name** inside
that table — the same by-name lookup any archive consumer needs — called
with the archive-table result and the string `"EELOAD"` (`KERNEL 0x80015D28`,
confirmed by exact offset match against `docs/analysis/15`'s own citation of
this constant). Failure here prints a second diagnostic naming the caller's
path (`KERNEL+0x5D08`) but does not otherwise diverge from the success path
in the traced range.

**The copy.** After the found entry is set up, and *after* the memory-clear
below, a word-copy loop (not the boot block's quadword copy) moves the file
in:

```
80005768:  lw  $3, 0x0($5)
8000576c:  addiu $4, $4, 0x4
80005770:  addiu $5, $5, 0x4
80005774:  slt $2, $4, $7
80005778:  sw  $3, 0x0($6)
8000577c:  bnez $2, 0x80005768
80005780:  addiu $6, $6, 0x4
```
`$6` is `$18 = 0x00082000` (the fixed destination, §1); `$7` is the found
entry's extent field (`entry+0xC` — a ROMDIR entry's size), read earlier from
the struct `0x8000D8A8` filled; `$5` is a source pointer computed earlier in
the function (not traced to the exact instruction — §5), plausibly the
archive's ROM base plus the entry's own offset. `$4` counts bytes, not words,
confirming `$7` is a byte count and the loop copies the whole `61840`-byte
file (`0xF190`, divisible by 4) into place four bytes at a time.

**Everything else torn down.** Before the copy, slot `0x06` walks the
256-entry, `0x4C`-byte-stride thread table (`0x800056D8`-`0x80005728`) killing
every thread but the current one — structurally identical to the teardown
`docs/analysis/30` found in slot `0x07`'s own operation function
(`0x80005850`-`0x80005898`, same table, same stride, same pair of calls
`0x80003EF8`/`0x80003DF8`). The `0x4C`-byte stride is new: neither `30` nor
`spec/05` SYS-10a state a thread record's size numerically; both slot `0x06`
and slot `0x3D` (`docs/analysis/36`'s own citation of `0x80005298`'s `mult
$v1,$v1,0x4c`) compute it the same way, pinning it at **76 bytes**.

**"Restart." and the memory clear.** After teardown, slot `0x06` clears the
reschedule flag (`0x800155B4`, matching `35`'s finding) and calls
`0x8000DBA0`:

```sh
python3 tools/romdis.py <outdir>/KERNEL --cpu ee --vma 0x80000000 --range 0x8000dba0 0x8000dcac
```

This prints `"# Restart.\n"` (`KERNEL 0x800162C8`), then re-does a *subset* of
hardware init — GS acknowledge, a partial INTC re-enable (`0x8000E618` with
mask `0xDFFD`, versus `0xFFFF` in the cold path below), DMAC/VU/VIF/GIF/IPU
reinit through the shared dispatcher `0x8000D9B8` (mask `0x7F`), FPU — and,
critically, calls **`0x8000E5C0` with `$a0 = 0x00082000`**. That function is a
16-byte-stride zero-fill loop from its argument up to a queried "end of
memory" (`jal 0x80000C40`, not decoded — §5) — the *same* class of loop
`§1` found `EELOAD`'s own `.bss` clear use. So **loading `EELOAD` clears all
of user memory from `0x00082000` upward**, immediately before the copy
overwrites the low end of that same range with `EELOAD`'s own bytes. It
prints `"# Restart Done.\n"` (`0x800162D8`) and returns.

**Entry.** `0x80005794`-`0x800057A8`: `mtc0 $18,$14,0` (**`EPC = 0x00082000`**),
`sync`, one more call (`jal 0x80001460`, not decoded — §5), `eret`. Control
reaches `EELOAD` by pointing `EPC` at it and returning from exception level,
exactly the mechanism `implementation.md`'s "There is no `EELOAD`" note
already inferred from the *other* direction ("by pointing EPC at the program
and letting the dispatcher's `eret` land there") — this is that code, read
directly.

**The cold-boot counterpart.** `0x8000DAD8` — called once, at kernel startup,
not from any syscall — is the same shape but prints
`"# Initialize Start."`/`"...Done."` (`0x800161F0`/`0x800162B0`, matching
`docs/analysis/15`'s console-log citation exactly) and clears memory from
`0x00080000`, the true base of mapped KUSEG, rather than `0x00082000`. The
`0x2000`-byte gap between the two is the reserved region noted in §1.

## 3. What EELOAD does

`main(argc, argv)` at `0x82388` walks `argv[1..]` for three command-line-style
flags — string prefixes stored in `EELOAD` itself, matched byte-for-byte by a
small helper at `0x82278`:

| Flag (at EELOAD-local address) | Selects |
| --- | --- |
| `"-m "` (`0x90780`) | `fno` **0** — load an IOP module |
| `"-k "` (`0x90788`) | `fno` **4** — `LoadStartKelfModule` |
| `"-x "` (`0x90790`) | `fno` **1** — load an EE ELF, via `0x82C40` |
| *(no flag; a plain path)* | `fno` **1** — load an EE ELF, via `0x82BF8` |

`argv[1]` (the path staged by slot `0x06`, §2) is additionally tested against
the literal strings `"moduleload"` and `"moduleload2"` (`0x90740`/`0x90750`)
before this loop runs; a match there was not traced to a distinct effect in
this pass (§5). The default case — a plain path with no recognised flag — is
the one that matters for the ordinary boot (`rom0:OSDSYS`, or a game's ELF):
it goes through `fno = 1`, confirming from the client side what
`docs/analysis/39` §4 could only infer from `LOADFILE`'s own binary
("`fno` 1's job is loading an EE-side ELF across SIF... `EELOAD`... the OSD's
own boot path"). Both the `-m` and `-k` handlers, and the two `fno = 1`
handlers, tail-call one of two shared RPC helpers, each of which first binds
`LOADFILE`:

```
0x00082630:  SifBindRpc(cd = 0x00093580, sid = 0x80000006, mode = 0)
```

retried in a bounded spin loop, structurally the same shape `docs/analysis/13`
found in `m1.dis`'s own `SifLoadFileInit` ("a retry loop (up to `0x1001`
attempts)"). `0x80000006` is built as a literal (`lui $5,0x8000; ori
$5,$5,0x6` at `0x82658`-`0x8265C`) — the well-known `LOADFILE_IRX` bind id
`docs/analysis/13` §3/§6 already pinned for `m1.dis`'s client, now confirmed
in the reference's own `EELOAD`.

**`fno = 0`/`4`** (module/KELF load) route through a shared helper at
`0x000828B0` whose reply buffer is 4 bytes (one word) — just the
`id_or_error`.

**`fno = 1`/`5`** route through a different shared helper, `0x000082AD0`,
whose reply buffer is **16 bytes (four words)** — matching
`docs/analysis/39` §4's own finding, from `LOADFILE`'s side, that `fno 1`/
`fno 5` reply with "two extra reply words" beyond the `{id_or_error, modres}`
pair every other `fno` uses. This is independent, client-side confirmation of
that reply shape. Two immediate-`fno` calls elsewhere in `EELOAD` use `fno =
2` and `fno = 3` directly — the raw memory peek/poke backdoor
`docs/analysis/39` §4 found in `LOADFILE`'s own dispatch table ("nothing in
this pass found any privilege check gating them") — confirming `EELOAD`
itself, not only debug tooling, is a legitimate client of that backdoor.

**A puzzling fifth call.** Immediately after the bind, before any of the
`-m`/`-k`/`-x`/default logic runs, `EELOAD` issues one more `SifCallRpc` to
the same bound server with **`fno = 6`** and a 4-byte reply
(`0x00082858`-`0x00082878`). `docs/analysis/39` §4 pinned `LOADFILE`'s own
range check at `fno < 6` — `6` is outside it, and that retail `LOADFILE`
would give this call no reply at all. `EELOAD`'s own error handling for this
call is non-fatal (it falls through with a negative return rather than
aborting), so the boot survives it either way, but what this call was meant
to accomplish is unresolved (§5).

**Entering the loaded program.** After a successful load, `0x000822B8`
performs the actual handoff:

```
822e0: lui $3,0x9 ; addiu $3,$3,-0xf0     # $3 = 0x0008FF10
822f0: lw $5,0x4($3)                       # $a1 = gp
822f4: lw $4,0x0($3)                       # $a0 = entry
82304: j 0x83230                           # tail-call the local syscall-0x07 stub
```
`0x00083230` is `EELOAD`'s own copy of the standard syscall-stub table
(`addiu $3,$zero,0x7; syscall; jr $ra`, one such stub for every syscall
number, the same shape `m1.dis` carries its own copy of). `$a2`/`$a3` are
`0x00822B8`'s own incoming arguments, passed straight through — the caller
supplies the target's `argc`/`argv`. This is `spec/05` SYS-8d, run by
`EELOAD` itself: `0x07(entry, gp, argc, argv)`, entered by tail-jump (`j`,
not `jal`) so nothing of `EELOAD`'s own frame survives the call — exactly the
"launcher's own registers at the `syscall`" contract SYS-8d describes.
`entry`/`gp` at `0x0008FF10`/`0x0008FF14` are written by `0x000082AD0`'s reply
handler (`sw $2,0x0($20)` / `sw $3,0x4($20)`, §4's helper); this pass
confirmed the write but not the exact chain from a wrapper's argument to
`$20 = 0x0008FF10` (§5) — the two fixed words being *entry then gp* is
inferred from field order and from `0x822B8` reading them in that order for
`$a0`/`$a1`, not from a labelled struct.

## 4. Strings, and who reads SYSTEM.CNF

`EELOAD`'s own strings are all plain, readable ASCII — no obfuscation, unlike
what §"OSDSYS" below found:

| String | EELOAD-local address | Role (from context) |
| --- | --- | --- |
| `"rom0:OSDSYS"` | `0x8E700` | matches slot `0x7B`'s pinned path (`15`) |
| `"rom0:TESTMODE"` | `0x8E710` | a second, unused-in-this-pass boot target |
| `"BootError"` / `"BootIllegal"` | `0x8E720` / `0x8E730` | referenced but not traced to a call site in this pass |
| `"moduleload"` / `"moduleload2 "` | `0x8E740` / `0x8E750` | tested against `argv[1]` before the flag loop (§3), effect not traced |
| `"rom0:UDNL rom0:EELOADCNF"` | `0x8E760` | a `printf`-style format string naming `EELOADCNF`, `EELOAD`'s own companion file — not traced to a call site |
| `"-m "` / `"-k "` / `"-x "` | `0x8E780`/`0x8E788`/`0x8E790` | the flag prefixes §3 decoded |
| `"all"` | `0x8E798` | the fallback path name the `-x`/default `fno = 1` helpers pass alongside a caller's argument (§3) |

None of these is `"SYSTEM.CNF"`, `"cdrom0:"`, or `"BOOT2"` — a direct search
for all three across the whole 61840-byte file (`grep -a`) finds nothing.
`EELOADCNF` itself (428 bytes) is not a text config in the shape of
`IOPBTCONF`; it opens with what is structurally a small `ROMDIR`/`EXTINFO`
table (`RESET`/`ROMDIR`/`EXTINFO`/`IOPBTCONF` entries) followed by a build
stamp (`"20030206-083917,eeload.conf,eeloadconf.bin,kuma@rom-server/..."`)
and then a literal copy of `IOPBTCONF`'s own module list — evidently a
build-time snapshot bundled under this name, not something `EELOAD` reads at
runtime in any form this pass could confirm (§5).

**`"cdrom0:\SYSTEM.CNF"` lives in `OSDSYS`, not `EELOAD`.**

```sh
strings -n 4 -t x <outdir>/OSDSYS | grep -i cdrom
```
```
49a1c cdrom0:\SYST
49a2b rEM.CNF;(kcan't open 'p
```
The bytes at `OSDSYS+0x49A1C` read `"cdrom0:\SYST"`, then three bytes that are
not printable ASCII (`01 00 0B`), then `"rEM.CNF;(k"` — an `'E'` reads as
`'r'` where a plain `"SYSTEM.CNF"` would need one, so this is **not** a
contiguous readable C string the way every `EELOAD` string above is; it reads
as encoded or table-packed data, decoded elsewhere in `OSDSYS`'s own code.
Decoding it was out of scope for this pass (§5), but its mere presence,
against its total absence anywhere in `EELOAD`, is the load-bearing fact:
**parsing `SYSTEM.CNF` for a disc boot is `OSDSYS`'s job, not `EELOAD`'s.**
`EELOAD`'s own contract is exactly what §3 found — load a path handed to it
and jump to it — with no device- or `SYSTEM.CNF`-specific logic of its own.
`OSDSYS` (the disc browser / menu the default boot enters, `docs/analysis/15`)
is what reads `cdrom0:\SYSTEM.CNF;1`, extracts the target ELF's path (the
well-known `BOOT2=` line, not directly observed in this pass), and is
presumably what then re-invokes slot `0x06`/`EELOAD` with *that* path — a
call this document did not trace but which is the natural reading of "OSDSYS
runs by default, `EELOAD` only ever loads whatever path it is given."

## 5. What this pins for the rebuild

- **`EELOAD` is raw R5900 code**, no container, loaded at the **fixed
  address `0x00082000`**, with its own `.bss`/heap bounds link-time-fixed
  relative to that address (§1).
- **Loading it (slot `0x06`) is: locate by name in the archive, tear down
  every other thread, clear user memory from `0x00082000` upward, copy the
  file's bytes in word by word, then enter by `EPC`** — a rebuild's slot
  `0x06`/`0x7B` needs all five steps in that order, not just the final entry.
  The `0x00082000` boundary (not `0x00080000`) is specifically the memory
  the reference is willing to destroy when staging a *fresh* `EELOAD`; the
  `0x2000` bytes below it are left alone.
- **`EELOAD`'s own invocation convention**: `argv = {"EELOAD", path,
  caller's original argv...}` — a rebuild's `EELOAD` stand-in, or a rebuild
  that keeps loading programs directly (`implementation.md`), needs to keep
  this shape in mind wherever it reproduces the *observable* contract, since
  it is what any real `EELOAD`-aware tooling (including PCSX2's own hooks)
  expects to see.
- **`EELOAD`'s SIF client**: binds `LOADFILE` (`sid = 0x80000006`), and for
  the ordinary case (no `-m`/`-k` flag) loads the target as an EE ELF via
  `fno = 1`, with a 4-word reply carrying at least `{id_or_error, modres}`
  plus `entry` and `gp` (this pass narrowed which two words are which by
  usage, not by a labelled reply struct — §"Unresolved" below). A rebuild's
  IOP-side `LOADFILE` needs `fno = 1`'s reply to carry a usable
  `entry`/`gp` pair for this handoff to work end to end.
- **The final handoff is `spec/05` SYS-8d**, run by `EELOAD` itself: tail-jump
  into the syscall-`0x07` stub with `(entry, gp, argc, argv)` in the argument
  registers it had at that point. A rebuild's `EELOAD` (if it builds one)
  needs nothing more than this to hand off correctly, given slot `0x07`'s
  existing, already-specified behaviour (`docs/analysis/30`).
- **The "Restart"/"Restart Without Memory Clear" distinction is about
  reloading `EELOAD` itself versus handing off to whatever `EELOAD` already
  loaded**: slot `0x06` (a fresh load, needs a clean slate above
  `0x00082000`) clears memory before copying in; slot `0x07` (entering a
  program `EELOAD` has *already* placed in memory) does not, because clearing
  would destroy what was just loaded. Both re-run a *subset* of the cold
  hardware init (GS/INTC/DMAC/VU/VIF/GIF/IPU/FPU), which a rebuild's own
  slot `0x06`/`0x07` should reproduce even where it does not reproduce the
  memory clear itself, since a program (the SDK runtime in particular) may
  depend on that hardware state being sane on entry.
- **`SYSTEM.CNF` parsing belongs to `OSDSYS`, not `EELOAD`**: a rebuild's
  `EELOAD`-equivalent needs no device- or config-specific logic; whatever
  reads `SYSTEM.CNF` for a disc boot is downstream of the default
  `rom0:OSDSYS` / `argv = {"BootBrowser"}` boot slot `0x7B` already reaches
  (`docs/analysis/15`), which this document did not trace further.

## 6. Unresolved

- **The exact source-pointer computation** for `EELOAD`'s copy (§2, the loop
  at `KERNEL 0x80005768`) — the destination and count are pinned, but the
  instruction that builds the ROM-side source address was not isolated in
  this pass.
- **`KERNEL 0x80000D80`** (`j 0x800059A0`, a panic-style routine reached when
  the archive table cannot be located) and **`0x80001460`** (called
  unconditionally right before slot `0x06`'s `eret`) — neither was decoded.
- **`KERNEL 0x80000C40`**, the "query end of memory" call the `0x00082000`-
  and `0x00080000`-rooted memory-clear routines both use to bound their
  loops, was not decoded to an instruction level, only recognised by role.
- **`KERNEL 0x80004968`/`0x80004280`/`0x80001460`/`0x80002ac0`/`0x80002a80`**
  and the several `printf`-adjacent helpers slot `0x06`'s tail calls
  (`0x8000E4F0` FPU reinit, `0x8000D9B8`'s per-unit init targets) were
  identified by role (from the "# Initialize ..."/"# Restart..." console
  strings they print immediately before or after) but not disassembled.
- **`EELOAD`'s `"moduleload"`/`"moduleload2"` test on `argv[1]`** (§3, §4) —
  the two literal-string comparisons were located and their inputs pinned,
  but the branch they feed was not traced to a distinct effect from the
  ordinary `-m`/`-k`/default paths in this pass.
- **The exact wiring from `0x000082AD0`'s reply write to the `entry`/`gp`
  cells at `0x0008FF10`/`0x0008FF14`** that `0x00822B8` reads for the final
  handoff (§3) — the write and the read were both located, and their values
  make sense as `entry` then `gp` by role, but the intermediate
  caller-supplied output pointer was not traced end to end.
- **`EELOAD`'s `fno = 6` call** (§3) — outside `LOADFILE`'s own documented
  `fno < 6` range (`docs/analysis/39` §4), issued unconditionally right after
  the bind, with no observed effect on the rest of the boot when it goes
  unanswered. Its purpose is unresolved; it may be a probe for a newer
  `LOADFILE` than this retail image ships.
- **`EELOADCNF`'s actual runtime use** (§4) — its content (a build-time
  `ROMDIR`/`EXTINFO`/`IOPBTCONF` snapshot) was read, but no code path in
  `EELOAD` reading `rom0:EELOADCNF` was located, despite the format string
  naming it.
- **`OSDSYS`'s encoded `"cdrom0:\SYSTEM.CNF"` reference** (§4) — its bytes
  were located and shown not to be a plain C string, but the encoding and the
  code that decodes it were not examined; likewise the `BOOT2=` parsing and
  the re-invocation of slot `0x06` with the disc's target path, which this
  document infers must exist but did not trace into `OSDSYS`'s own code.
- **`"BootError"`/`"BootIllegal"`/`"rom0:TESTMODE"`** (§4) — located as
  strings inside `EELOAD` but not traced to their call sites.
