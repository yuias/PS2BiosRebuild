# The IOP Reboot Protocol

A retail title reboots the IOP once, mid-boot, to bring up a fresher module
set than `rom0:`'s own `IOPBTCONF` carries — a mechanism `docs/analysis/12`
already named (`REBOOT` "additionally imports `modload`, which matches the
reboot strings `10` found in `MODLOAD`") but did not trace, and
`docs/analysis/34` left the RESET_CMD handler itself as "not traced"
(`SifCmdResetData_t{header,arglen,mode,arg[80]}` was only a header-derived
shape). `docs/analysis/41` found the same mechanism named, but unreferenced,
inside `EELOAD` — the string `"rom0:UDNL rom0:EELOADCNF"`. This document reads
`REBOOT`, `UDNL` and `MODLOAD`'s reboot core directly, to settle the wire
protocol, the argument grammar, the merge rule and the hand-over precisely
enough to rebuild a `REBOOT`/`UDNL` pair and, on the EE side, whatever calls
`sceSifIopReset`.

```sh
python3 tools/romdir.py assets/SCPH-50000.bin --extract <outdir>   # outside the repo
for m in REBOOT UDNL MODLOAD IGREETING ADDDRV EELOAD; do
  python3 tools/irxinfo.py <outdir>/$m --imports
done
python3 tools/irxinfo.py <outdir>/REBOOT --dump-load <outdir>/REBOOT.text
python3 tools/romdis.py <outdir>/REBOOT.text --cpu iop --vma 0 --range 0x0 0x1e0
python3 tools/irxinfo.py <outdir>/UDNL --dump-load <outdir>/UDNL.text
python3 tools/romdis.py <outdir>/UDNL.text --cpu iop --vma 0 --range 0x16c 0x4c8
python3 tools/irxinfo.py <outdir>/MODLOAD --dump-load <outdir>/MODLOAD.text
python3 tools/romdis.py <outdir>/MODLOAD.text --cpu iop --vma 0 --range 0x1508 0x1504
```

One outside lead is used here, in the pattern `docs/analysis/24` set: a
sibling project of the same author's, `PS2e/docs/hw-notes.md`, not part of
this repository, contributed a black-box observation of the same retail
sequence (its own emulator's console/DMA trace, not disassembly). It is
marked wherever used, and this document says explicitly where its own
disassembly could and could not confirm it.

## 1. The wire protocol: `SIF_CMD_RESET_CMD`

`docs/analysis/34` §2 pinned the reserved command range as
`SIF_CMD_ID_SYSTEM | n`, `n = 3` for `RESET_CMD` — `cid = 0x80000003` — but
left its handler and body unread. `REBOOT`'s own code supplies both.

**REBOOT+0xc8 reads the packet.** This is the function registered as the
system-command callback (§2), and it is what runs directly in the SIF
dispatch path when a `cid = 0x80000003` packet arrives:

```
0xc8..0xd0:  $4 = the incoming packet
0xd0: lw $2, 0x10($4)              # arglen
0xd8: blez $2, +copy skipped
0xe4: lbu $3, 0x18($2)             # source byte i, from packet+0x18+i
0xec: sb  $3, 0x8($2)              # dest byte i, into a local buffer+8+i
      (loop while i < arglen)
0x104: lw $2, 0x14($4)             # mode
0x10c: sw $2, 0x4($5)              # dest+4 = mode
0x110: jal thevent.7($4=evflag, $5=0x400)   # SetEventFlag
```

`+0x10` sits right after a 16-byte header, `+0x14` right after that, `+0x18`
after that — which **confirms `SifCmdResetData_t`'s field layout by offset,
byte for byte**, not merely by header comment:

```
struct SifCmdResetData_t {
    SifCmdHeader_t header;  // +0x00, 16 bytes (docs/analysis/34 §2)
    u32 arglen;              // +0x10
    u32 mode;                 // +0x14
    char arg[80];               // +0x18
};
```

The handler does nothing but copy `arg[0..arglen)` and `mode` into a local
buffer and post an event flag — it runs in dispatch context and cannot
block, so it hands the real work to a thread (below).

**The RESET_CMD reboot argument is one blob, tokenised later, not by
`REBOOT`.** `arg[80]` carries the whole string —
`"rom0:UDNL rom0:EELOADCNF"` or, for a title's own reboot,
`"rom0:UDNL cdrom0:\MODULES\IOPRP310.IMG;1"` — and `REBOOT` passes it on
**verbatim** to `MODLOAD` (§2). By the time `UDNL`'s own entry point sees it,
it is already a conventional `argc`/`argv` (`UDNL`'s prologue saves `$a0` as
`argc` and `$a1` as `argv` directly), with `argv[0]` its own invocation path
and `argv[1..]` the rest of the string split on whitespace. **Where that
split happens was not isolated in this pass** — it is somewhere inside
`MODLOAD`'s syscall-12 trap or the function it calls (§2) — flagged in
Open Questions.

**Registration, not `SIFCMD`'s own entry.** `docs/analysis/34` §5 left "exact
writer of `SIF_CMD_RESET_CMD`'s handler... not located" as unresolved. It is
`REBOOT`'s own module-init code, not `SIFCMD`:

```
REBOOT+0x160: $16 = 0x3f0                       # see the caution below
REBOOT+0x168: jal sifcmd.4 ; sw $2, 0x0($16)     # result stashed at $16+0
REBOOT+0x170: $4 = 0x80000003                     # lui $4,0x8000; ori $4,$4,3
REBOOT+0x178: $5 = &REBOOT+0xc8                    # the handler above
REBOOT+0x180: jal sifcmd.10 ($4=cid, $6=$16)        # == sceSifAddCmdHandler
```

`sifcmd` ordinal 10 is called with `(cid, handler, harg)` — the same
`{handler, harg}` shape `docs/analysis/34` already found `sceSifAddCmdHandler`
uses for the dispatch tables — so **`sceSifAddCmdHandler(0x80000003,
REBOOT+0xc8, harg)`** is what makes `0x80000003` live. This runs once, from a
worker thread `REBOOT`'s module entry creates and starts (`thbase.4` =
CreateThread with `entry = REBOOT+0x128`, `thbase.6` = StartThread), not from
`REBOOT`'s residency-rule entry point itself — the entry point's only job is
to spawn that thread and return.

**Caution — a coincidental address, not BOOT-8's table.** `REBOOT+0x160`
builds `$16 = 0x3f0` as a plain link-time constant
(`lui $16,0; addiu $16,$16,0x3f0`). `spec/03` BOOT-8 also pins an absolute
IOP address `0x3F0` — the boot-parameter-table pointer `EECONF`/`SIFINIT`/
`SIFCMD`/`IGREETING` all consult. **These are almost certainly not the same
cell.** The boot block and `IOPBOOT` run in place at fixed ROM addresses, so
a literal `0x3F0` there is the real absolute address; `REBOOT` is a
relocated IRX (`spec/02` IRX-2/IRX-9's link-then-relocate model), so the same
bit pattern most likely resolves, after relocation, to `REBOOT`'s own load
address plus `0x3f0` — an offset into `REBOOT`'s own `.data`/`.bss`
(`sizes ... bss 0x60` from `irxinfo`), not the shared table. This was not
independently confirmed either way in this pass (it would need the actual
relocated load address, which needs `MODLOAD`'s or `LOADCORE`'s own
relocation step observed at run time) — flagged in Open Questions, and worth
remembering before assuming any reboot code touches BOOT-8's table.

**The event flag, and when "Get Reboot Request From EE" prints.** After
registering the handler, the same worker thread calls `thevent.10` —
`WaitEventFlag(evflag, 0x400, mode)` — on the same flag id the `0xc8` handler
signals with `SetEventFlag(evflag, 0x400)`. So the thread **blocks** right
after registering, and only after an actual `RESET_CMD` packet has arrived
and been unpacked does it print `"Get Reboot Request From EE"` (the string
sits at `REBOOT+0x3bc`, printed right after the wait returns) — the string
names a real event, not a readiness banner. `"Reboot service module.
(99/11/10)"` (`REBOOT+0x398`) prints earlier, unconditionally, right after
the thread starts (and, conditionally, a re-`sceSifInit` — `sifman.5` — if
`sifman.29`'s check says the SIF is not already up).

**SIF flags and the DMA channel are silenced, not merely handed off.**
Immediately after the wait returns, before calling into `MODLOAD`:

```
REBOOT+0x1ac: jal sifman.22 ($4 = 0x00020000)    # SIF_STAT_CMDINIT's bit value
REBOOT+0x1c4: sw $zero, 0xBF801538                # SIF1 CHCR (IOP side, KSEG1), = 0
```

`0xBF801538` is exactly the SIF1 (EE→IOP) channel's IOP-side `CHCR` mirror
`docs/analysis/34` §1 already identified (`"0xbf801528/0xbf801538 ... CHCR of
chan 9 / chan 10"`). Zeroing it stops the receiving channel outright, before
any teardown starts. This is a direct, disassembly-level confirmation of the
outside lead already carried in `docs/project-state.md` §4 ("in-flight FIFO
state must be discarded") and restated more sharply in `PS2e/docs/hw-notes.md`
(a sibling project of the same author's, not part of this repository): *"the
IOP reboots via UDNL without going through the ROM reset stub (no POST
codes). In-flight SIF FIFO state must be discarded at that point or the new
kernel's `sifcmd` handshake parses stale garbage and EELOAD retries
forever."* `sifman.22`'s exact target register was not independently
confirmed (only the argument value, `0x00020000`, matching
`SIF_STAT_CMDINIT`'s bit); if it does write `SMFLG`, BOOT-10c's asymmetric
rule (`spec/03`) means an IOP write can only *set* bits there, so this would
be REBOOT *announcing* something rather than clearing it — the clearing, if
any, is unresolved and may simply not exist (the CHCR write may be doing all
the work the outside lead describes).

**What was not found: no `0x80000003` construction anywhere in `EELOAD`.**
A raw byte scan of `EELOAD`'s full 61,840 bytes for the pattern
`03 00 00 80` (little-endian `0x80000003`), and a scan of every
`lui $x,0x8000` in its disassembly for a following `ori $x,$x,0x3`, found
**no** construction of the RESET_CMD cid anywhere in the file — nor any
reference to the `"rom0:UDNL rom0:EELOADCNF"` string at `0x8E760`
(`docs/analysis/41` §4 already noted the string is "not traced to a call
site"; this independently confirms it with two different search strategies).
Yet `PS2e/docs/hw-notes.md` (same attribution as above) reports observing
exactly this call, twice, roughly 0.7 s apart, in the middle of a boot chain
it traced as `OSDSYS ExecutePs2GameDisk → LoadExecPS2 → EELOAD (0x82000,
entered via eret) → chains rom0:PS2LOGO → LoadExecPS2(cdrom0:ELF) → EELOAD
resets the IOP with "rom0:UDNL rom0:EELOADCNF" → sceCdDiskReady →
sceCdSearchFile/sceCdRead for the ELF`. Read together: the reset happens on
the **second** entry into the `0x06`/`EELOAD`-staging path of that chain (the
one loading the disc's own ELF, after `PS2LOGO` has already run), and
`EELOAD` itself — the file this document's byte search covered completely —
contains no code that could issue it. The caller is most likely `PS2LOGO`
(chained through the same `LoadExecPS2`/syscall-`0x06` path,
`docs/analysis/41` §2/§3), not `EELOAD`'s own `main`. `PS2LOGO`'s own code was
not read in this pass (out of budget here, and its animation payload is
off-limits under `docs/clean-room-policy.md` §3 regardless) — left open.

**What is inferred, not observed, on the EE client side.** No SDK-built EE
binary in this ROM or in `M1`'s own program calls `sceSifIopReset` (the M1
program "opts out of that weak hook", `docs/analysis/34` §1), so nothing here
disassembles the client's own reset/resync code. Structurally, though, three
things must follow from what **is** confirmed:
- `SIF_SYSREG_MAINADDR`/`SUBADDR`/`RPCINIT` (software registers 0/1/2,
  `docs/analysis/34` §1/§6) live entirely in the EE's own RAM — the IOP never
  touches them — so "resetting" them is pure EE-side bookkeeping: zeroing the
  cells `sceSifSetReg` writes, which forces the next `sceSifInitRpc` to
  renegotiate `INIT_CMD`/`SET_SREG` (`spec/03` BOOT-12c) from scratch rather
  than short-circuiting at its "already active" check (`docs/analysis/34` §1
  step 4).
- Since the IOP's `SIFCMD` command layer and the SIF1 channel are explicitly
  silenced by `REBOOT` before teardown (above), any client waiting on a
  pending command at that moment cannot be answered — a correctly-behaved
  `sceSifIopSync` must poll something that only becomes true again once the
  *new* kernel's `SIFMAN`/`SIFCMD` have re-run their own BOOT-10/BOOT-12b
  bring-up, which re-raises `SIF_STAT_SIFINIT` (`0x10000`) in `SMFLG` exactly
  as it did at cold boot (`docs/analysis/34` §1 step 5) — the same bit
  `REBOOT`'s own thread's conditional `sifman.5` call would also re-trigger
  if the merged kernel calls it again. No distinct `SIF_STAT_BOOTEND`-style
  bit was found anywhere in this pass (no ps2sdk header was available to
  confirm or refute one exists) — flagged in Open Questions rather than
  assumed.
- The three-tier picture — RESET_CMD sent, `SifIopSync` waiting on
  `SIF_STAT_SIFINIT` reappearing, `SifInitRpc` renegotiating — accounts for
  the whole observable shape without needing a fourth mechanism, but it is a
  reconstruction from the IOP-side evidence above, not a read of EE-side
  code, and should be verified the moment an SDK client that actually calls
  `sceSifIopReset` is available to trace (a natural M2 follow-up, `tests/m1`
  or `OSDSYS`'s own boot path once decoded further).

## 2. The IOP side: teardown, merge, and hand-over

**`modload.4` is a syscall trap, not a normal export.** `REBOOT` calls it
with `($4 = arg_ptr, $5 = mode)`, and its body is five instructions:
build a fixed `"modload"` tag string, `syscall` number `0xC` (12), return.
**The IOP kernel's syscall 12 is the reboot entry point** — where that slot's
table entry is installed (presumably by `MODLOAD` at its own module-init
time) was not traced in this pass.

**The real teardown/reload function**, `MODLOAD+0x12e0`..`0x1504` (called
`(tag, arg, mode)` by shape; its own parameters land in `$21`/`$20`), does,
in order:

1. **Conditional print, gated by `mode`'s top bit.** If `mode & 0x80000000`,
   prints `" ReBootStart: Terminate resident Libraries"`
   (`docs/analysis/03`/`10` already found this string; this pins exactly what
   controls it).
2. **Walk and tear down every resident module**, but only in that same
   `mode`-gated branch: `loadcore.<ord>` returns the head of the resident
   module list, and each entry with a non-null pointer at `+8` is called as
   `fn(0)` — a generic per-module teardown hook distinct from, and coarser
   than, the ordinary non-residency teardown `spec/02` IRX-12b describes
   (which runs *inside* a module's own entry return, not from an external
   walk). **A rebuild's reboot path must expose this walk-and-call hook on
   every module it wants reboot-capable**, not just implement IRX-12b.
3. **Re-apply BOOT-4 step 1's bus/RAM-controller table, without the reset
   vector.** `MODLOAD` re-reads `CP0 $15` (`PRId`) and re-evaluates BOOT-3's
   exact discriminator (`PRId < 0x10 || *(u32*)0xBF801450 & 8`), then reads
   two fixed-offset word pairs straight out of the boot block itself
   (`lw $4, 0xBFC02000+{0x8,0xc}` or `+{0x10,0x14}` depending on the
   discriminator) and applies them through what is, by shape, the same
   "walk `(address,value)` pairs until a zero address" routine BOOT-4 step 1
   already describes. **This is the mechanism behind the outside lead's "no
   POST codes"**: the update reboot re-runs a subset of BOOT-4's own table
   application in software, in place, without re-entering `0xBFC02000` at
   all — so none of BOOT-5's POST writes fire a second time, and a rebuild's
   software reboot path must call the same table-apply logic directly rather
   than re-triggering the reset vector.
4. **Re-run the self-locating ROMDIR scan** against the fixed ROM window
   (`$4=0xBFC00000, $5=0xBFC10000`) — the **eighth** independent copy of this
   scan across the image (after the boot block, `IOPBOOT`, `MODLOAD`'s own
   module-loading use, `ROMDRV`, the EE reset vector, `EELOAD`, and `UDNL`
   itself, per docs/analysis/11 §"four times over", `41` §2, and §"UDNL"
   below — the count keeps growing every time another file is read).
5. **An indirect call**, `jalr $16`, with four integer arguments, right after
   the scan. By calling convention this is almost certainly the transfer
   into `UDNL`'s own entry — `UDNL`'s signature is exactly the four-argument
   `entry(argc, argv, 0, module_record)` shape — but the actual argument
   values (a shifted size/address in `$4`, and values in `$5`/`$6` that did
   not resolve cleanly to `argc`/`argv`) were not pinned in this pass.
   **Flagged as unresolved rather than asserted.**

### `UDNL`: the merge core

Entered as `entry(argc, argv, 0, module_record)` — `argv[0]` is `UDNL`'s own
invocation path (e.g. `"rom0:UDNL"`), `argv[1..]` the rest of the RESET_CMD
argument string, already tokenised by the time `UDNL` sees it (§1).

**Every named source is opened, and rom0 is always one of them, unnamed.**
For each `argv[i]`, `i ≥ 1`: `ioman.4` opens it, `ioman.6` (`lseek`) measures
its size; failure panics `"file '%s' can't open"` and stops. Independently of
what `argv` names, `UDNL` **always** re-runs the ROM-window self-locating
scan (`$4=0xBFC00000, $5=0xBFC10000`) exactly like the boot block/`IOPBOOT`
— so `rom0`'s own archive is unconditionally in the pool of sources `UDNL`
can resolve names against, whether or not anything in `argv` names it.

**`IOPBTCONF` is resolved by name across that pool, then parsed with a
richer grammar than `IOPBOOT`'s.** A helper walks every opened source plus
the rom0 scan looking for an entry literally named `IOPBTCONF`, panicking
`"panic ! '%s' not found"` only if **none** of them has one — which, given
rom0's scan is unconditional, can only happen if rom0's own archive is
somehow missing its `IOPBTCONF` (not this project's scenario: the retail
title's `cdrom0:\MODULES\IOPRP310.IMG` lacks one, but rom0 always supplies
one, so the lookup succeeds using rom0's copy). The resolved file is then
tokenised:

| Leading byte | Effect |
| --- | --- |
| `@` (hex) | base address, same as `IOPBOOT`'s grammar (`spec/03` BOOT-9) |
| `"!addr "` | parses hex, stores it into a **different** field than `@` does — not merely an unused synonym, unlike `IOPBOOT`'s parser (`spec/03` BOOT-9b), which never implements it at all |
| `"!include "` | parses hex, appends it to the target list as a **tagged** (low bit set) entry, distinct from a plain module-name pointer — the consumer of that tag was not traced (Open Questions) |
| anything else | a module name, resolved as below |

**The merge rule: highest `.iopmod` version wins, across every source.** Name
resolution (`UDNL`'s `0x878` helper) does **not** prefer the caller's archive
over rom0, or vice versa — it walks **every** opened source plus the rom0
scan result, and for every entry matching the token's name, reads its
version half-word and keeps the highest. Only after the scan does it
re-resolve the winning entry to copy its data in.

This is the precise mechanism behind the observed retail merge. Rom0's own
`IOPBTCONF` supplies the **order** (all 29 names, since it is the only
`IOPBTCONF` in the pool when the caller's archive has none) — `BOOT-9c`'s
"load order is not storage order" applies here exactly as it does at cold
boot. Then, name by name:

- The 16 names the title's `IOPRP310.IMG` also carries (`SYSMEM LOADCORE
  SIFCMD SIFMAN THREADMAN IOMAN MODLOAD FILEIO CDVDMAN CDVDFSV LOADFILE
  TIMEMANI ROMDRV EESYNC SYSCLIB STDIO`) resolve to whichever copy has the
  higher version — in practice the disc's, since a shipped title's own
  module build is newer than a several-year-old retail BIOS's. This is
  confirmed independently: the disc's own `THREADMAN` (in the extracted `<outdir>`
  copy of `IOPRP310.IMG`, not part of this ROM) carries the literal string
  `"IOP Realtime Kernel Ver. 2.2"`, which is absent everywhere in
  `assets/SCPH-50000.bin` (checked with `strings` across the whole 4 MiB
  image); rom0's own `THREADMAN` carries `"IOP Realtime Kernel Ver.0.9.1"`
  instead (`0x3593a` in the reference image). The console banner switching
  between these two exact strings is not a coincidence of wording — it is
  this merge rule picking the newer file.
- The other 13 names rom0 alone supplies (`EXCEPMAN INTRMANP INTRMANI
  SSBUSC DMACMAN TIMEMANP HEAPLIB EECONF VBLANK IGREETING REBOOT SIFINIT
  SECRMAN`) have exactly one candidate each and load from rom0 unchanged —
  which is also why `REBOOT` itself (never present on a title's disc)
  survives every reboot as the *same*, never-reloaded resident service and
  its worker thread.

**`"ROM directory not found"` remains unexplained by this pass.** It is
`ADDDRV`'s only string, and `ADDDRV` is the only one of the 91 extracted
files that carries it. Neither `UDNL` nor `REBOOT` nor `MODLOAD`'s reboot
core references the literal name `"ADDDRV"` anywhere in their bytes (checked
by direct search), yet the observed retail console trace for the title's own
reboot prints exactly this string, right after `"Get Reboot Request From
EE"`. `docs/analysis/10` only ever placed `ADDDRV` in `OSDCNF`'s alternate
boot list. How it enters a plain `sceSifIopReset` reboot at all is open —
possibly `MODLOAD`'s `0x12e0` function reaches it before handing off to
`UDNL` (the unresolved `jalr $16` step, §"An indirect call" above, was not
traced past its own address computation), possibly by archive **position**
rather than by name. Left in Open Questions rather than guessed.

**Staging.** `UDNL` sums every resolved module's size (seeded at a fixed
`0x420`-byte header allowance) and asks `sysmem` for one contiguous buffer of
that total; a `0x20`-byte sub-header at its base is zeroed before staging
begins. The per-module byte-copy loop that actually fills that buffer was
not reached in this pass (budget ran out disassembling the merge-rule logic
first) — flagged in Open Questions. The final hand-over into the freshly
staged kernel (an entry address, a jump or `eret`, and whatever the IOP-side
counterpart of `spec/03` BOOT-9's boot-parameter table becomes for the new
kernel core) was likewise not reached.

**`0x1F8100` was not located.** Nothing found in this pass — not `IOPBOOT`'s
own stack/struct setup (which computes a stack top from `RAMsize << 20` and a
small fixed-offset struct near `0x20000`, neither matching this address), not
`REBOOT`, not `UDNL`'s partial trace, not the outside lead — pins an argument
block at IOP address `0x1F8100`. This is left as an open question rather
than invented; whether the merged kernel core rebuilds the same
address-`0x3F0`-rooted boot-parameter table BOOT-8 describes, or an entirely
separate structure, is unresolved.

### `IGREETING`'s banner pins the console-message sequence

`docs/analysis/12` already found `IGREETING` consults boot record key 4 and
prints "the boot banner and a diagnostic line," without reading which value
selects which text. It resolves the whole observed message sequence
directly:

```
IGREETING+0x10: jal loadcore.12 ($4 = 4)     # boot record, key 4
IGREETING+0x30: lhu $3, 0x0($16)              # the record's selector word
```

Branching on that word: `0` prints `"Hard reset boot"`, `1` prints `"Soft
reboot"`, `2` prints `"Update reboot complete"`, `3` prints `"Update
rebooting.."` — each read directly at its own file offset in the extracted
`IGREETING` (`0xa50`/`0xa60`/`0xa6c`/`0xa84`). Every one of those four is
printed **in addition to**, right
before, a generic line built from `"\nPlayStation 2 ======== "` plus a
`%04x-%04x`/`%x`/`%lx, %ldMB` CPU-identification format string, unconditional
regardless of the selector's value (or its absence — a null boot record
falls through to the generic line alone). This is exactly the observed
`"PlayStation 2 ======== \nUpdate rebooting.."` pairing from the task's own
boot trace, now pinned to a specific record value (`3`) rather than read off
a console transcript.

**`IGREETING` reloads on every reboot, since it is not one of the 16 names a
disc's `IOPRP*.IMG` supplies** (§2's merge rule), so it prints this banner
fresh each time rom0's copy is picked up again — once at the very first cold
boot (where the record must read `0`, "Hard reset boot," matching a
power-on), and again on every software reboot. **Who writes the selector
value into the boot record before each reload — presumably `REBOOT` or
`UDNL`, setting it to `3` before triggering `MODLOAD`'s teardown — was not
traced to a specific write in this pass**, since it depends on resolving
whether `REBOOT+0x160`'s `$16` is BOOT-8's shared table cell or `REBOOT`'s
own private data (the caution above). Nor was anywhere a write of `2`
(`"Update reboot complete"`) found, so what would make it observable — a
*third* run of `IGREETING`, or a different code path entirely — is open.

## 3. What differs for `EELOAD`'s own reboot

The mechanism is identical — the same `SIF_CMD_RESET_CMD`, the same `REBOOT`/
`MODLOAD`/`UDNL` chain, the same highest-version-wins merge. What differs is
only the **argument**: `"rom0:UDNL rom0:EELOADCNF"` names a nested archive
(`EELOADCNF`, `docs/analysis/10` §"Boot configurations are nested archives")
rather than a disc path. `EELOADCNF`'s own `IOPBTCONF` (`docs/analysis/10`)
differs from rom0's by exactly two substitutions — `LOADFILE → XLOADFILE`,
`CDVDMAN → XCDVDMAN` — both disc-facing drivers, which is exactly what a
subsequent `cdrom0:` read needs and nothing else does; every other name is
identical to rom0's, so under `UDNL`'s version-wins rule those two resolve to
`EELOADCNF`'s copies (presumably built with higher version numbers than
rom0's, the same mechanism the title's own reboot exercises with 16 names
instead of 2) and everything else keeps rom0's modules untouched. Note that
`EELOADCNF`'s own `IOPBTCONF` entry, per `docs/analysis/10`'s byte diff, is
the **same length** as rom0's and substitutes by position — consistent with
"same 29-name order, different file behind two of the names" rather than a
shorter, hand-picked list.

Whether it is `EELOAD` itself, or `PS2LOGO` chained through the same
syscall-`0x06` path, that actually issues this reset is the open question §1
already states plainly: `EELOAD`'s own 61,840 bytes contain no construction
of `cid = 0x80000003` and no reference to the format string that names this
exact reboot, despite `docs/analysis/41` finding the string present and
`PS2e/docs/hw-notes.md` (same attribution as §1) observing the call happen
twice in the middle of a chain that re-enters the `0x06` path a second time
for `PS2LOGO`. Given `EELOADCNF`'s substitutions are disc-driver-specific,
and the outside lead places the reset immediately before `sceCdDiskReady`,
the most likely caller is whatever runs between `PS2LOGO`'s hand-back and the
disc's own ELF load — which this pass did not disassemble.

**What an emulator needs**, distilled from §1 and §2 together:
- The RESET_CMD packet's exact field offsets (§1), so a rebuild's `REBOOT`
  parses the same argument string an SDK client sends.
- The IOP-side SIF1 channel (`CHCR` at `0xBF801538`) explicitly silenced
  before teardown starts (§1) — an emulator's DMA state must tolerate being
  zeroed mid-reboot without treating it as a fault, and a rebuild's `REBOOT`
  must do the same zeroing or risk the stale-FIFO failure the outside lead
  describes.
- No re-entry of the reset vector (`0xBFC02000`) at all — an emulator's own
  BIOS-boot detection (if it keys off the reset vector, as PCSX2's fast-boot
  hook does for the EE side per `docs/analysis/25`) must not expect a second
  hardware reset here; the update reboot is entirely software, driven by
  `MODLOAD`'s syscall-12 handler re-applying a bus-config table and jumping
  into `UDNL` (§2).
- On the EE side, nothing beyond re-running `sceSifInitRpc`'s existing
  `INIT_CMD`/`SET_SREG` exchange (`spec/03` BOOT-12c, already built) should
  be needed once the merged kernel re-raises `SIF_STAT_SIFINIT` — no new EE
  kernel syscall or SIF slot is implicated by anything found in this pass.

## What this pins for the rebuild

- `SifCmdResetData_t` is confirmed byte-for-byte:
  `{header:16, arglen@0x10:4, mode@0x14:4, arg[80]@0x18}`.
- `REBOOT`'s only job at its own module entry is spawning one worker thread;
  that thread registers `sceSifAddCmdHandler(0x80000003, handler, harg)`,
  then blocks on an event flag the handler signals per packet — a rebuild's
  `REBOOT` needs the same split between a non-blocking dispatch-context
  handler and a blocking worker thread, not one function doing both.
- The reboot silences the IOP-side SIF1 `CHCR` (`0xBF801538 = 0`) before
  handing off — a rebuild must do this, and an emulator target must tolerate
  it.
- `modload`'s reboot entry is IOP kernel **syscall 12**, not an ordinary
  export call.
- The teardown-and-reload core (`MODLOAD+0x12e0`) re-applies BOOT-4 step 1's
  bus-config table **without** re-entering the reset vector — "no POST
  codes" is a direct consequence of this, not a separate fact to reproduce.
- `UDNL`'s merge rule is **highest-`.iopmod`-version wins per name**, scanned
  across every source it is handed **plus** an always-present, unconditional
  scan of rom0's own archive — not "archive overrides rom0" or "rom0 is a
  fallback." The load **order** always comes from whichever source supplies
  an `IOPBTCONF` (rom0's, when the caller's archive has none, as in this
  project's disc-boot scenario).
- `UDNL`'s own `IOPBTCONF` grammar is a strict superset of `IOPBOOT`'s
  (`spec/03` BOOT-9): it actually implements `"!addr "`, and adds
  `"!include "`, neither of which `IOPBOOT` recognises.
- `EELOAD`'s and a title's own reboot are the **same mechanism** with a
  different argument string; nothing EE-kernel-specific is implicated.
- `IGREETING`'s boot-record key 4 selects the exact console banner
  (`0`=Hard reset boot, `1`=Soft reboot, `2`=Update reboot complete,
  `3`=Update rebooting..), always followed by a generic CPU-identification
  line — a rebuild's boot-record table needs this same key with these same
  four values to reproduce the observed message sequence.

## Open questions

- **Where the RESET_CMD argument string is tokenised** into `UDNL`'s
  `argc`/`argv` — inside `MODLOAD`'s syscall-12 handler, or somewhere in
  `MODLOAD+0x12e0` before the final `jalr`. Not isolated in this pass.
- **Whether `REBOOT+0x160`'s `$16 = 0x3f0` is BOOT-8's shared absolute table
  cell or an offset into `REBOOT`'s own relocated `.data`/`.bss`.** Flagged
  as "most likely the latter" above but not confirmed either way; needs the
  module's actual relocated load address, observed at run time.
- **`sifman` ordinals 5/22/29's exact SDK names**, and specifically whether
  ordinal 22 (called with argument `0x00020000`, `SIF_STAT_CMDINIT`'s bit
  value) writes `SMFLG` at all — no ps2sdk header was available in this pass
  to confirm, and BOOT-10c's asymmetric write rule (`spec/03`) means an IOP
  write there could only set, not clear, that bit if it does.
- **Whether a distinct `SIF_STAT_BOOTEND`-shaped bit exists** for an EE
  client's `sceSifIopSync` to poll, versus a rebuild only ever needing
  `SIF_STAT_SIFINIT`'s reappearance (§1's "What is inferred" reasoning). No
  EE-side client code that actually calls `sceSifIopReset`/`sceSifIopSync`
  was available to trace in this pass.
- **`MODLOAD+0x12e0`'s final `jalr $16`** — very likely the hand-over into
  `UDNL`'s `entry(argc, argv, 0, module_record)`, but its four argument
  values were not cleanly decoded.
- **`"ROM directory not found"` (`ADDDRV`'s only string) appearing in a plain
  `sceSifIopReset` reboot's console trace**, despite `ADDDRV` being
  unreferenced by name anywhere in `UDNL`/`REBOOT`/`MODLOAD`'s bytes, and
  previously known only as an `OSDCNF`-list entry (`docs/analysis/10`).
  Unresolved — possibly reached by archive position rather than name, inside
  the untraced part of `MODLOAD+0x12e0`.
- **`UDNL`'s per-module copy loop and the final staged-kernel hand-over**
  (entry address, jump/`eret`, and whatever becomes of BOOT-8's table for
  the new kernel core) — the merge rule and the staging allocation are
  pinned; the actual copy and hand-over were not reached before this pass's
  budget ran out.
- **`"!include "`'s consumer** — parsed and tagged distinctly from a plain
  module name, but what reads that tag was not traced.
- **IOP address `0x1F8100`** — not located as an argument block, a stack, or
  anything else in this pass; whether it is load-bearing for a rebuild is
  open.
- **Which code actually issues `EELOAD`'s own `sceSifIopReset`.** `EELOAD`'s
  61,840 bytes contain no construction of `cid = 0x80000003` and no
  reference to `"rom0:UDNL rom0:EELOADCNF"` at `0x8E760` (confirmed by two
  independent byte-level searches), yet `PS2e/docs/hw-notes.md` (a sibling
  project of the same author's, not part of this repository) observed the
  call happening twice, mid-chain, around a second entry into the `0x06`
  path for the disc's own ELF. `PS2LOGO`, chained through the same path
  right before it, was not disassembled in this pass and is the likeliest
  candidate.
