# SIO2MAN: the Controller and Card Driver's Startup

`tests/m1/main.c` (`docs/project-state.md` §6) asks the IOP to load
`rom0:SIO2MAN`; the reference answers a module id (25), ours has none. `40`
does not yet write the module — it reads what the retail one *does* between
its `_start` returning and the point a pad or memory-card driver would first
call it, since that startup sequence is what fixes which kernel facilities
(`INTRMAN`, `DMACMAN`, `THBASE`/`THEVENT`, `STDIO`, `LOADCORE`) our image must
already serve correctly before `SIO2MAN` can be built at all. `docs/project-
state.md` §4 also carries an unverified lead from a sibling emulator project:
"`CTRL` bit 0 must read back clear with `I_STAT` bit 17 raised, or `SIO2MAN`
spins." Part of this document's job is to find the code that lead is about
and say exactly what it does — the retail binary is the authority, not the
lead.

```sh
python3 tools/romdir.py assets/SCPH-50000.bin --extract <outdir>   # outside the repo
python3 tools/irxinfo.py <outdir>/SIO2MAN --exports --imports
python3 tools/irxinfo.py <outdir>/XSIO2MAN --exports --imports
python3 tools/irxinfo.py <outdir>/SIO2MAN --dump-load <outdir>/SIO2MAN.text
python3 tools/romdis.py <outdir>/SIO2MAN.text --cpu iop --vma 0 --range 0x0 0xea0
xxd -s 0xea0 -l 0x150 <outdir>/SIO2MAN.text   # the module's own .data, read as literals
```

ps2sdk's `iop/sio/sio2man/include/sio2man.h`, `iop/system/{intrman,dmacman,
threadman,loadcore,stdio}/include/*.h` (fetched from the `ps2dev/ps2sdk`
GitHub tree) supply every name marked "[header]" below, plus the ordinal
macros used to identify import/export slots. `iop/sio/sio2man/src/exports.tab`
(also ps2sdk) gave the export-table *order*, which was cross-checked against
this binary by literal ordinal-to-code-address agreement (§1) rather than
trusted on its own — the same corroboration method `docs/analysis/34` used for
`SIFCMD`/`SIFMAN`. No `sio2man.c` implementation source was read; only the
headers and the `exports.tab`/`imports.lst` ordinal tables, which describe the
*interface*, were consulted, and the binary's own bytes are what every claim
below is checked against.

## 0. Ordinal maps and the register-pointer table

Retail `SIO2MAN` (v1.01) exports 26 ordinals; `ps2sdk`'s `sio2man1` export
table (`exports.tab`'s `DECLARE_EXPORT_TABLE(sio2man1, 1, 2)` block, the
26-entry "SDK 1.3x" table) names every one of them in the same order, and
every name lands on a distinct, plausible offset with no collisions — the same
kind of corroboration `34` used for `SIFCMD`/`SIFMAN`. `XSIO2MAN` (v2.01)
exports 59 ordinals that match a *prefix* of `exports.tab`'s newer 2,7 table
(through `sio2_mtap_update_slots`) — checked the same way, by literal address
agreement: ordinals 27-49 of `XSIO2MAN` point at the *same* code addresses as
ordinals 4-26, exactly where the header's `#define sio2_ctrl_set2 sio2_ctrl_set`
-style aliases say they should.

The module's own `.data` (file offset `0xea0`-`0xff0`, dumped by `--dump-load`)
holds, read as literal little-endian words:

- `0xea0`-`0xea7`: the C string `"sio2man\0"`, the export table's name.
- `0xea8`-`0xf27`: two 16-entry jump tables (`sio2_regN_set`/`sio2_regN_get`'s
  case targets, §2).
- `0xf28`-`0xf51`: the C string `"SIO2_BASIC_THREAD : why I wakeup ? %08lx\n"`.
- `0xf60`: `0x00000ea0` (name pointer, into the string above) and `0xf64`:
  `0x00000101` (version `1.01`) — the export table's own header fields.
- `0xf68`-`0xfe8`, 33 consecutive words, each one address apart by 4: literal
  pointers `0xbf808200`, `0xbf808204`, ..., `0xbf808280` — the **uncached
  KSEG1 form** of the SIO2 register block, i.e. physical `0x1F808200`-
  `0x1F808280` (`doc 34`'s "always uses the KSEG1 form" convention, confirmed
  again here). This is a plain address table, index `n` at `0xf68 + 4n`
  holding `0x1F808200 + 4n` — not a struct copy, a genuine pointer array the
  accessor functions dereference through.
- `0xfec`: `0x00000000` — the residency flag (§1).
- `0xff0`-`0xfff` (`.bss`, not present in the file): the event flag id and
  thread id, written at runtime (§1).

Every accessor function (§2) was disassembled and its dereferenced table
index recorded; the map below is exhaustive over the 26 exports that touch
hardware, and is used throughout this document instead of the register names
by themselves, since the offsets are what is actually verified:

| idx | phys addr | struct field [header `sio2man.h`] | accessor ordinal(s) |
|---|---|---|---|
| 0-15 | `0x1F808200`-`0x1F80823C` | `regdata[0..15]` | 12 `sio2_regN_set`, 13 `sio2_regN_get` |
| 16,18,20,22 | `0x1F808240`, `0x1F808248`, `0x1F808250`, `0x1F808258` | `port_ctrl1[0..3]` | 7 `sio2_portN_ctrl1_set`, 8 `..._get` |
| 17,19,21,23 | `0x1F808244`, `0x1F80824C`, `0x1F808254`, `0x1F80825C` | `port_ctrl2[0..3]` | 9 `sio2_portN_ctrl2_set`, 10 `..._get` |
| 24 | `0x1F808260` | data out | 19 `sio2_data_out` |
| 25 | `0x1F808264` | data in | 20 `sio2_data_in` |
| 26 | `0x1F808268` | **CTRL** | 4 `sio2_ctrl_set`, 5 `sio2_ctrl_get` |
| 27 | `0x1F80826C` | `stat6c` | 6 `sio2_stat6c_get` |
| 28 | `0x1F808270` | `stat70` | 11 `sio2_stat70_get` |
| 29 | `0x1F808274` | `stat74` | 14 `sio2_stat74_get` |
| 30 | `0x1F808278` | unnamed [`sio2_unkn78_*`, header] | 15/16 |
| 31 | `0x1F80827C` | unnamed [`sio2_unkn7c_*`, header] | 17/18 |
| 32 | `0x1F808280` | **STAT** | 21 `sio2_stat_set`, 22 `sio2_stat_get` |

**This table is the first hard finding worth flagging against §4's lead**:
the accessor named `sio2_ctrl_set`/`sio2_ctrl_get` — the one ps2sdk calls
"CTRL" — dereferences idx 26, physical `0x1F808268`, not `0x1F808260`.
`0x1F808260` is where `sio2_data_out` writes. Whatever external documentation
the lead in `docs/project-state.md` §4 was drawn from evidently numbers this
block differently from what this binary's own accessor names imply; §3
below resolves what the lead is actually describing, and it does not depend
on which address is called "CTRL".

## 1. The entry, traced end to end

`.iopmod`: name `sio2man`, version `1.01`, entry file offset `0xa84`
(`--dump-load --vma 0` places file offsets equal to run addresses), `gp
0x8fe0`, sizes `text 0xea0 data 0x150 bss 0x10`. Import libraries and the
ordinals actually stubbed (`irxinfo.py --imports`), each name from its header:

| library | ordinals | names [header] |
|---|---|---|
| `loadcore` | 6 | `RegisterLibraryEntries` |
| `intrman` | 4,5,6,7,17,18 | `RegisterIntrHandler`, `ReleaseIntrHandler`, `EnableIntr`, `DisableIntr`, `CpuSuspendIntr`, `CpuResumeIntr` |
| `stdio` | 4 | `printf` |
| `dmacman` | 28,32,33,34,35 | `sceSetSliceDMA`, `sceStartDMA`, `sceSetDMAPriority`, `sceEnableDMAChannel`, `sceDisableDMAChannel` |
| `thbase` | 4,6,20 | `CreateThread`, `StartThread`, `GetThreadId` |
| `thevent` | 4,6,7,8,10 | `CreateEventFlag`, `SetEventFlag`, `iSetEventFlag`, `ClearEventFlag`, `WaitEventFlag` |

Each import ordinal maps to a call target by the stub itself: every
unresolved IRX import in this file is a `jr $ra` / `addiu $zero,$zero,ORDINAL`
pair (the ordinal sits as the delay slot's immediate, readable directly in
the disassembly — the same pattern that produces `34`'s "reserved slot"
stubs, here used to *identify* ordinals rather than find gaps). This gives an
exact table (loadcore `0xd64`; intrman `0xd88,90,98,a0,a8,b0`; stdio `0xdd4`;
dmacman `0xdf8,e00,08,10,18`; thbase `0xe3c,44,4c`; thevent `0xe70,78,80,88,
90`), used to name every `jal` below without guessing from argument shape
alone.

`_start` (`0xa84`), read straight through:

```
a84-a98  prologue
a88-a94  jal 0xd64 (RegisterLibraryEntries, &sio2man_exports@0xcc0)
a9c-aa0  bnez $2, exit(1)              // registration failed
aa4-ab0  lw $2, [data+0xfec]; bnez $2, exit(1)   // residency flag already set
ab8-abc  [data+0xfec] = 1                          // claim residency
ac0      jal 0x68c    -> sio2_ctrl_set(0x3bc)       // §3, initial CTRL write
ac8      jal 0xa14    -> CreateEventFlag({attr=EA_MULTI(2)[header], initBits=0})
             -> id stored at [data+0xff4] (.bss)
ad8      jal 0xa40    -> CreateThread({attr=TH_C(0x02000000)[header thbase.h],
             option=0, entry=0x89c, stacksize=0x2000, priority=0x18})
             -> id stored at [data+0xff8] (.bss)
ae4      jal 0xda8 (CpuSuspendIntr, &flags@sp+0x10)
afc      jal 0xd88 (RegisterIntrHandler, intr=0x11, mode=1,
             handler=0x9d8, arg=&[data+0xff0])
b04      jal 0xd98 (EnableIntr, 0x11)
b10      jal 0xdb0 (CpuResumeIntr, flags)
b1c,b28  jal 0xe08 (sceSetDMAPriority, ch=11,3) ; (ch=12,3)
b30,b38  jal 0xe10 (sceEnableDMAChannel, ch=11) ; (ch=12)
b44      jal 0xe44 (StartThread, [data+0xff8], 0)
b4c      $2 = 0
b50-b5c  epilogue, return
```

So: register the export table, gate on a one-shot residency flag (return 1
either on registration failure or if already resident — the same value for
both outcomes), write the interrupt-controller-visible IRQ number `0x11`
(**17** — `I_STAT` bit 17, matching `docs/project-state.md` §4's own number
for the SIO2 line) into `RegisterIntrHandler`/`EnableIntr`, enable DMA
channels **11** and **12** at priority **3** (`sceSetDMAPriority` before
`sceEnableDMAChannel`, each called once per channel — this answers the task's
"registered as DMA sub-interrupts?" question directly: **no**, `SIO2MAN`
installs exactly one interrupt handler, for IRQ 17; channels 11/12 are only
primed through `dmacman`, never given their own `RegisterIntrHandler` call),
start the service thread, and return **0** on the normal path.

`_deinit` (`0xb60`, ordinal 2) is the mirror: `GetThreadId` (result not
visibly used — **unresolved**, see §9), `CpuSuspendIntr`, `DisableIntr(0x11)`,
`ReleaseIntrHandler(0x11)`, `CpuResumeIntr`, `sceDisableDMAChannel(11)`,
`sceDisableDMAChannel(12)`. No `DeleteThread`/`TerminateThread` call appears
in this function's traced range.

`CreateThread`'s parameters (`0xa40`) are read straight off the
`iop_thread_t` [header `thbase.h`] fields the code writes to a stack local
before the call: `attr = TH_C` (`0x02000000`, "the entry point is a C
function" [header]), `entry = 0x89c`, `stacksize = 0x2000` (8192 bytes),
`priority = 0x18` (24 — high on the IOP's 1(highest)-126(lowest) scale
[header `thbase.h`]). `CreateEventFlag`'s attr `0x2` is `EA_MULTI` [header
`thevent.h`] — more than one thread/caller is expected to wait on it, which
matches §3: both the service thread and any of the three transfer-entry
exports block on the same event flag from different sides.

## 2. The 26 exports, ordinal by ordinal

| ord | offset | name [header] | dereferences (§0) / role |
|---|---|---|---|
| 0 | `0xa84` | `_start` | §1 |
| 1 | `0xd40` | `_retonly` | bare `jr $ra` stub (reserved slot) |
| 2 | `0xb60` | `_deinit` | §1 |
| 3 | `0xd40` | `_retonly` | same stub as ordinal 1 |
| 4 | `0x0` | `sio2_ctrl_set` | idx26, `0x1F808268` |
| 5 | `0x18` | `sio2_ctrl_get` | idx26 |
| 6 | `0x30` | `sio2_stat6c_get` | idx27, `0x1F80826C` |
| 7 | `0x48` | `sio2_portN_ctrl1_set` | dispatch on `N`∈{0..3}, idx16/18/20/22 |
| 8 | `0xe8` | `sio2_portN_ctrl1_get` | same table, read |
| 9 | `0x18c` | `sio2_portN_ctrl2_set` | idx17/19/21/23 |
| 10 | `0x22c` | `sio2_portN_ctrl2_get` | same table, read |
| 11 | `0x2d0` | `sio2_stat70_get` | idx28, `0x1F808270` |
| 12 | `0x2e8` | `sio2_regN_set` | bounds-check `N<16`, jump table @`0xea8`, idx0-15 |
| 13 | `0x48c` | `sio2_regN_get` | jump table @`0xee8`, idx0-15 |
| 14 | `0x5bc` | `sio2_stat74_get` | idx29, `0x1F808274` |
| 15 | `0x5d4` | `sio2_unkn78_set` | idx30, `0x1F808278` |
| 16 | `0x5ec` | `sio2_unkn78_get` | idx30 |
| 17 | `0x604` | `sio2_unkn7c_set` | idx31, `0x1F80827C` |
| 18 | `0x61c` | `sio2_unkn7c_get` | idx31 |
| 19 | `0x634` | `sio2_data_out` | idx24, `0x1F808260` |
| 20 | `0x644` | `sio2_data_in` | idx25, `0x1F808264` |
| 21 | `0x65c` | `sio2_stat_set` | idx32, `0x1F808280` |
| 22 | `0x674` | `sio2_stat_get` | idx32 |
| 23 | `0xbb8` | `sio2_pad_transfer_init` | event-flag handshake only, §4 |
| 24 | `0xc08` | `sio2_mc_transfer_init` | event-flag handshake only, §4 |
| 25 | `0xc58` | `sio2_transfer` | queues a transfer, §4 |

Ordinals 4-22 are uniformly `{load pointer from the §0 table; dereference;
return}` or `{...; store}` — thin accessors, no side effects beyond the one
word of hardware they touch. `sio2_portN_ctrl1_set`/`_get` (7/8) and
`sio2_portN_ctrl2_set`/`_get` (9/10) branch on `N`∈{1,2,3} against literal
compares (`beq`) rather than indexing arithmetically, four hand-written cases
each; `sio2_regN_set`/`_get` (12/13) instead bounds-check `N<16` and use a
16-entry jump table at `0xea8`/`0xee8` (in the `.data` region dumped in §0),
so the compiler treated the 16-register block differently from the two
4-register ones. Ordinals 23-25 do not touch hardware directly at all — see
§4.

## 3. `CTRL`, `STAT`, and the "SIO2MAN spins" lead

No polling loop on any SIO2 register was found anywhere in `SIO2MAN`'s
traced code (a backward-branch scan over the whole `0x0`-`0xea0` text region
found only unrolled-loop constructs in the register-push/pull and the
byte-copy loops of §4, none of them touching `CTRL`/`STAT` in a spin). Three
places write `CTRL` (idx26, `0x1F808268`), all through the `sio2_ctrl_set`
accessor:

- `0x68c` (called once, from `_start`): `sio2_ctrl_set(0x3bc)` — the one-time
  init write.
- `0x6ac`: `sio2_ctrl_set(sio2_ctrl_get() | 0xC)` — bits 2,3.
- `0x7fc`: `sio2_ctrl_set(sio2_ctrl_get() | 0x1)` — bit 0, the "start" write
  (§4).

`STAT` (idx32, `0x1F808280`) is read and written only inside the interrupt
handler (`0x9d8`, registered for IRQ 17 in §1): `sio2_stat_get()` immediately
followed by `sio2_stat_set()` **with the value just read** — a write-back
acknowledge, the same shape as a hardware register that clears pending bits
on a write of 1 (documented nowhere in this binary, but the code's own
symmetry — read then write-same-value, nothing else — has no other sensible
reading). After acknowledging, the handler calls `iSetEventFlag(ef, 0x80)`
(§4) and returns.

So `SIO2MAN` never spins on `CTRL`. It sets `CTRL` bit 0 once per transfer
(`0x7fc`) and then blocks the service thread in `WaitEventFlag` for bit
`0x80` — a bit only the IRQ-17 handler ever sets. **This is what
`docs/project-state.md` §4's lead is describing from the outside**: an
emulator whose SIO2 model does not both clear `CTRL` bit 0 *and* raise
`I_STAT` bit 17 when a transfer finishes leaves that `WaitEventFlag` blocked
forever — which looks exactly like "spins" to anything watching the guest
from outside (the thread never returns, the module never answers), even
though there is no CPU-side polling loop to find. An emulator's SIO2 model
must, at minimum: accept a write of `CTRL` bit 0 as "start", complete the
programmed transfer, and then raise IOP `I_STAT` bit 17 — `SIO2MAN` supplies
no fallback path if it does not.

## 4. The service thread and its event-flag protocol

The thread (`0x89c`, `SIO2_BASIC_THREAD`, per the debug string at `0xf28`)
and the three transfer-entry exports (23-25) share one `CreateEventFlag`
handle (`EA_MULTI`, §1) and use eight of its bits as a request/acknowledge
protocol. Every `ClearEventFlag(ef, bits)` call in this code clears **the one
bit whose complement `bits` encodes** (e.g. `ClearEventFlag(ef, -3)` is
`0xFFFFFFFD`, which clears bit 1 and leaves everything else — checked
directly against every occurrence below, not assumed):

| bit | set by | cleared by | meaning |
|---|---|---|---|
| 0 (`0x1`) | `sio2_pad_transfer_init` | thread, on wake | pad-mode request |
| 1 (`0x2`) | thread, after seeing bit 0 | `sio2_pad_transfer_init` | pad-mode ack |
| 2 (`0x4`) | `sio2_mc_transfer_init` | thread, on wake | mc-mode request |
| 3 (`0x8`) | thread, after seeing bit 2 | `sio2_mc_transfer_init` | mc-mode ack |
| 4 (`0x10`) | `sio2_transfer` | thread, before running the transfer | "run it" request |
| 5 (`0x20`) | thread, after harvesting results | `sio2_transfer` | "done" ack |
| 6 (`0x40`) | `sio2_transfer`, after consuming bit 5 | thread | "you may loop" release |
| 7 (`0x80`) | the IRQ-17 handler (`iSetEventFlag`) only | thread, after its hardware wait | hardware finished |

Thread body, `0x89c`-`0x9c0` (loops back to `0x8b4`):

1. `WaitEventFlag(ef, 0x5, OR)` — blocks for either bit 0 or bit 2.
2. Neither (a spurious wake somehow reached the flag): `printf("SIO2_BASIC_
   THREAD : why I wakeup ? %08lx\n", res)` (`0xdd4`, stdio ordinal 4) and the
   function **returns** — the thread exits rather than re-looping. This is
   the one path in the whole module with no recovery; **unresolved** whether
   anything can re-`StartThread` it (§9).
3. Bit 0 or bit 2: clear it, `SetEventFlag(ef, 2 or 8)` (the matching
   ack) — sent *before* any hardware access.
4. `WaitEventFlag(ef, 0x10, AND)` — now blocks for `sio2_transfer`'s "run it".
5. Clear bit 4. `sio2_ctrl_set(sio2_ctrl_get()|0xC)` (`0x6ac`).
6. `0x6d4`, given the transfer struct's address (read from a global at
   `[data+0xffc]`, written by `sio2_transfer`, step below): calls
   `sio2_portN_ctrl1_set(N, td->port_ctrl1[N])` for `N`=0..3, then
   `sio2_regN_set(N, td->regdata[N])` for `N`=0..15; if `td->in_size>0`,
   calls `sio2_data_out()` once per byte of `td->in`; if `td->in_dma.addr`
   is non-null, `sceSetSliceDMA(11, td->in_dma.addr, .size, .count)`
   (`0xdf8`) then `sceStartDMA(11)` (`0xe00`); the same for `td->out_dma`
   on channel **12**. `td->port_ctrl2[0..3]` is **never written** by this
   routine — **unresolved**, see §9.
7. `sio2_ctrl_set(sio2_ctrl_get()|0x1)` (`0x7fc`) — the hardware start.
8. `WaitEventFlag(ef, 0x80, AND)` — blocks until the IRQ-17 handler answers
   (§3).
9. Clear bit 7. `0x824`: `td->stat6c = sio2_stat6c_get()`, `td->stat70 =
   sio2_stat70_get()`, `td->stat74 = sio2_stat74_get()`; if `td->out_size>0`,
   `td->out[i] = sio2_data_in()` for each byte.
10. `SetEventFlag(ef, 0x20)` — "done".
11. `WaitEventFlag(ef, 0x40, AND)` — blocks for `sio2_transfer`'s release.
12. Clear bit 6, loop to step 1.

`sceSetSliceDMA`/`sceStartDMA` (`dmacman` 28/32) are only reached here, for
per-transfer DMA of `in_dma`/`out_dma` buffers; the `sceSetDMAPriority`/
`sceEnableDMAChannel` calls in `_start` (§1) are one-time channel setup, not
per-transfer.

`sio2_pad_transfer_init` (`0xbb8`) and `sio2_mc_transfer_init` (`0xc08`):
`SetEventFlag(ef, 1 or 4)`, `WaitEventFlag(ef, 2 or 8, AND)`, `ClearEventFlag`
the ack bit, return — **no hardware register access anywhere in either
function**. Given the thread body only reacts to bits 0/2 by clearing and
acking (step 3 above), and the actual transfer only runs once bit 4 is set,
these two exports read as a synchronization step — "claim the driver for a
pad-shaped or mc-shaped request" — with the register values themselves
supplied by the caller's own `sio2_transfer_data_t` [header `sio2man.h`] and
pushed only when `sio2_transfer` is called. This reading is **inferred** from
the bit protocol and the absence of hardware access in these two functions,
not from a comment or header describing the intended split.

`sio2_transfer` (`0xc58`, ordinal 25): stores its `td*` argument at
`[data+0xffc]`, `SetEventFlag(ef, 0x10)`, `WaitEventFlag(ef, 0x20, AND)`,
`ClearEventFlag` bit 5, `SetEventFlag(ef, 0x40)`, returns 1. This is the only
one of the 26 exports that drives a real hardware transfer; the answer to
"is a minimal SIO2MAN that starts, registers, and sits waiting faithful
until a pad/memory-card driver calls it" is **yes for `_start`/`_deinit`/the
21 register accessors**, and yes for `sio2_pad_transfer_init`/
`sio2_mc_transfer_init` doing nothing but the handshake — but `sio2_transfer`
itself needs the full `0x6ac`/`0x6d4`/`0x7fc`/IRQ-wait/`0x824` chain to be
real, since that is where the SIO2 hardware is actually driven.

## 5. `XSIO2MAN`, for comparison

`XSIO2MAN` (v2.01, 59 exports) mirrors `SIO2MAN`'s `_start` closely: same
import set (`loadcore` 6; `intrman` 4,5,6,7,17,18; `dmacman` 28,32,33,34,35;
`thbase` 4,6,20; `thevent` 4,6,7,8,10 — `stdio` is not imported at all, so
the "why I wakeup" diagnostic did not survive into this revision), the same
`RegisterIntrHandler(intr=0x11, mode=1, ...)`/`EnableIntr(0x11)` pair, the
same `sceSetDMAPriority`/`sceEnableDMAChannel` calls for channels 11 and 12
at priority 3, and the same `CpuSuspendIntr`/`CpuResumeIntr` bracketing. The
residency flag moved to a larger `.data` offset (`0x125c` vs `0xfec`) and
`_start` additionally zeroes four consecutive words (`[state+0x0..0xc]`)
before creating the event flag — new per-instance state, plausibly for the
multitap slot-tracking exports (`sio2_mtap_*`, ordinals 51-58, unique to
this revision and not present in `SIO2MAN`'s 26) — not decoded further here,
since the task treats this file as a comparison point only. The agreement on
IRQ number, DMA channels, and DMA priority across both revisions is treated
as confirmation that these three facts are stable module-to-module, not an
artifact of one binary.

## 6. What this pins for the rebuild

For `LoadStartModule("rom0:SIO2MAN")` to return a module id with the module
resident and its service thread parked correctly waiting, in the order
`_start` needs them:

1. **`LOADCORE`**: `RegisterLibraryEntries` must accept the 26-entry export
   table and make it findable by name (`"sio2man"`) for whatever resolves
   `SifLoadModule`'s answer into a module id.
2. **`INTRMAN`**: `RegisterIntrHandler`/`EnableIntr` for IOP IRQ **17**
   (`I_STAT` bit 17) must succeed and the handler must actually be callable
   later; `CpuSuspendIntr`/`CpuResumeIntr` must round-trip a flags value
   correctly around the setup (§1). `ReleaseIntrHandler`/`DisableIntr` need
   to work too, for `_deinit`, though nothing in the M1 pull calls it yet.
3. **`THEVENT`**: `CreateEventFlag` with `EA_MULTI` must return a usable id;
   `WaitEventFlag`/`SetEventFlag`/`iSetEventFlag`/`ClearEventFlag` must
   implement the OR/AND wait modes and the bit-clear-by-complement
   convention exactly, since the thread's own progress depends on it (§4).
4. **`THBASE`**: `CreateThread` must accept `TH_C`/priority 24/stack 0x2000
   and return an id `StartThread` can use; the thread must actually run
   independently once started — this is the module's whole reason for
   existing as a *service*, not a one-shot init.
5. **`DMACMAN`**: `sceSetDMAPriority`/`sceEnableDMAChannel` for channels 11
   and 12 must succeed at `_start` time; `sceSetSliceDMA`/`sceStartDMA` must
   be real for a driver to move buffers larger than what the byte-at-a-time
   `sio2_data_out`/`sio2_data_in` loop can carry (§4).
6. **`STDIO`**'s `printf` must exist for the one diagnostic path (§4 step 2)
   not to fault — a minimal image could stub this to a no-op, since it is
   only reached on what the code itself treats as an error case.

And the hardware behaviour an emulator must show, independent of any of the
above being implemented faithfully in software:

- `CTRL` (`0x1F808268` by this binary's own accessor naming, §0) accepting a
  bit-0 write as "start a transfer."
- On completion of that transfer: `I_STAT` bit 17 raised, so the IRQ-17
  handler fires, acknowledges via `STAT`'s (`0x1F808280`) read-then-write-
  same-value pattern (§3), and sets event-flag bit `0x80` — the only path
  that unblocks the service thread's hardware wait. Absent this, `SIO2MAN`'s
  thread blocks forever in `WaitEventFlag`, which is the actual mechanism
  behind `docs/project-state.md` §4's "spins" lead (§3).
- `sio2_data_out`/`sio2_data_in` (`0x1F808260`/`0x1F808264`) and the 16-word
  `regdata`/4-word `port_ctrl1`/`port_ctrl2` blocks (`0x1F808200`-`0x1F80825C`)
  behaving as plain read/write hardware ports — no polling behaviour was
  found gating any of them.

## 7. Unresolved

- The `sio2_unkn78`/`sio2_unkn7c` registers (idx 30/31, `0x1F808278`/
  `0x1F80827C`) are unnamed in `sio2man.h` itself [header] and were not
  written or read anywhere in `SIO2MAN`'s traced code outside their own
  accessor bodies — their role in the transfer sequence, if any, is
  undetermined.
- `_deinit`'s `GetThreadId()` call (`0xb68`) has no visible consumer of its
  return value in the traced range — plausibly compared against something
  outside what was disassembled, or dead code; not resolved.
- `td->port_ctrl2[0..3]` is part of `sio2_transfer_data_t` [header] but is
  never read by the `0x6d4` push routine, unlike every other field in the
  struct's first half — whether callers are expected to have already set it
  via the exported `sio2_portN_ctrl2_set` accessor directly (bypassing the
  struct), or whether this is simply unused by the retail driver, was not
  determined.
- The exact split of responsibility between `sio2_pad_transfer_init`/
  `sio2_mc_transfer_init` and `sio2_transfer` (§4) is inferred from the
  event-flag bit protocol and the absence of hardware access in the two
  `_init` functions; no PAD or memory-card driver module (which would call
  these) was disassembled in this pass to confirm the calling convention
  from the other side.
- What happens if the service thread's outer `WaitEventFlag` wakes for
  neither bit 0 nor bit 2 (§4 step 2, the thread's only exit path) —
  whether any code elsewhere ever re-`StartThread`s it, or this is meant to
  be unreachable in practice — was not determined.
- `XSIO2MAN`'s `_start` (§5) was read only far enough to confirm agreement
  with `SIO2MAN` on IRQ number, DMA channels, and DMA priority; its extra
  per-instance state (four zeroed words) and its nine `sio2_mtap_*`
  ordinals were not disassembled.
- The literal init value `sio2_ctrl_set(0x3bc)` (`0x68c`, called once from
  `_start`) was not decoded bit-by-bit against any documented CTRL-register
  layout; it is reported as observed, not interpreted.
