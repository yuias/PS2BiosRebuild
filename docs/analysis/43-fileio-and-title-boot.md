# FILEIO and the Title's Own Boot

`docs/project-state.md` §6 names M2's remaining pull: `OSDSYS` reading
`SYSTEM.CNF` needs `CDVDMAN`/`CDVDFSV` (disc hardware — `docs/analysis/42`,
out of scope here) and **`FILEIO`** (`IOMAN` over the SIF), and running the
title needs the EE kernel's `LoadExecPS2` path plus whatever the title's own
SDK-runtime start-up pulls with `SifLoadModule`. This document covers those
two pieces: `FILEIO`'s RPC surface, read directly off the retail module, and
the retail title's own start-up, read directly off `SLPS-25918.iso`'s boot
ELF. It does not cover the disc hardware or `CDVDMAN`/`CDVDFSV` — `docs/
analysis/42-cdvd.md` is that document.

```sh
python3 tools/romdir.py assets/SCPH-50000.bin --extract <outdir>
python3 tools/irxinfo.py <outdir>/FILEIO --exports --imports
python3 tools/irxinfo.py <outdir>/FILEIO --dump-load <outdir>/FILEIO.text
python3 tools/irxinfo.py <outdir>/SIFCMD --exports    # ordinal -> address, cross-checked below
python3 tools/irxinfo.py <outdir>/SIFMAN --exports
python3 tools/romdis.py  <outdir>/FILEIO.text --cpu iop --vma 0 --range START END
```

The title's ELF is not stored in the repository and never extracted into it.
`assets/SLPS-25918.iso` is a plain ISO9660 image; no ISO9660 tool is
installed on this machine, so its `SYSTEM.CNF` and boot ELF were read out
with a small standalone ECMA-119 (ISO9660) reader — a public, generic
file-format, not any project's implementation source — writing only to a
scratch directory outside the repository. The ELF disassembles cleanly with
`/usr/lib/llvm-22/bin/llvm-objdump -d --triple=mipsel --mcpu=mips3`, since it
already carries correct link-time addresses; `tools/romdis.py --cpu ee`
(MIPS-III decode, no true R5900 target — `docs/analysis/22`'s already-noted
limitation) was not needed for this pass and no R5900-opcode gaps were hit in
the regions read below.

ps2sdk's headers were consulted only for `FILEIO`'s client-facing fno
enumeration and to look for wire-format structs; every such use is marked
`[header]`. The retail binary is the authority for everything else, and for
byte offsets specifically the binary is the *only* source: the current
ps2sdk headers (`common/include/fileio-common.h`) define aligned structs
whose C layout does not obviously correspond to what the retail module reads
off the wire (see §5).

## 0. Ordinal maps

**`FILEIO`** (`.iopmod` name `FILEIO_service`, v1.01; entry `0x0`, text
`0x1220`, data `0x250`, bss `0x790`) exports nothing — like `LOADFILE`
(`docs/analysis/39` §4), its whole public surface is two SIF RPC servers
(§2). Its own imports, ordinal-checked below the same way `docs/analysis/34`
and `39` check theirs:

`sysmem` (v1.01, 3 stubs) `[4, 5, 14]` — `AllocSysMemory`/`FreeSysMemory`/an
unidentified third; `loadcore` (v1.01, 2 stubs) `[5, 12]` — ordinal 12 is
`QueryBootMode` (`docs/analysis/39` §0), ordinal 5 not previously
identified; `intrman` (v1.02, 3 stubs) `[9, 17, 18]` — 17/18 are the
`CpuSuspendIntr`/`CpuResumeIntr` critical-section pair (`docs/project-state.md`
§4), ordinal 9 not previously identified; `sifman` (v1.01, 4 stubs)
`[5, 7, 8, 29]`; `thbase` (v1.01, 3 stubs) `[4, 6, 20]` —
`CreateThread`/`StartThread`/`GetThreadId`; `sifcmd` (v1.01, 5 stubs)
`[14, 17, 19, 22, 23]` — one more than `LOADFILE`'s `[14, 17, 19, 22]`
(`docs/analysis/34` §0); `stdio` (v1.02, 1 stub) `[4]` — `printf`; `ioman`
(v1.02, 17 stubs) `[4..18, 20, 21]` — every `IOMAN` operation `docs/analysis/
39` §1 names, in ordinal order.

**`sifman`'s own export table** (`<outdir>/SIFMAN --exports`) resolves the
four ordinals `FILEIO` imports directly, correcting/extending `docs/
analysis/34`'s text-address citations to ordinal numbers:

| ordinal | address | identity |
| --- | --- | --- |
| 5 | `0x148` | `sceSifInit` — exact match, `docs/analysis/34` §1 |
| 7 | `0x598` | `sceSifSetDma` — exact match, `docs/analysis/34` §0's "`598` SetDma" |
| 8 | `0x740` | not named by `34`; behaviour here (§4) matches `sceSifDmaStat` — **inferred from call shape, not a header or a named address** |
| 29 | `0x2d8` | `sceSifCheckInit` — exact match, `docs/analysis/34` §0's "`2d8` CheckInit" |

**`sifcmd`'s own export table**, same method, gives ordinal 23 (the one
import `FILEIO` has beyond `LOADFILE`'s set) as `0xad4` — exactly `docs/
analysis/34`'s "`ad4` GetOtherData": `FILEIO` is a genuine client of
`sceSifGetOtherData` (`SIF_CMD_RPC_RDATA`, `docs/analysis/34` §3), used in
`dread`/`getstat`/`chstat` and in `read`'s unaligned head/tail (§4).

## 1. Two RPC services, two threads

`FILEIO`'s entry (`0x0`–`0xfc`) does no file-serving work itself. After a
`loadcore` ordinal-12 (`QueryBootMode`) call that only gates which banner
string prints (not decoded further — cosmetic), it creates **two** IOP
threads, both priority `0x60` (96 decimal), and starts each in turn:

| Thread | Stack | Body | Registers |
| --- | --- | --- | --- |
| 1 | `0x1000` (4 KiB) | `0xd04` | `sid = 0x80000001` |
| 2 | `0x800` (2 KiB) | `0xf70` | `sid = 0x80000003` |

Both bodies are structurally identical and match `LOADFILE`'s own shape
(`docs/analysis/39` §4) closely — `sceSifCheckInit`(sifman 29), conditionally
`sceSifInit`(sifman 5), a banner `printf`, `sceSifInitRpc`(sifcmd 14, `$a0=0`
i.e. wait mode), `sceSifGetThreadId`(thbase 20), `sceSifSetRpcQueue`(sifcmd
19), `sceSifRegisterRpc`(sifcmd 17), `sceSifRpcLoop`(sifcmd 22, never
returns):

```
# thread 1, 0xd4c-0xd94
RegisterRpc(sd=0x1600, sid=0x80000001, func=0xac4, buf=0x1648,
            cfunc=0, cbuf=0, qd=0x15e8)

# thread 2, 0xfb0-0xff8
RegisterRpc(sd=0x1ab8, sid=0x80000003, func=0xec0, buf=0x1b00,
            cfunc=0, cbuf=0, qd=0x1aa0)
```

Both `sid` literals are built as `lui $5,0x8000; ori $5,$5,N` immediates, the
same pattern `docs/analysis/34` §3 used to pin `LOADFILE`'s `0x80000006` —
**`0x80000001` is `FILEIO`'s well-known service id, confirmed directly from
the binary**, and independently corroborated: the title's own client
(companion research, §11) binds the identical `0x80000001` at its own call
site.

**`sid = 0x80000003`, the second service, is left unidentified.** Its
dispatch function (`0xec0`) is a 3-way mode switch, `func(mode, a, b)`:

| mode | target | behaviour |
| --- | --- | --- |
| 1 | `0xe5c` | `AllocSysMemory(type=0, size=*a); *b = pointer` |
| 2 | `0xe94` | `FreeSysMemory(*a); *b = result` |
| 3 | `0xdb0` | open `a+4` (`flags=1`), `lseek` size, `read` the whole file into `*b`, `close` |

mode 3's shape (open read-only, size via `lseek`-to-end, one `read` sized to
the file, `close`) is exactly `MODLOAD`'s own file-read idiom (`docs/
analysis/39` §3). Nothing in this pass identifies what calls this second
service or why `FILEIO` hosts it alongside the file RPC proper — **flagged
in §12, not resolved.**

## 2. The bounce buffer, allocated lazily by whichever fno runs first

A shared function at `0x100` probes for the largest working scratch
allocation, seeding `0x14a4` (a `.bss` word — the *chosen chunk size*) with
`0x4000` (16 KiB) and calling `sysmem` ordinal 4 (`AllocSysMemory(type=1,
size=chunk)`); on failure it **halves** the chunk size (`(size >> 31) + size)
>> 1`, i.e. plain truncating divide-by-two) and retries, **up to 8 times**
(sizes `0x4000, 0x2000, 0x1000, 0x800, 0x400, 0x200, 0x100, 0x80` — a floor
of 128 bytes on the 8th attempt). The winning pointer is stored at `0x14a0`
(a second `.bss` word — the *bounce buffer address*, shared by every fno
that needs one).

This probe is **not** run at module entry. `open` (§3, fno 0) and `write`
(fno 3) each test `0x14a0` for non-null and call `0x100` themselves if it is
still zero, printing a shared "allocation failed" string (`FILEIO.text
+0x1280`) and answering `-1` if even the smallest attempt fails. **`read`
(fno 2) does not perform this check** — it reads `0x14a0` directly at
`0x2cc`+`0x30c` and uses whatever is there, including a null pointer if
`read` is somehow the very first fno a caller issues before any `open`. This
reads as an unguarded assumption in the retail binary — flagged, not
independently confirmed as reachable in practice, since a real client always
`open`s before it `read`s.

## 3. The `sid = 0x80000001` fno dispatch table

The registered `func` (`0xac4`) receives `(fno, buf, size)`, drops `size`,
range-checks `fno < 0x11` (17 — `sltiu $2,$3,0x11`; out of range prints
`FILEIO.text+0x1368` and returns nothing, matching `LOADFILE`'s own "no
reply for an out-of-range fno" shape, `docs/analysis/39` §4), and jumps
through a 17-entry pointer table at `+0x1390` (read directly from the file's
`.data`, base 0 since the module loads unrelocated in this trace):

| fno | table target | operation | ioman ordinal called |
| --- | --- | --- | --- |
| 0 | `0x1a4` | open | 4 |
| 1 | `0x264` | close | 5 |
| 2 | `0x2cc` | read | 6 (chunked, §4) |
| 3 | `0x568` | write | 7 (chunked, §4) |
| 4 | `0x290` | lseek | 8 |
| 5 | `0x700` | ioctl | 9 |
| 6 | `0x738` | remove | 10 |
| 7 | `0x780` | mkdir | 11 |
| 8 | `0x7c8` | rmdir | 12 |
| 9 | `0x8ec` | dopen | 13 |
| 10 | `0x948` | dclose | 14 |
| 11 | `0x974` | dread | 15 (+ `sceSifSetDma`, §5) |
| 12 | `0xa00` | getstat | 16 (+ `sceSifSetDma`, §5) |
| 13 | `0xa8c` | chstat | 17 |
| 14 | `0x810` | format | 18 |
| 15 | `0x858` | AddDrv | 20 |
| 16 | `0x8a4` | DelDrv | 21 |

Every handler is called through a common thunk that sets `$a0 = buf` (the
fixed request buffer, `0x1648`) and `$a2 = 0x15d8` (the fixed **reply**
buffer, 112 bytes before the request buffer) before `jal`ing the case body;
the wrapper's own return value — what `sceSifExecRequest` sends back as the
`RPC_END` body (`docs/analysis/34` §3) — is that same `0x15d8` pointer,
exactly `LOADFILE`'s "answer into a fixed `.bss` area, return only a
pointer" shape (`docs/analysis/39` §4).

**A likely retail defect, flagged rather than asserted:** the jump-table
entries are 0x1c bytes apart (7 instructions: set `$a0`/`$a2`, `jal`, `j
0xce8` epilogue, `nop`) for every fno **except fno 6** (`remove`, `0x738`),
whose thunk is only 0x14 bytes (5 instructions — no `j 0xce8;nop` pair).
`remove`'s own body properly saves/restores `$ra` around its internal calls
and returns normally, so the `$ra` the wrapper set (`0xbc0`, the very next
instruction — the start of fno 7's, `mkdir`'s, argument setup) is where
control lands: **a `remove` call appears to fall straight through into a
`mkdir` call using the same buffer**, both writing through the same fixed
reply address before the real reply is sent. This was read from the jump
table's raw 17 x `u32` entries at `FILEIO.text+0x1390` (unrelocated, since
the module loads at base 0 in this trace) and the thunk boundaries, not
inferred from naming; it was not exercised under a simulator or emulator in
this pass, so whether it is truly reachable (rather than an artefact of how
this table was read) is an open question (§12):

```sh
python3 -c "
import struct
data = open('<outdir>/FILEIO.text', 'rb').read()
for i in range(17):
    print(i, hex(struct.unpack_from('<I', data, 0x1390 + i*4)[0]))
"
```

## 4. Request/answer layouts

Offsets are from the fixed request buffer (`0x1648`) unless noted; every
answer is written to the fixed reply buffer (`0x15d8`) unless noted.
`printf` debug-format calls that accompany each handler (all through `stdio`
ordinal 4) are omitted below; they read as diagnostics only, one string
literal per operation.

```
open    (fno 0):  { mode:u32 @0, path:cstr @4 }        -> { fd_or_err:s32 }
close   (fno 1):  { fd:u32 @0 }                          -> { result:s32 }
lseek   (fno 4):  { fd:u32 @0, offset:s32 @4, whence:u32 @8 } -> { position:s32 }
ioctl   (fno 5):  { fd:u32 @0, cmd:u32 @4, arg:... @8 }  -> { result:s32 }
remove  (fno 6):  { path:cstr @0 }                        -> { result:s32 }
mkdir   (fno 7):  { path:cstr @0 }                        -> { result:s32 }
rmdir   (fno 8):  { path:cstr @0 }                        -> { result:s32 }
format  (fno 14): { path:cstr @0 }                        -> { result:s32 }
dopen   (fno 9):  { path:cstr @0 }                        -> { fd_or_err:s32 }
dclose  (fno 10): { fd:u32 @0 }                            -> { result:s32 }
dread   (fno 11): { fd:u32 @0, dest_ee_addr:u32 @4 }      -> { count_or_err:s32 }
                   -- dirent (0x12c = 300 bytes) DMA'd straight to dest_ee_addr, not in the reply
getstat (fno 12): { dest_ee_addr:u32 @0, path:cstr @4 }   -> { result:s32 }
                   -- iox_stat_t-sized (0x28 = 40 bytes) DMA'd straight to dest_ee_addr
chstat  (fno 13): { mask:u32 @0, stat:[0x28 bytes] @4, path:cstr @0x2c } -> { result:s32 }
AddDrv  (fno 15): { device_ptr:u32 @0 }                    -> { result:s32 }
DelDrv  (fno 16): { name:cstr @0 }                          -> { result:s32 }
```

`open`'s `path` starting at `+4` (not `+0`) and `getstat`'s `dest_ee_addr`
starting at `+0` with the path at `+4` are read exactly as the disassembly
gives them — the field order is **not** uniform across operations, so a
rebuild must take each layout from its own handler rather than assume one
shared convention. `getstat`'s 40-byte transfer and `chstat`'s inline
40-byte stat block agree with each other (`chstat`'s path begins exactly at
`+4+0x28 = +0x2c`) — internal cross-confirmation that 0x28 bytes is the
retail on-wire stat struct's size, matching `iox_stat_t`'s known field sizes
(`mode`+`attr`+`size` as 32-bit words, three 8-byte timestamps, one more
32-bit word: `4+4+4+8+8+8+4 = 40`) closely enough to be convincing, though no
header's struct was read to confirm field names or order — **inferred from
size and position only.**

`ioctl`'s `arg` is passed as a raw pointer into the request buffer at `+8`
(not copied elsewhere) — the caller's `arg` payload rides inline in the same
112-ish bytes as the rest of the request, so an `ioctl` whose argument needs
more than that (the request buffer is bounded by the RPC packet's own
`0x200`-ish practical ceiling, not independently measured here) is not
served by this path. `AddDrv`'s single field being handed straight to
`ioman` ordinal 20 as a raw `iop_device_t*` looks unlikely to be a real,
working RPC path from an EE caller — an EE-side pointer cannot be a valid
IOP `iop_device_t` with live function pointers — and was not chased further;
flagged as probably vestigial/never exercised by real SDK client code (§12).

## 5. How `read` and `write` move data: alignment split, `SifSetDma`, `GetOtherData`

`read`'s handler (`0x2cc`) reads the request as `{ fd:u32 @0,
dest_ee_addr:u32 @4, length:u32 @8 }` and, for any request of 16 bytes or
more, splits the transfer into three pieces **by the 16-byte alignment of
`dest_ee_addr`**, not by any alignment of the file offset:

- **head**: `(16 - (dest_ee_addr & 0xf)) & 0xf` bytes, bringing `dest_ee_addr
  + head` onto a 16-byte boundary (zero if already aligned);
- **middle**: the largest run from there that is itself a multiple of 16
  bytes;
- **tail**: whatever remains below 16 bytes.

**Head and tail** (each ≤ 15 bytes) are read from the file into two small,
fixed 16-byte `.bss` scratch buffers (`0x1480` for the head, `0x1490` for
the tail — 16 bytes apart, sized to the largest possible unaligned
fragment) via a plain `ioman` ordinal-6 `read`, then pushed to the EE with
`sceSifGetOtherData` (`sifcmd` ordinal 23, `SIF_CMD_RPC_RDATA` — `docs/
analysis/34` §3's "server-initiated extra copy from the server's buffer
straight to an EE address"), which can target an address of any alignment.
This is the answer to "how does `FILEIO` place bytes at an address the SIF
DMA engine cannot address directly": it does not — it routes the unaligned
remainder through the RPC layer's own arbitrary-address primitive instead of
the raw DMA one.

**The aligned middle** goes through the shared bounce buffer (`0x14a0`,
§2), one probed-chunk-size (`0x14a4`, ≤16 KiB, ≥128 bytes) piece at a time:
`ioman` ordinal-6 `read` into the bounce buffer, then — inside a critical
section (`intrman` ordinals 17/18, `CpuSuspendIntr`/`CpuResumeIntr`) — a
**raw `sceSifSetDma`** call (`sifman` ordinal 7) with a single-entry transfer
descriptor `{ src = bounce_buffer, dest = current_ee_addr, size = chunk,
attr = 0 }`. Before reusing the bounce buffer for the next chunk, the
handler polls `sceSifDmaStat` (`sifman` ordinal 8, inferred — §0) in a tight
loop until the previous transfer has drained. So **the alignment rule is: an
EE destination address needs no alignment at all for `read`** — anything
under 16 bytes at either end goes through `GetOtherData`, and the aligned
interior moves through raw DMA in chunks bounded by whatever the lazy
16 KiB-or-smaller probe (§2) could allocate.

`write`'s handler (`0x568`) shares the same lazy bounce-buffer allocation
(same failure string, same `0x100` probe call) and the same two ioman
operations it drives (`ioman` ordinal 7, `write`, called at two distinct
sites — a small inline first chunk taken directly from the request body at
`+0x10`, then further chunks) — a mirror of `read`'s structure moving data
the other way, through the same primitives (`GetOtherData` for the
odd portion, chunked writes for the rest). This pass traced its request
layout only partially: `{ fd:u32 @0, ?:u32 @4, length:u32 @8,
first_chunk_len:u32 @0xc, first_chunk_data:[...] @0x10, ... }` — the field
at `+4` and the mechanism that supplies later chunks beyond the inline first
one were not fully resolved; flagged in §12.

## 6. The title's ELF and `SYSTEM.CNF`

`SYSTEM.CNF`, read verbatim from the ISO9660 image:

```
BOOT2 = cdrom0:\SLPS_259.18;1
VER = 1.02
VMODE = NTSC
```

The named file is `\SLPS_259.18;1` — ELF32, MIPS, `e_entry = 0x00100134`,
one `PT_LOAD` segment (`vaddr 0x100000`, `filesz 0x19c6f8`, `memsz
0x2ca8f0`). This confirms `docs/analysis/41`'s finding that `SYSTEM.CNF`
parsing belongs to `OSDSYS`, not `EELOAD`: nothing in `EELOAD` itself reads
this file (`docs/analysis/41` §4), so whatever stands in `OSDSYS`'s slot for
M2 has to do it — see §10.

## 7. The title's syscalls

The runtime carries the SDK's usual generic stub table (`addiu $3,$zero,N;
syscall; jr $ra`, one per number) — the same shape `docs/analysis/41` found
`EELOAD` and `m1.dis` both carrying their own copies of — running `0x00`
through `0x82`, with slots `0x80`/`0x81` absent, matching `docs/
project-state.md`'s "the SDK's runtime installs its own handlers at `0x7F`
and `0x82`" note exactly. A second cluster of *inline* (non-table) stub
bodies supplies more slots directly, including several past the documented
`0x00`–`0x7C` range (`0x54`–`0x5b`, `0x74`, `0x80`, `0x83`); `0x3C`/`0x3D`
at the program's very entry and `0x23` are fully inline, no shared stub.

**Every distinct slot number confirmed by a live call site** (not merely
present in the stub table):

```
02 04 06 07 0d 0e 10 11 12 13 14 15 16 17
1a 1b 1c 1d
20 21 22 23 24 25
29 2b 2f 30 31 32 33 34 35 37 38 39
3c 3d 3e
40 41 42 43 44 45 46 47 4a 4b
54 55 56 57 58 59 5a 5b
64 68 6b 6f
71 73 74 76 77 78 79 7a 7b 7c
7f 80 82 83
```

Slots negated at their call site (`spec/05`'s "number passed in `$v1` with
negatives negated" convention) were seen for several of the alias-block
numbers (e.g. `0x1c`) and for at least one slot reachable positively as well
(`0x2f` called both as `+0x2f` and `-0x2f`) — direct client-side confirmation
of that convention from a second, independent client (`m1.dis` was the
first, `docs/analysis/41`/`21`).

**Individually identified, high confidence:**

- **`0x3C`** at the very entry (`SetupThread`, `spec/05` SYS-8a) immediately
  followed by **`0x3D`** with `size = -1` (`SetupHeap`, "use all available
  memory") — the same self-priming pattern `docs/analysis/41` §1 found in
  `EELOAD` itself.
- **`0x7a`** (`SifGetReg`) called with `$a0=4` in a poll loop masking bit
  `0x40000` — exactly `docs/analysis/34` §1's `sceSifGetReg(4)` SBUS/SIF
  control-bit poll inside `sceSifInitCmd`, independently confirmed from a
  second client's own statically-linked copy of the library.
- **`0x74`** (`SetSyscall`, `spec/05` SYS-5a) called four times — the title
  installs its own handlers at some syscall numbers at run time, which is
  exactly how `0x7F`/`0x80`/`0x82`/`0x83` (outside the documented 125 slots)
  come to be callable at all: **the client supplies them itself**, they are
  not something the kernel needs to pre-populate.
- **`0x71`** (`spec/05` SYS-7c, the GS interrupt-mask *write* half of the
  `0x70`/`0x71` pair) is called — the title genuinely exercises the GS-IMR
  path the reference's own shipped kernel implements.
- **`0x06`**/**`0x07`** (program loader / `ExecPS2`) are present in the
  linked runtime but not confirmed to run on the ordinary path (they back
  the "return to browser" and re-exec machinery, not necessarily exercised
  during a title's own steady-state boot).

The remaining slots' precise identity (most of `0x40`–`0x4b`, `0x54`–`0x5b`,
`0x64`–`0x83`) was not individually traced to argument-shape evidence in
this pass — see §12 and §10's table for what that means for the "needs"
side.

## 8. `SifLoadModule` order

`SifLoadModule`'s calls all route through one shared helper reached from
`0x18a068($a0 = "cdrom0:\MODULES\"` constant, `$a1 = filename)`, called
**twelve times, back to back, with no branch between them**, from one
init function entered at `0x103c38`:

```
sio2man.irx -> msifrpc.irx -> padman.irx -> cdvdstm.irx -> mcman.irx
-> mcserv.irx -> libsd.irx -> sdrdrv.irx -> modmidi.irx -> modhsyn.irx
-> ezmidi.irx -> cri_adxi.irx
```

All twelve are the title's **own** copies, loaded from the disc's
`\MODULES\` directory — none of them is `rom0:SIO2MAN`, the module this
project already builds (`src/iop/sio2man.cpp`, in the archive per `src/rom/
manifest.txt.in`). This is the load-bearing difference from M1: M1 needed
*us* to serve a working `SIO2MAN` from `rom0:`; M2's title brings its own
IOP-side drivers on the disc and only needs a working `cdrom0:` device and a
generic loader (`LOADFILE`/`MODLOAD`/`IOMAN`, already built) to fetch them —
see §10.

Immediately **before** the twelve loads, the same init function polls a
function (`0x181de0`) with the literal path `"cdrom0:\MODULES\IOPRP310.IMG;1"`
and then polls `SifGetReg(4) & 0x40000` (the same SBUS/SIF bit `sceSifInitCmd`
polls, §7) — **until it is set, not until it clears**; the polarity is
corrected in `docs/analysis/45` §"Open questions", from a run of the title
against this project's image where the loop was observed directly — `0x181de0` internally references the string
`"rom0:UDNL"` (found once, nowhere else in the ELF), which is the pattern
`docs/project-state.md`'s carried lead names for `sceSifIopReset` ("SIFCMD
cid `0x80000003` reboots the IOP without the ROM stub"). **This was not
resolved to a confirmed `SifIopReset` call and packet in this pass** — see
§9.

## 9. `SifIopReset`/`SifInitRpc`/`SifExitRpc`/`SifIopSync`

No unambiguous `SifIopReset(image, mode)` call site, with its `RESET_CMD`
(`SIF_CMD_ID_SYSTEM | 3` = `0x80000003`, `docs/analysis/34` §2's table) wire
traffic, was isolated. The circumstantial case (§8: `"cdrom0:\MODULES\
IOPRP310.IMG;1"` handed to a function that references `"rom0:UDNL"`,
immediately before the twelve sequential `.irx` loads) is consistent with a
title-side "reset the IOP with this bundled image, then reload the
individually named modules" sequence — `IOPRP310.IMG` has exactly the shape
(`IOPRPxxx.IMG`) of a retail title's bundled default-module image — but this
pass could not confirm the actual `RESET_CMD` packet or its argument at the
instruction level. **This is the open question that most directly bears on
whether our IOP needs to survive or honour a mid-boot reset for M2**, and it
is left open rather than guessed at (§12).

No separately-named `SifExitRpc`/`SifIopSync` call site was individually
isolated either — plausibly inlined into the same init routine without a
distinguishable call boundary in the disassembly examined.

## 10. What the title needs vs. what exists today

In the order the title's own start-up meets each item:

| Item | Title needs | Exists today |
| --- | --- | --- |
| `0x3C`/`0x3D` (SetupThread/SetupHeap) | yes, at entry | **built** (`sysSetupThread`/`sysSetupHeap`, `src/kernel/tables.cpp`) |
| Syscalls `0x02`, `0x04` | called, arity known (`spec/05` SYS-1) | **not built** — `sysUndefined` |
| Syscalls `0x06`–`0x39` range (thread/sema core, `0x1a`-`0x39` etc.) | called throughout | **built** — every one of these lands on a real handler already |
| Syscalls `0x4a`, `0x4b` | called | **not built** — `sysUndefined`; arity known (`spec/05` SYS-1), no semantic doc covers them |
| Syscalls `0x54`–`0x5b` | present in the linked binary's call graph | **already spec-compliant as `sysUndefined`** — `spec/05` SYS-3 documents these as legitimately undefined *on the reference itself*; their presence in the title's binary is very unlikely to be exercised on the real boot path (see §12) |
| Syscall `0x64`, `0x68`, `0x6f` | called | **not built** — `sysUndefined`; arity known only |
| Syscall `0x6b` (`SifStopDChain`) | called | **built** (`sysSifStopDChain`) |
| Syscall `0x71` (GS-IMR write, `spec/05` SYS-7c) | called | **not built** — `sysUndefined`, though behaviour is fully specified already |
| Syscall `0x73` | called | **not built** — `sysUndefined`; arity known only |
| Syscall `0x74` (`SetSyscall`) | called 4x, to self-install `0x7f`/`0x80`/`0x82`/`0x83` | **built** (`sysSetSyscall`) — this is the one slot that must work for the four "extra" slots to become callable at all; those four need no kernel-side table entries of their own |
| Syscalls `0x76`–`0x7a` (SIF group) | called | **built** (`sysSifDmaStat`/`sysSifSetDma`/`sysSifSetDChain`/`sysSifSetReg`/`sysSifGetReg`) |
| `SifBindRpc(0x80000001)` (`FILEIO`) | binds it, per §7's cross-confirmation and this document's own §1 finding | **not built** — no `src/iop/fileio.cpp`; this document's §1–§5 is the spec-ready analysis for it |
| `SifBindRpc(0x80000006)` (`LOADFILE`) | binds it, separately from `EELOAD`'s own bind | **built** (`src/iop/loadfile.cpp`) |
| `SifLoadModule("cdrom0:\MODULES\...")` x12 | needs a working `cdrom0:` `IOMAN` device | **not built** — no `cdrom0:` device exists; `docs/analysis/42`'s territory. The loader chain above it (`LOADFILE`/`MODLOAD`/`IOMAN`/`LOADCORE`) is already built and device-agnostic (`docs/analysis/39` §5), so once `cdrom0:` exists as a driver, no further loader work is needed for these twelve modules — the title brings its own `PADMAN`/`MCMAN`/etc. binaries |
| `SifBindRpc` for cdvd family (`0x80000592`–`0x8000059c` range) | binds several | **not built** — `docs/analysis/42`'s territory |
| `SifBindRpc(0x80000100)`/`0x80000101` (pad) | binds both | **not built** — no `PADMAN`-side service exists on our IOP, but the title loads its **own** `padman.irx` from disc (§8), so the RPC server the title binds to is one it just loaded itself, not something our image needs to provide, *once `cdrom0:` module loading works* |
| An MC (`0x80000090`-ish) `SifBindRpc` | expected (title loads `mcman.irx`/`mcserv.irx`) | not located in the traced call sites — open question, §12 |
| `SifIopReset` (possible, `IOPRP310.IMG`) | unconfirmed whether genuinely called | not applicable — our IOP's behaviour under a mid-boot reset is untested either way; §9 |
| `SYSTEM.CNF` read + `BOOT2=` re-invocation of `0x06`/`EELOAD` | `OSDSYS` must do this (`docs/analysis/41` §4) | **not built** — the current `OSDSYS` slot (`src/rom/manifest.txt.in`, `CMakeLists.txt` `PS2_OSDSYS`) is our own placeholder stub ("prints one line and spins", `docs/project-state.md` §1), not the retail `OSDSYS`. M2's own gate needs *something* in that slot that opens `cdrom0:\SYSTEM.CNF`, reads `BOOT2=`, and re-issues syscall `0x06` with that path — most plausibly a small SDK-built stand-in analogous to M1's own test program, not a full `OSDSYS` rebuild (`docs/project-state.md` §6 defers the real `OSDSYS`/menu to M3) |

## 11. Cross-confirmation

`sid = 0x80000001` for `FILEIO` and `sid = 0x80000006` for `LOADFILE` were
each independently found twice in this pass: once from `FILEIO`'s own
binary (§1) and once from the title's own client-side `SifBindRpc` call
sites (`0x17d5f4` and `0x180ee4` respectively, both routed through one
shared bind-wrapper at `0x17c418`). Agreement between an IOP-side server's
own registration and an EE-side client's own bind literal, from two
completely independent disassemblies, is the same corroboration pattern
`docs/analysis/34` §0 used for `SIFCMD`/`SIFMAN`'s ordinal tables.

The title also binds a cdvd-family cluster (`0x80000592`, `0x80000593`,
`0x80000595`, `0x80000597`, `0x8000059a`, `0x8000059c`) and two pad-family
ids (`0x80000100`, `0x80000101`); `0x80000593` matches `docs/project-state.md`
§4's already-known "`CDVDFSV` RPC service `0x80000593`" exactly. These
belong to `docs/analysis/42`'s coverage and are reported here only as the
cross-check they provide for this document's own `FILEIO`/`LOADFILE`
findings.

## 12. Unresolved

- ~~**`FILEIO`'s second RPC service, `sid = 0x80000003`**~~ — **its caller
  is now known, and so is what it is.** `SLPS-25918` binds it immediately
  after its IOP reboot returns (§8's sequence), before any of the twelve
  module loads. Reading the dispatch to the byte (below) names it: it is the
  **IOP-heap service** the SDK's `sceSifAllocIopHeap` /
  `sceSifFreeIopHeap` / `sceSifLoadIopHeap` trio talks to. Its own strings
  say as much — `"sce_iopmem: unrecognized code %x\n"` on the dispatch's
  reject path and `"iop heap service (99/11/03)\n"` as the thread's banner.

  The switch is on the RPC `fno` itself, not on a word in the request, and
  every path — including a rejected `fno` — returns the same four-byte
  `.bss` cell at `0x1a90`, so a rejected call is acknowledged with whatever
  the previous call left there. The request buffer is `0x1b00` and, from the
  module's own end, exactly `0x100` bytes.

  | `fno` | Request | Reply at `0x1a90` |
  | --- | --- | --- |
  | 1 | `+0`: size | the pointer from `AllocSysMemory(type 0, size, addr 0)`, or 0 |
  | 2 | `+0`: pointer | `FreeSysMemory`'s return |
  | 3 | `+0`: destination IOP address; `+4`: the path, inline | 0, or -1 if `open` was refused |

  `fno 3` is `open(path, flags 1)`, `lseek(fd, 0, SEEK_END)` for the size,
  `lseek(fd, 0, SEEK_SET)`, one `read` of the whole file **straight into the
  IOP address the request named** — no staging through §2's bounce buffer,
  no chunking — then `close`. The size is never reported back and neither
  `read`'s nor `close`'s return is checked; a refused `open` is the only
  failure it can answer. One instruction is load-bearing and easy to lose:
  the `SEEK_END` result is captured in the *delay slot* of the second
  `lseek`'s call, before that call clobbers `$v0`.

  ```sh
  python3 tools/romdis.py <outdir>/FILEIO.load --cpu iop --vma 0 --range 0xdb0 0x1000
  ```
- **The fno-6/fno-7 (`remove`/`mkdir`) fallthrough** (§3): read directly off
  the jump table's raw bytes and the thunks' instruction counts, but not
  exercised under a simulator or emulator, so whether it is genuinely
  reachable — and whether a real client ever triggers it — is open.
- **`read`'s missing bounce-buffer-null check** (§2): `read` uses `0x14a0`
  unconditionally where `open`/`write` both guard it; whether this is ever
  reachable (i.e., whether any real client issues `read` before any `open`
  or `write` on the same `FILEIO` instance) was not determined.
- **`sifman` ordinal 8** (§0) is identified only by call shape (a
  drain-poll before bounce-buffer reuse, matching `sceSifDmaStat`'s role
  exactly) — no header or independently-named address confirms it.
- **`write`'s full request layout** (§5): the field at `+4` and the
  mechanism supplying chunks beyond the first inline one were not resolved.
- **`AddDrv` (fno 15)'s real usability from an EE client** (§4) — passing a
  raw request-buffer pointer to `ioman` ordinal 20 as an `iop_device_t*`
  looks unlikely to work from the EE side; not chased further.
- **`sysmem` ordinal 14, `loadcore` ordinal 5, and `intrman` ordinal 9**
  (§0) — imported by `FILEIO` but not identified by name or behaviour in
  this pass (ordinal 5/9's one call site each gate a cosmetic banner choice
  and were not traced past that).
- **Most of the title's individual syscall identities** beyond the
  high-confidence set in §7 — `0x40`–`0x4b`, `0x54`–`0x5b`, `0x64`–`0x83`
  are pinned only to a number and a call-site address, not to a name or
  argument-shape trace.
- **`SifIopReset`, confirmed or not** (§9) — the single open question this
  document flags as most load-bearing for M2: does the title's start-up
  genuinely reset the IOP mid-boot (and with what image/mode), and if so,
  what must our IOP do to survive or honour it. Needs a dedicated pass on
  the `0x181de0`/`0x181d90` region and the system-cid `0x80000003` traffic
  shape, ideally run under `PS2e`'s or `PCSX2`'s own debugger against the
  reference image the way `docs/analysis/29` settled its own open question.
- **An MC (`0x80000090`-ish) `SifBindRpc`** (§10) — expected given the title
  loads `mcman.irx`/`mcserv.irx`, not located among the traced bind call
  sites; may use a different call shape than the other binds, or sit in a
  code path not covered by this pass.
- **The sid at the title's earliest bind call site** (`0x1402f4`, before any
  of the ones in §11) is loaded from a data global rather than a literal;
  its value was not read out of the ELF's initialised data.
