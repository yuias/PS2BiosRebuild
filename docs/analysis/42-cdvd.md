# CDVD: `CDVDMAN`'s Disc Driver and `CDVDFSV`'s RPC Surface

`docs/analysis/12-ee-facing-services.md` surveyed `CDVDMAN` (`cdvd_driver`, boot
list position 24, 62 exports, a `0x16430`-byte `.bss`) and `CDVDFSV`
(`cdvd_ee_driver`, position 25, 10 exports) only as far as their shape;
`docs/analysis/26-cdvd-nvm-and-config.md` went one layer deeper for the S-command
transport and the OSD configuration blocks, and is not repeated here except by
citation. This document is what `docs/project-state.md` §6's M2 (a retail title
boots from disc) needs for the rest of the disc side: `CDVDMAN`'s start-up, the
N-command register block and the DMA channel a 2048-byte sector read actually
uses, how `cdrom0:\PATH;1` is resolved against the ISO9660 layout, and
`CDVDFSV`'s RPC surface — the SIF services and function numbers a title's EE
client (`libcdvd`) calls, and which `CDVDMAN` export each reaches.

```sh
python3 tools/romdir.py assets/SCPH-50000.bin --extract <outdir>   # outside the repo
python3 tools/irxinfo.py <outdir>/CDVDMAN --exports --imports
python3 tools/irxinfo.py <outdir>/CDVDFSV --exports --imports
python3 tools/irxinfo.py <outdir>/CDVDMAN --dump-load <outdir>/CDVDMAN.load
python3 tools/irxinfo.py <outdir>/CDVDFSV --dump-load <outdir>/CDVDFSV.load
python3 tools/romdis.py <outdir>/CDVDMAN.load --cpu iop --vma 0 --range 0x0 0x4d30
python3 tools/romdis.py <outdir>/CDVDFSV.load --cpu iop --vma 0 --range 0x0 0x4980
```

ps2sdk's `iop/cdvd/cdvdman/include/cdvdman.h`, `common/include/libcdvd-common.h`,
`ee/rpc/cdvd/include/libcdvd.h` and `common/include/libcdvd-rpc.h` (fetched from
`raw.githubusercontent.com/ps2dev/ps2sdk/master/...`) name every ordinal,
struct field and enum value marked "[header]" below; no `.c`/`.cpp` file from
ps2sdk was read, per `docs/clean-room-policy.md`. Every register address, every
ordinal-to-address binding and every disassembled sequence is checked against
this ROM's own bytes — the header supplies names, the binary is the authority,
and several places below note where the two disagree.

## 0. Ordinal maps

`CDVDMAN`'s 62-entry export table (`irxinfo --exports`) matches
`cdvdman.h`'s `DECLARE_IMPORT(N, name)` [header] numbering exactly at every
position checked — confirmed independently by string cross-reference (below)
rather than trusted on the header alone, the same corroboration method
`docs/analysis/34` used for `SIFCMD`/`SIFMAN` and `docs/analysis/40` used for
`SIO2MAN`. **Important ceiling**: this retail v1.04 `CDVDMAN` exports only
ordinals `0`–`61`; `cdvdman.h` [header] also documents ordinals up to `191`
(`sceCdMmode` is `75`, `sceCdStPause`/`StResume` are `67`/`68`, `sceCdReadGUID`
is `80`, and so on) — **none of those exist in this ROM's `CDVDMAN`**. A title
that calls `sceCdMmode` against this BIOS gets whatever the SDK's client does
when a bind fails, not a working call; the task description that prompted this
document asked for `sceCdMmode` among the exports, and the answer is that it is
absent from this retail image.

| Ord | Addr | Name [header] | Confirmed by |
| --- | --- | --- | --- |
| 0 | `0x0` | (module entry) | — |
| 1–3, 18 | `0x2780` | (reserved stub) | shared address |
| 4 | `0x2a84` | `sceCdInit` | `"sceCdInit called mode= %d"` string load |
| 5 | `0x3270` | `sceCdStandby` | ordinal position; not independently traced |
| 6 | `0x34c0` | `sceCdRead` | `"Read Command call"` string load; full trace, §3 |
| 7 | `0x3b20` | `sceCdSeek` | ordinal position |
| 8 | `0x3190` | `sceCdGetError` | reads the IRQ-2 handler's result byte, §2b |
| 9 | `0x9f8` | `sceCdGetToc` | calls the disk-type/S-command routine at `0x3318` |
| 10 | `0x9d8` | `sceCdSearchFile` | calls `0x19f0`, the path-walk routine, §4 |
| 11 | `0x960` | `sceCdSync` | polls `sceCdCheckCmd`'s word, §3 |
| 12 | `0xbd4` | `sceCdGetDiskType` | reads `0xBF40200F` directly |
| 13 | `0xa18` | `sceCdDiskReady` | polls `0xBF40200A` for `0x0A`, §2b |
| 14 | `0x429c` | `sceCdTrayReq` | ordinal position |
| 15 | `0x32a8` | `sceCdStop` | ordinal position |
| 16/17 | `0xd00`/`0xbfc` | `sceCdPosToInt`/`sceCdIntToPos` | ordinal position |
| 19 | `0x33f0` | `sceCdGetToc2` | ordinal position |
| 20 | `0x3934` | `sceCdReadDVDV` | `"DVD read pram set"`/`"DVD read command call"` |
| 21 | `0x3088` | `sceCdCheckCmd` | reads `struct+4`, the ISR's "done" word, §2b/§3 |
| 22/23 | `0x3b80`/`0x3bf4` | `sceCdRI`/`sceCdWI` | ordinal position |
| 24/25 | `0x4014`/`0x407c` | `sceCdReadClock`/`sceCdWriteClock` | ordinal position |
| 26/27 | `0x411c`/`0x41e0` | `sceCdReadNVM`/`sceCdWriteNVM` | `docs/analysis/26` |
| 28 | `0xbe8` | `sceCdStatus` | ordinal position; enum matches `0xBF40200A`, §2b |
| 29 | `0x2dbc` | `sceCdApplySCmd` | wraps the S-command sender, `docs/analysis/26` |
| 30 | `0x4b1c` | `sceCdSetHDMode` | `"Set HD mode= %d"` string load |
| 31–34 | `0x467c`…`0x4a3c` | `sceCdOpenConfig`…`sceCdWriteConfig` | `docs/analysis/26` |
| 35 | `0x433c` | `sceCdReadKey` | ordinal position; disc-key derivation, §7 |
| 36 | `0x4648` | `sceCdDecSet` | ordinal position; sector-decrypt control, §7 |
| 37/38 | — /`0x32e0` | `sceCdCallback`/`sceCdPause` | ordinal position |
| 39 | `0x2e48` | `sceCdBreak` | `"Break call"`/`"cdvd: Abort command On"` |
| 40 | `0x3718` | `sceCdReadCDDA` | ordinal position |
| 41/42 | `0x3c58`/`0x3cd4` | `sceCdReadConsoleID`/`sceCdWriteConsoleID` | ordinal position |
| 43 | `0x3d60` | `sceCdMV` | ordinal position |
| 44 | `0x3454` | `sceCdGetReadPos` | ordinal position |
| 45 | `0x3f2c` | `sceCdCtrlADout` | `"Audio Digital Out: Set param %d"` |
| 46 | `0x3208` | `sceCdNop` | ordinal position |
| 47 | `0x74` | `sceGetFsvRbuf` | 2-instruction function returning the fixed constant `0x81b0` — see §5e |
| 48–51 | `0x27d0`…`0x29e0` | `sceCdstm0Cb`/`sceCdstm1Cb`/`sceCdSC`/`sceCdRC` | ordinal position |
| 52 | `0x3ee4` | `sceCdForbidDVDP` | ordinal position |
| 53 | `0x3dd0` | `sceCdReadSUBQ` | reads three consecutive N-registers, §2b |
| 54 | `0x3000` | `sceCdApplyNCmd` | `"Apply NCmd call cmd= 0x%02x"` string load, §2b |
| 55 | `0x3fa0` | `sceCdAutoAdjustCtrl` | `"Auto Adjust Ctrl: Set param %d"` |
| 56–61 | `0xd80`…`0xea8` | `sceCdStInit`…`sceCdStStop` | `"sceCdSt* call"` string block |

`CDVDFSV` (`cdvd_ee_driver`, `iop_cdvd_cdvdfsv` v1.01, 10 exports — `0` entry,
`1–3`/`9` a shared reserved stub, `4`–`8` real) imports **49 of `CDVDMAN`'s 62**
ordinals (`irxinfo --imports`): `[4,5,6,7,8,9,10,11,12,14,15,20,21,22,23,24,
25,26,27,28,29,30,31,32,33,34,35,36,38,39,40,41,42,43,44,45,46,47,49,50,52,
53,54,55,56,57,58,59,61]` — missing only `13,16,17,19,37,48,51,60` (`sceCdDiskReady`
is conspicuously among the missing — see §5b/§6, `CDVDFSV`'s own `0x80000592`
service calls it nowhere the disassembly reached, and no `fno` of `0x80000593`
does either; this is recorded as open in §9). `CDVDFSV`'s own `cdvdman`
import-stub table is laid out at module offset `0x47f0`, one 8-byte `jr $ra` /
`addiu $zero,$zero,ORDINAL` pair per imported ordinal, **in exactly the order**
`irxinfo --imports` lists them — so `stub_addr = 0x47f0 + 8×index` turns every
`jal` inside `CDVDFSV`'s own code into a `CDVDMAN` ordinal without
disassembling `CDVDMAN` itself, the same trick `docs/analysis/37`/`38`/`39`
use throughout for reserved-slot and import-ordinal identification.

## 1. `CDVDMAN`: start-up

Entry (`0x0`):

```
0x0:  jal <loadcore 6>   RegisterLibraryEntries(&cdvdman_exports@0x2670)
      bnez $2, -> return 1                        (registration failed)
0x20: jal <ioman 21>     DelDrv("cdrom")
0x30: jal <ioman 20>     AddDrv(&cdvd_iop_device_t@0x57b0)
      bnez $2, 0x58: jal 0x100 (cleanup); return 1  (AddDrv failed)
      else:      jal 0x2a18 (irq/DMA init, below); return 0   (stays resident)
```

`AddDrv` calls the device's own `ops->init` (module offset `0x84`) before
returning, per `docs/spec/06-iop-kernel.md` IOP-4a. `init()` creates **three**
`SA_THPRI`-attribute, binary (`initial=1, max=1`) semaphores via `thsemap`
ordinal 4 (`CreateSema`), storing their ids at `.bss+0x5970`, `+0x5810` and
`+0x5814`, and zeroes a 16-entry, `0x14`-byte-stride directory-entry cache
table at `.bss+0x5840` (the cache `"CdSearchFile: cache dir data used"` and
`"CD_cachefile:"` describe, §4). `deinit` (ops+`0x04` = `0x100`) is the
mirror for the `+0x5970` semaphore and the cache table only; the other two
semaphores are freed only by `sceCdInit(mode=5)`'s own teardown (below), not
by module `deinit`.

### The `iop_device_t` and its ops table

```
device @0x57b0: name=0x4d48("cdrom") type=0x10(IOP_DT_FS) version=1
                desc=0x4d40("CD-ROM ") ops=0x576c
```

| Slot | ops+ | Target | Real / shared stub |
| --- | --- | --- | --- |
| init | `0x00` | `0x0084` | real |
| deinit | `0x04` | `0x0100` | real |
| format | `0x08` | `0x0934` | **shared stub** |
| open | `0x0c` | `0x0148` | real |
| close | `0x10` | `0x0358` | real |
| read | `0x14` | `0x03d4` | real |
| write | `0x18` | `0x0934` | **shared stub** |
| lseek | `0x1c` | `0x0834` | real |
| ioctl/remove/mkdir/rmdir/dopen/dclose/dread/getstat/chstat | `0x20`–`0x40` | `0x0934` | **shared stub**, all nine |

Unlike `ROMDRV` (`docs/analysis/39` §2, which splits its stubs between a
"return `0`" no-op and a dedicated "return `-5`" for `write`), `CDVDMAN`
routes **all eleven** unimplemented ops — `write` included — through **one**
shared function at `0x934` that prints `"nulldev call"` and unconditionally
returns `0`. A rebuild's `cdrom` device therefore reports `write`/`getstat`/
`chstat` as succeeding-with-nothing, not as an error.

`open` (`0x148`) and `read` (`0x3d4`) are what a caller reaches through
`IOMAN`'s ordinary path parser (`docs/spec/06-iop-kernel.md` IOP-4c–4d):
`open` runs the internal `sceCdSearchFile`-shaped path walk (§4) against the
post-colon remainder `IOMAN` hands it, and disc recognition (`CD_newmedia`,
§4) runs from this same path — not only from `sceCdInit` — the first time a
name is looked up. `read` dispatches into the same low-level sector reader
`sceCdRead` (ordinal 6) uses.

### No dedicated service thread

`CDVDMAN`'s `thbase` import stub table binds only ordinals **20**
(`GetThreadId`) and **33** (`DelayThread`) — ordinal 4 (`CreateThread`) is
simply absent, so no code path in this module can call it. This is
structurally different from `SIO2MAN` (`docs/analysis/40`), which runs its
whole protocol on a dedicated service thread. **`CDVDMAN` runs entirely on
the calling thread**: `DelayThread` backs busy-wait backoff (e.g. the drive-
stop poll inside `sceCdInit(mode=5)`'s teardown), and the three semaphores
serialize concurrent callers against each other and against the interrupt
handler, not a producer/consumer hand-off to a service thread. A rebuild's
`CDVDMAN` should be structured the same way: hardware commands issued
synchronously on whatever thread calls in; the "service" shape belongs to
`CDVDFSV` (§5), which does run dedicated threads.

### Interrupt and DMA priming

A private routine at `0x2a18` (called once from the entry's success path,
and again — the load-bearing call — from `sceCdInit`'s normal-mode branch,
below) does:

```
RegisterIntrHandler(irq=2, mode=1, handler=0x27f0, arg=0x1BC30)   [intrman 4]
EnableIntr(2)                                                     [intrman 6]
DPCR (0xBF8010F0) |= 0x8000     -- channel 3's own enable nibble (bit 15)
CHCR (0xBF8010B8) = 0            -- DMA channel 3 reset/idle
```

**IRQ 2 is the IOP's CDVD interrupt line** — read directly off the argument
to `RegisterIntrHandler`, and matching `tools/iopsim.py`'s own
`IRQ_CDVD = 2` constant and the reference's own `I_MASK = 0x1080D` (bit 2 set)
already recorded in `docs/analysis/24-sif-data-path.md`. **DMA channel 3 is
CDVD's DMA channel**: `0xBF8010B0`/`B4`/`B8` (`MADR`/`BCR`/`CHCR`) follow the
bank-1 `0x10`-per-channel stride `docs/analysis/07-bus-and-dma.md` already
established, and channel 3 is the PS1-heritage CD-ROM slot in that bank
(0=MDECin, 1=MDECout, 2=GPU, 3=CDROM, 4=SPU, 5=PIO, 6=OTC). `CDVDMAN` imports
no `dmacman` ordinal at all — every DMA register it touches is poked directly
(IOP-2c/2i's convention for a driver that owns its channel outright, the way
`DMACMAN` itself pokes its priority/reset fields per `docs/analysis/07`), and
`CDVDMAN` never calls `RegisterIntrHandler`/`EnableIntr` for channel 3's own
IRQ (`0x23`, IOP-2c's bank-1 numbering) anywhere in the module — the only
`RegisterIntrHandler` call in the whole file is IRQ 2's, above. The driver
relies on the CDVD hardware's own command-complete interrupt, not on DMA
channel 3's completion interrupt, to know a transfer has landed — flagged as
open in §9, since it is not confirmed from software alone that the DMA is
reliably finished by the time IRQ 2 fires.

## 2. The hardware: two command channels

### 2a. S-commands (recap)

`docs/analysis/26` already pinned the S-command transport in full: sender at
module offset `0x2b70`, registers `0xBF402016` (write: start command),
`0xBF402017` (read: status; write: one parameter byte), `0xBF402018` (read:
one result byte), status bit `0x80` busy / bit `0x40` result-FIFO-empty. It
declines rather than waits on a busy channel. `sceCdApplySCmd` (ordinal 29,
`0x2dbc`) is the exported wrapper; NVM access (ordinals 26/27, S-commands
`0x0A`/`0x0B`) and the OSD configuration quartet (ordinals 31–34, S-commands
`0x40`–`0x43`) are the two S-command families `26` decoded in full. Not
repeated here.

### 2b. N-commands: reading, seeking and everything data-path

A **separate** register block, distinct from the S-command one:

| Addr | Use |
| --- | --- |
| `0xBF402004` | write: N-command number (starts the command) |
| `0xBF402005` | read: status — bit `0x80` busy, bit `0x40` set = ready; precondition to send is `(status & 0xC0) == 0x40` (opposite polarity convention from the S-command sender's drain loop, which waits for `0x40` to *clear*); write: one parameter byte, looped |
| `0xBF402006` | write: a one-byte "read submode" `sceCdRead` sets directly before applying the command (observed values `0x40/0x80/0x83/0x85/0x86/0x8f`, chosen by disk type and datapattern, not decoded bit-by-bit); read: the command's raw result byte, read by the IRQ-2 handler |
| `0xBF402007` | write-only; `sceCdBreak` (ordinal 39) writes `1` here right after logging `"Abort command On"` — the abort/break trigger |
| `0xBF402008` | read: bit 0, tested by the IRQ-2 handler to pick between its two completion paths; write: `1` or `2` (path-dependent) as the acknowledge |
| `0xBF40200A` | read: drive state — `sceCdDiskReady` (ordinal 13) polls this for the literal value `0x0A`, which is **exactly `SCECdStatPause`** [header, `enum SCECdvdDriveState`] — `sceCdStatus` (ordinal 28) reads the same register and returns the raw byte as that enum |
| `0xBF40200B` | read: bit 0, a completion/expected-state flag tested by a two-slot wait scheme at module offset `0x309c` (below) |
| `0xBF40200C`–`0E` | read as three consecutive bytes only inside `sceCdReadSUBQ` (ordinal 53) — the subchannel-Q data bytes |
| `0xBF40200F` | read: disk-type byte — `sceCdGetDiskType` (ordinal 12) returns it raw; observed comparison values `0x14` (`SCECdPS2DVD` [header]), `0xFD` (`SCECdCDDA`) and `0xFE` (`SCECdDVDV`) both drive `sceCdRead`'s submode choice and a spin-up routine's S-command parameter (`0x84` for `0x14`/`0xFE`, `0x80` for `0xFD`) |

**The command-issue sequence** — a private, unexported sender at module
offset `0x2ee8` (`sceCdApplyNCmd`, ordinal 54 at `0x3000`, is a thin
debug-print-and-retry wrapper around it: on a `-1` result it prints
`"Apply NCmd call cmd= 0x%02x"`, `DelayThread(2000µs)` and retries):

1. `PollSema` (thsemap ordinal 9) on the semaphore cached at `.bss+0x5810`. A
   `-419` (`KE_SEMA_ZERO`) result declines immediately, returning `-1` — no
   retry inside this routine itself (the `0x3000` wrapper is what retries).
2. Read `0xBF402005`; require `(status & 0xC0) == 0x40`, else `SignalSema`
   (undo step 1) and return `-1`.
3. `GetThreadId()` (thbase ordinal 20) and stash the caller's own id into a
   small command-state record in IOP RAM at `0x1BC30` (`{cmd@0, result@1,
   retries@3, done@4, thread_id@8, …}`, sizes inferred from field accesses
   below) — the same record `RegisterIntrHandler`'s `arg` points at.
4. Write each parameter byte, one at a time, to `0xBF402005` — the same port
   doubles as status(read)/param(write), mirroring the S-command pair.
5. Write the command number to `0xBF402004`; read `0xBF402004` back
   (stored, not obviously consumed further).
6. `SignalSema`, return `0` — **"the command was accepted," not "the command
   finished."** The sender never blocks for completion.

**The IRQ-2 handler**, module offset `0x27f0` (`arg` = the `0x1BC30` record):

1. Reads `0xBF402006` → stores at `record+1` (the result byte
   `sceCdGetError`, ordinal 8, later returns).
2. Reads `0xBF402008` bit 0. **Clear**: bumps a retry counter at `record+3`,
   acknowledges by writing `2` to `0xBF402008`. **Set**: reads `0xBF402005`
   bit 0 to decide `record+4` — **bit 0 set is the error**, giving `-1`, and
   bit 0 clear gives `1` — the word `sceCdCheckCmd` (ordinal 21) later
   returns raw — and acknowledges by writing `1` to `0xBF402008`.
   The polarity is easy to read backwards, because the `-1` is assigned in
   the branch's **delay slot** and so runs on both paths:
   `2840 bnez $2, 0x284c; 2844 addiu $2,$zero,-1; 2848 addiu $2,$zero,1;
   284c sw $2, 4($16)`. Taking the branch (bit 0 set) keeps the delay slot's
   `-1`; falling through overwrites it with `1`.
3. Depending on a caller-set selector at `record+0xC` (e.g. `sceCdRead`
   writes `2` into `.bss+0x5828` right before issuing its command), calls one
   of two registered function-pointer callbacks (`.bss+0x5820`/`+0x5824`),
   passing the `.bss+0x5828` value as the argument.

**No `SignalSema` call appears anywhere in the IRQ-2 handler.** Completion is
purely the `record+4` word and the optional callback pointers — and,
critically, `sceCdSync` (ordinal 11, `0x960`), the caller-facing "wait for
the command" primitive, **does not block on a semaphore either**: its default
mode (`0` per `libcdvd-common.h` [header], "blocking wait") is a plain loop
calling `sceCdCheckCmd` and, while it reads `0`, `DelayThread(1000µs)` before
retrying; mode `1` ("check current status and return immediately" [header])
polls once. So a caller's read-and-wait sequence is: issue the N-command
(returns as soon as accepted) → **busy-poll** `sceCdSync` with a 1&nbsp;ms
sleep between checks until IRQ 2's handler has written a nonzero
`record+4`. The `thsemap` import is used only to serialize concurrent
S-/N-command senders against each other, never to signal read completion to
a blocked reader.

A second, separate two-slot scheme at module offset `0x309c` reads
`0xBF40200B` bit 0 against a stored expected value at `.bss+0x5800`; when
they differ it clears a 2-element array at `.bss+0x5804` indexed by the
caller's argument and its XOR-with-1 companion — plausibly what the debug
strings `"Intr func0 no seting"`/`"Intr func1 no seting"` diagnose when one of
the two slots has no registered callback. What blocks past this point (a
second semaphore, most likely) was not traced to an instruction — open, §9.

## 3. A 2048-byte sector read, step by step

`sceCdRead(lbn, sectors, buffer, mode)` (ordinal 6, module offset `0x34c0`;
`mode` a `sceCdRMode*` [header] `{trycount, spindlctrl, datapattern, pad}`,
all one byte):

1. Save `lbn`/`sectors`/`buffer` (the last two into `.bss` for the IRQ path).
2. Branch on `mode->datapattern` (`mode` is argument `$7`, so this is a read
   of `mode+2`): the value `1` selects N-command **`6`** directly, with a
   ×97 sector-size multiplier applied to the requested count (consistent
   with `SCECdSecS2352`-family raw formats, `libcdvd-common.h`'s
   `SCECdvdSectorType` [header]); other values pick a different multiplier
   and, further down, a command byte from a 16-entry code-address jump table
   at `.data 0x5508`. **The value `0` (`SCECdSecS2048` [header], the plain
   2048-byte case) is the routine's fall-through default**, computing
   `count << 4` before continuing into the shared tail below — this is the
   path a title's ordinary `sceCdRead(lbn, sectors, buf, NULL)` (mode
   defaulted) takes.
3. The shared tail reads `0xBF40200F` (disk type) and folds it together with
   the datapattern-derived command/length pair to pick the final submode
   byte (`0x80`/`0x83`/`0x85`/`0x86`/`0x8f`/`0x40`) written to `0xBF402006`.
4. **Arms DMA channel 3** via the private routine at `0x31b8` (args
   `(size, sector_count, dest_addr)`, called at `0x36b8`):
   ```
   CHCR (0xBF8010B8) = 0                              -- reset
   MADR (0xBF8010B0) = dest_addr                       -- IOP RAM destination
   BCR  (0xBF8010B4) = (sector_count << 16) | size     -- block count / block size
   CHCR (0xBF8010B8) = 0x41000200                      -- start
   ```
   This runs **inside `sceCdRead` itself**, before the N-command is issued —
   confirming the PS2e sibling-project lead (§7) that the DMA channel must be
   armed before the N-command, not after.
5. Builds an 11-byte N-command parameter block on its own stack and calls
   the sender (§2b) with `cmd = 6`, `len = 0xB`:

   | Offset | Field |
   | --- | --- |
   | `+0..3` | `lbn`, little-endian |
   | `+4..7` | `sectors`, little-endian |
   | `+8` | `mode->trycount` |
   | `+9` | the derived submode byte (also the one written to `0xBF402006`) |
   | `+10` | `mode->datapattern` |

6. Returns as soon as the sender's step 6 (§2b) returns — **the sector bytes
   are not yet in `buffer` when `sceCdRead` returns.** The caller must call
   `sceCdSync(mode)` (ordinal 11) and busy-poll until it reports completion,
   at which point channel 3's DMA has already landed the 2048-byte sector
   (or whatever multiple `sectors` requested) at `dest_addr`.

So the full sequence, register by register, for one 2048-byte sector: build
the 11-byte N-command block → arm `MADR`/`BCR`/`CHCR` on DMA channel 3 → write
the submode byte to `0xBF402006` → write parameter bytes to `0xBF402005` →
write `6` to `0xBF402004` → IRQ 2 fires once the drive has the data and the
DMA has moved it → the handler writes `0xBF402006`'s readback into the
result-byte slot and `record+4`'s completion word → the caller's `sceCdSync`
poll loop sees it and returns.

## 4. Disc recognition and ISO9660 path resolution

### `CD_newmedia`: reading the PVD

An internal (non-exported) routine at module offset `0x1d14`, reached from
exactly one call site — inside `sceCdSearchFile`'s own body (`0x1a2c`, below)
— when a "media checked" flag is stale. It calls the low-level sector reader
directly (module offset `0x2540`, itself a thin wrapper that argument-shuffles
into `sceCdRead`'s implementation at `0x34c0`) with `count=1, lba=0x10`
(**LBA 16 — the ISO9660 Primary Volume Descriptor**, confirmed directly from
the call site's literal argument, not assumed from convention), then compares
the sector's byte offset `+1` against the 5-byte literal `"CD001"` via a
`sysclib` memcmp-shaped stub, logging `"CD_newmedia: Illegal disc media type"`
/ `"…Read error in disc_read(PVD)"` / `"…Disc format error in cd_read(PVD)"`
on the respective failures. This is a plain, direct confirmation of the
sector-16/`"CD001"`-at-offset-1 convention — no inference needed.

### `sceCdSearchFile`: the path walk

Ordinal 10, module offset `0x9d8`, tail-calling the real body at `0x19f0`.
Signature per `libcdvd-common.h` [header]: `sceCdSearchFile(sceCdlFILE *file,
const char *name)`, `name` "in the form `'\SYSTEM.CNF;1'`" [header] —
backslash-separated, matching `SYSTEM.CNF`'s own `BOOT2 = cdrom0:\SLPS_259.
18;1` line exactly. The routine, read straight through:

1. Calls the `0x309c` completion-flag check (§2b); if it signals stale state,
   re-runs `CD_newmedia` (above) to (re-)read the PVD before trusting any
   cached directory data.
2. Requires the path's first byte to be `0x5C` (`'\'`) — anything else is a
   silent "not found" (`return 0`), matching the header's documented form.
3. **Splits the path at `'\'` into components, up to 8 levels
   (`CdlMAXLEVEL` [header])**, each component up to some fixed length,
   logging `"%s: path level (%d) error"` if the depth limit is exceeded.
4. For each level, walks a directory-entry table (helper at `0x20d4`) built
   from the current directory's own extent, comparing the component against
   each entry's name (case handling and the `;version` suffix not traced
   bit-for-bit in this pass); a miss logs `"%s: dir was not found"` and
   returns `0`.
5. On the final component's match, calls `0x2514` with the found entry and a
   fixed `0x20`-byte stride (matching `sceCdlFILE`'s own layout, below) to
   fill the caller's output structure, and returns `1`. A full disc-error or
   cache-miss path logs `"CdSearchFile: disc error"` /
   `"CdSearchFile: searching %s..."` / `"%s: found"` / `"%s: not found"` —
   the string block `docs/analysis/12` had already flagged as belonging to
   this routine.

`sceCdlFILE` [header]: `{ lsn: u32, size: u32, name: char[16], date: u8[8] }`
— `lsn` and `size` are exactly what a caller needs to hand straight to
`sceCdRead`/`sceCdReadDVDV` once a name has resolved.

### Concrete example: `SLPS-25918.iso`

Read directly against `assets/SLPS-25918.iso` (2048-byte sectors, no
ISO9660 library — raw field offsets, the same way this repository reads every
other on-disk structure):

```
PVD:  sector 16, signature "CD001" at offset 1, confirmed.
      Root directory record (PVD offset 156, 34 bytes): extent LBA 261, size 496 bytes.
Sector 17: type byte 0xFF ("CD001" volume descriptor set terminator) —
      no Joliet SVD; this disc is plain ISO9660 Level 1, 8.3 names with ";1" suffixes.
Root directory extent (LBA 261, 496 bytes — fits in exactly one sector, 9 entries):
      lba=261     size=496        <self>
      lba=261     size=496        <parent>
      lba=526637  size=57         SYSTEM.CNF;1
      lba=526638  size=1694300    SLPS_259.18;1
      lba=527829  size=838318     AMAGAMI.IMG;1
      lba=528239  size=7381386    SCENARIO.ARC;1
      lba=262     size=876        MODULES/
      lba=263     size=336        GRAPH/
      lba=264     size=386        SOUND/
SYSTEM.CNF contents (57 bytes, LBA 526637):
      BOOT2 = cdrom0:\SLPS_259.18;1
      VER = 1.02
      VMODE = NTSC
```

**Both `SYSTEM.CNF` and `BOOT2`'s target (`SLPS_259.18;1`) are top-level
entries of the root directory**, which itself fits in one 2048-byte sector.
So resolving `cdrom0:\SLPS_259.18;1` end to end costs exactly **two metadata
sector reads** (LBA 16 for the PVD, LBA 261 for the root directory) plus
however many data sectors the file itself needs (`1694300 / 2048 = 828`
sectors, rounding up) — no path-table walk and no subdirectory descent for
this disc's two boot-critical files, even though a path table (L-table at LBA
257, `0x36` bytes) and three subdirectories exist for everything else on the
disc. `SYSTEM.CNF`'s own read is the PVD, the root directory, and one data
sector (57 bytes fits in one).

## 5. `CDVDFSV`: the EE-facing RPC surface

### Five services, two threads

`CDVDFSV`'s entry (`0x0`) creates one IOP thread (entry `0x104`), which after
its own `SifInitRpc`-shaped setup spawns **two further, long-lived** service
threads — entry `0x44ac` and entry `0x457c` (`TH_C`, priority `0x51`, stack
`0x1800` each) — each building its own `SetRpcQueue` handle once and reusing
it as every `RegisterRpc`'s `qd` argument. Five `sceSifRegisterRpc` calls
(sifcmd ordinal 17) follow, `cfunc`/`cbuf` `NULL` in every one (no completion
callback; every request served synchronously off `RpcLoop`, the same shape as
`LOADFILE`, `docs/analysis/39` §4):

| Thread | `sid` | Dispatch fn | Request buffer |
| --- | --- | --- | --- |
| `0x44ac` | `0x80000592` | `0x204` | `.bss 0x67f0` |
| `0x44ac` | `0x8000059A` | `0x32d8` | `.bss 0x6920` |
| `0x44ac` | `0x80000593` | `0x41b8` | `.bss 0x63a8` |
| `0x457c` | `0x80000597` | `0x2f0` | `.bss 0x67f8` |
| `0x457c` | `0x80000595` | `0x3f3c` | `.bss 0x67b0` |

The task description that prompted this document named a `0x80000592`–
`0x8000059C` range; the five actually registered fall inside it but do not
fill it (`593`, `595`, `597`, `59A` — not, e.g., `594`/`596`/`598`/`599`).

### 5a. `sid 0x80000592`: init

Not `fno`-switched at all: `$a0` is never read anywhere in its dispatch
function (`0x204`..`0x27c`). The body reads the request buffer's first word
and calls `sceCdInit` (ordinal 4, via `CDVDFSV`'s own import stub at
`0x47f0`) with it, brackets that call with two debug prints gated on the
verbosity word at `0x5158`, stores `1` into `0x51b0` — an "initialised" flag
whose reader was not found — and acknowledges. This is a dedicated,
single-purpose **init** service, the natural binding for the EE's own
`sceCdInit(mode)` call, distinct from the large multiplexed service below.

> **Correction.** An earlier pass of this section read the body as calling
> `sceCdBreak` (ordinal 39, stub `0x48d8`) unconditionally after `sceCdInit`.
> Re-reading the whole function finds **no call to `0x48d8` anywhere in it**;
> the service does not break a command in flight. Reproduce with
> `python3 tools/romdis.py <outdir>/CDVDFSV.load --cpu iop --vma 0 --range
> 0x204 0x280`.

### 5b. `sid 0x80000593`: the 25-`fno` table

`docs/analysis/27-osdsys-payload-and-config.md` had already found this
service's dispatch function (`0x41b8`) jumping through a 25-entry table at
module offset `0x50e8` and pinned six of its cases (the NVM pair and the OSD
configuration quartet). Reading the remaining nineteen, and naming every
`CDVDMAN` ordinal reached through `CDVDFSV`'s own import-stub table (§0):

| `fno` | Wrapper | `CDVDMAN` ordinal(s) | Name(s) [header] |
| --- | --- | --- | --- |
| 1 | `0x3888` | 24 | `sceCdReadClock` |
| 2 | `0x38d0` | 25 | `sceCdWriteClock` |
| 3 | `0x3538` | 12 | `sceCdGetDiskType` |
| 4 | `0x3e60` | 8 | `sceCdGetError` |
| 5 | `0x3e88` | 14 | `sceCdTrayReq` |
| 6 | `0x35b0` | 22 | `sceCdRI` |
| 7 | `0x35fc` | 23 | `sceCdWI` |
| 8 | `0x3944` | 26 | `sceCdReadNVM` (`docs/analysis/26`) |
| 9 | `0x39b0` | 27 | `sceCdWriteNVM` (`docs/analysis/26`) |
| 10 | `0x3d10` | 36 | `sceCdDecSet` |
| 11 | `0x3d70` | 29, 54, 11 | `sceCdApplySCmd` + `sceCdApplyNCmd` + `sceCdSync` — a **generic raw-command passthrough** |
| 12 | `0x3574` | 28 | `sceCdStatus` |
| 13 | `0x3a1c` | 30 | `sceCdSetHDMode` |
| 14 | `0x3a88` | 31 | `sceCdOpenConfig` (`docs/analysis/26`/`27`) |
| 15 | `0x3b20` | 32 | `sceCdCloseConfig` |
| 16 | `0x3b94` | 33 | `sceCdReadConfig` |
| 17 | `0x3c0c` | 34, 35 | `sceCdWriteConfig` + `sceCdReadKey` — needs re-verification, §9 |
| 18 | `0x3654` | 41 | `sceCdReadConsoleID` |
| 19 | `0x36a0` | 42 | `sceCdWriteConsoleID` |
| 20 | `0x36f8` | 43 | `sceCdMV` |
| 21 | `0x37d8` | 45 | `sceCdCtrlADout` |
| 22 | `0x280` | 6, 8, 11, 20, 21, 39, 40, 44, 46, 49, 50 | **a large multi-way dispatcher** — see below |
| 23 | `0x3744` | 53 | `sceCdReadSUBQ` |
| 24 | `0x3790` | 52 | `sceCdForbidDVDP` |
| 25 | `0x3830` | 55 | `sceCdAutoAdjustCtrl` |

Every `fno`'s trampoline sets a fixed `$6 = 0x5e08` scratch/context pointer
before its wrapper call, and every wrapper converges on a shared epilogue at
module offset `0x4494` — **one fixed reply context, not sized per `fno`**,
structurally the same shape as `LOADFILE`'s fixed 8-byte answer area
(`docs/analysis/39` §4), sized to the largest single reply this service
needs (the OSD configuration quartet's 16-byte block, `docs/analysis/26`).

**The `fno` space is exactly 1..25, and an out-of-range `fno` is answered,
not dropped.** The dispatch function's own bounds check is three
instructions:

```
41c8  addiu $3, $7, -0x1        # $7 is fno
41cc  sltiu $2, $3, 0x19        # (fno - 1) <u 25
41d4  beqz  $2, 0x4470          # out of range
41dc  sll   $2, $3, 0x2         # index = (fno - 1) * 4
41e8  lw    $2, 0x50e8($1)      # table[fno - 1]
41f0  jr    $2
```

The compare is unsigned on `fno - 1`, so `fno = 0` wraps to `0xFFFFFFFF` and
is rejected along with everything above 25. The rejected path at `0x4470`
prints `"sce_cdvd block IO :unrecognized code 0x%02x\n"` — but only if the
verbosity word at `0x5158` is positive — and then **falls into the same
shared epilogue at `0x4494` every served `fno` uses**, so the client still
gets its acknowledgement, pointed at the same fixed `0x5e08` reply context
with whatever the last call left in it. An unknown `fno` is a silent no-op
here, never a stall.

This matters for a rebuild because **a retail title sends an `fno` outside
this range**: `SLPS-25918` was observed calling `fno = 0x22` on this `sid`
and carrying on regardless (`docs/analysis/43` §11 records the same title's
bind cluster). `fno 0x22` is not in this generation's table at all — it
belongs to `XCDVDFSV`, the `X`-generation module in the same archive, whose
own `0x80000593` dispatcher (`0x71e0`) is the identical shape with the bound
widened to `sltiu (fno - 1), 0x38` — `fno 1..56` — over a table at `0x86b0`.
There, `fno 0x22` reaches a trampoline at `0x73d8` and a wrapper at `0x595c`
that calls `XCDVDMAN` ordinal 75, `sceCdMmode` [header], corroborated by the
wrapper's own debug string `"Media_Mode\n"` at `0x82c4`. `XCDVDMAN` (v2.0b)
exports 129 ordinals, so 75 exists there; it is above plain `CDVDMAN`'s
62-slot ceiling (§0), exactly as §6's `sceCdMmode` row already noted.
`XCDVDFSV`'s table also has holes — `fno`s `0x24`-`0x27`, `0x2a`-`0x2f`,
`0x33`-`0x34` and `0x36`-`0x37` all point at `0x7630`, which is also that
dispatcher's own out-of-range target — which is what confirms `0x22` is a
genuine entry rather than filler.

```sh
python3 tools/irxinfo.py <outdir>/XCDVDFSV --dump-load <outdir>/XCDVDFSV.load
python3 tools/romdis.py <outdir>/CDVDFSV.load  --cpu iop --vma 0 --range 0x41b8 0x4300
python3 tools/romdis.py <outdir>/CDVDFSV.load  --cpu iop --vma 0 --range 0x4440 0x44b0
python3 tools/romdis.py <outdir>/XCDVDFSV.load --cpu iop --vma 0 --range 0x71e0 0x7240
python3 tools/romdis.py <outdir>/XCDVDFSV.load --cpu iop --vma 0 --range 0x595c 0x59b4
```

**`fno` 22 is structurally different from every other entry**: its wrapper
(module offset `0x280`) is a multi-hundred-instruction function, not the
2–4-instruction trampoline every other `fno` uses, and it reaches `sceCdRead`
(6), `sceCdGetError` (8), `sceCdSync` (11), `sceCdReadDVDV` (20),
`sceCdCheckCmd` (21), `sceCdBreak` (39), `sceCdReadCDDA` (40),
`sceCdGetReadPos` (44), `sceCdNop` (46), `sceCdstm1Cb` (49) and `sceCdSC` (50)
across what reads as several internal branches — the shape of a
sub-opcode-selected dispatcher, the request buffer almost certainly carrying
a second operation selector the way `sceCdApplyNCmd` generalizes raw
N-commands on the IOP side. **This is very likely the `fno` a title's
`sceCdRead`/`sceCdSync`/`sceCdSeek`/`sceCdGetReadPos` calls actually route
through from the EE** — every ordinal a read-and-wait sequence needs is
reachable from it — but its internal sub-opcode field was not decoded in this
pass; see §9.

### 5c. `sid`s `0x80000597` and `0x8000059A`: search and disc-ready

Both are single-purpose, and neither is `fno`-switched: `$a0` is never read
in either dispatch body.

**`0x80000597` is `sceCdSearchFile`**, and it settles `libcdvd-rpc.h`'s
`rpc4` wire shape against this ROM's own bytes. The request buffer **is** the
output struct:

| Offset | What |
| --- | --- |
| `+0x00` | the `sceCdlFILE` the call fills, 0x20 bytes |
| `+0x20` | the path, NUL-terminated |
| `+0x120` | the EE address the filled `sceCdlFILE` goes back to |

```
31c  move  $4, $16                 # $a0 = the buffer          -> sceCdlFILE *file
320  jal   0x4820                  # cdvdman.10  sceCdSearchFile
324  addiu $5, $16, 0x20           # delay slot: $a1 = buffer + 0x20 -> const char *name
330  sw    $16, 0x10($sp)          # SifDmaTransfer.src  = the buffer
334  lw    $5,  0x120($16)         #              .dest  = the EE address
33c  sw    $3,  0x18($sp)          #              .size  = 0x20
344  jal   0x46e4                  # intrman.17 CpuSuspendIntr
350  jal   0x47bc                  # sifman.7   sceSifSetDma(&transfer, 1)
35c  jal   0x46ec                  # intrman.18 CpuResumeIntr
36c  sw    $16, 0x0($2)            # reply[0] = sceCdSearchFile's return code
```

The answer therefore goes back **two ways at once**: the struct by DMA to the
address the request named, and the return code alone in the service's own
reply cell at `.bss 0x51f0`. There is no `sceSifDmaStat` wait — the reply
packet `RpcLoop` sends afterwards travels the same channel, so the data is
already ahead of it — and no drive-ready poll, retry or semaphore.

**`0x8000059A` is `sceCdDiskReady`, and it calls no `CDVDMAN` ordinal at
all.** Request word 0 is the mode: `0` spins on the N-command status register
until `(status & 0xC0) == 0x40`, anything else checks once. The answer is `2`
when ready and `6` otherwise (`SCECdComplete`/`SCECdNotReady` [header]),
written to `.bss 0x5db8`.

```
32f0  lw   $16, 0x0($5)                        # mode
3314  bnez $16, +skip                          # non-zero: check once
3370  loop: lbu [0xBF402005]; andi 0xC0; bne 0x40 -> loop
33bc  beq  $2, 0x40, +ready
33c0  addiu $3, $zero, 0x2                     # delay slot, BOTH paths: ready = 2
33d4  addiu $3, $zero, 0x6                     # not ready = 6
```

This closes §5's own open question about ordinal 13. `sceCdDiskReady` is
absent from `CDVDFSV`'s import list because **no service needs it**: the one
that answers a title's `sceCdDiskReady()` reads the register itself, the same
register `CDVDMAN`'s ordinal 13 reads. Neither the "folded into another
ordinal's answer" nor the "EE client computes it from `sceCdStatus`" reading
offered there is what happens.

### 5d. `sid 0x80000595`: a second, 14-`fno` table

Not unresolved after all, and not a different mechanism: the same dispatch
shape as `0x80000593` with a narrower bound — `sltiu (fno - 1), 0xe`, so
`fno 1..14` — over a table at module offset `0x5060`, with the same
acknowledged no-op for anything outside it (`0x417c`) and the same single
shared exit, returning one reply cell at `0x5dc8`.

| `fno` | Wrapper | `CDVDMAN` ordinal(s) reached |
| --- | --- | --- |
| 1 | `0x3f7c` → `0x4d8` | 6, 8, 11, 21 |
| 2 | `0x3f94` → `0x15ac` | 8, 11, 21, 40 (+ `sifman.7`, `intrman.17/18`) |
| 3 | `0x3fac` → `0xd8c` | 8, 11, 20, 21 |
| 4 | `0x3fc4` → `0x340c` | 9 (+ `sifman.7/8`, `intrman.17/18`) |
| 5 | `0x3fdc` | 7, then 11(2) |
| 6 | `0x4040` | 5, then 11(2) |
| 7 | `0x4074` | 15, then 11(2) |
| 8 | `0x40a8` | 38, then 11(2) |
| 9 | `0x411c` → `0x1d5c` | 6, 11, 46, 49, 50 |
| 10 | `0x414c` → `0x273c` | 11, 40, 46, 49, 50 |
| 11 | `0x4164` → `0x3c90` | 35 |
| 12 | `0x40ec` → `0x3e0c` | 11, 54 |
| 13 | `0x4104` → `0x380` | 6, 8, 11, 21, 44 (+ `sifman.7/8`, `intrman.17/18`, `thbase.33`) — the read-and-wait path |
| 14 | `0x4134` → `0x3ee0` | **none** — the same `(status & 0xC0) == 0x40 ? 2 : 6` check as `0x8000059A`, without the blocking mode |

`fno`s 1/2/3/9/10/12/13 hand their helper `($a0 = buffer, $a2 = 0x5dc8)`, so
the helper writes the reply cell itself; 5/6/7/8/14 have the trampoline store
`$v0` there.

The five helpers at `0x4d8`, `0x15ac`, `0xd8c`, `0x1d5c` and `0x273c` were
scanned for `jal` targets over a 0x400-byte window that did not reach their
`jr $ra`, so **their ordinal lists may be truncated** — they cannot contain a
wrong entry, only miss a late one. The other five terminated inside the
window and are complete. The per-`fno` request-word layouts were not read
except for `fno`s 5 and 6.

### 5e. The buffers: one request buffer per service, one reply cell

§5's table above lists what `sceSifRegisterRpc`'s `$a3` was given for each
service. That argument is the **request** buffer — where the EE's arguments
land before the call packet — not a reply buffer; the reply pointer is
whatever each dispatch function *returns*, and those are four separate
`.bss` cells. Sorting the five request buffers gives each service's size from
the gap to the next:

| `sid` | Request buffer | Size | Reply cell |
| --- | --- | --- | --- |
| `0x592` | `0x67f0` | `0x8` | — |
| `0x597` | `0x67f8` | `0x128` | `0x51f0` |
| `0x595` | `0x67b0` | `0x40` | `0x5dc8` |
| `0x593` | `0x63a8` | `0x408` | `0x5e08` |
| `0x59A` | `0x6920` | (last) | `0x5db8` |

`0x597`'s `0x128` matches its `rpc4` shape exactly (`0x20 + 0x100 + 4`), and
`0x593`'s `0x408` is what makes room for the configuration blocks
`docs/analysis/26` describes. Nothing in any dispatch body checks the EE's
`send_size` against the buffer it is about to fill.

```sh
python3 tools/romdis.py <outdir>/CDVDFSV.load --cpu iop --vma 0 --range 0x2f0  0x380
python3 tools/romdis.py <outdir>/CDVDFSV.load --cpu iop --vma 0 --range 0x32d8 0x340c
python3 tools/romdis.py <outdir>/CDVDFSV.load --cpu iop --vma 0 --range 0x3f3c 0x41b8
python3 tools/romdis.py <outdir>/CDVDFSV.load --cpu iop --vma 0 --range 0x44ac 0x4620
```

## 6. The exports a title's EE client calls

The task this document answers named a specific export list. Resolved
against §0/§5:

| Export | `CDVDMAN` ord | Served over RPC by | Semantics [header] |
| --- | --- | --- | --- |
| `sceCdInit(mode)` | 4 | `sid 0x80000592` | `mode`: `0`=init and wait, `1`=init only, `5`=deinit (`enum SCECdvdInitMode`) |
| `sceCdRead(lbn, sectors, buf, mode)` | 6 | `fno 22` of `sid 0x80000593` (probable, §5) | non-blocking; issues the N-command and arms DMA channel 3, §3; requires a follow-up `sceCdSync` |
| `sceCdSync(mode)` | 11 | `fno 11` and/or `fno 22` | `mode 0`=block until done, `mode 1`=poll once; polls `sceCdCheckCmd`'s word, §2b/§3 — not a semaphore wait |
| `sceCdGetError()` | 8 | `fno 4` | returns the IRQ-2 handler's stored result byte |
| `sceCdDiskReady(mode)` | 13 | `sid 0x8000059A` (and `fno 14` of `0x80000595`), which read the register themselves rather than calling the ordinal — §5c | polls `0xBF402005` for `(v & 0xC0) == 0x40` via a bare register spin loop in blocking mode — **not** an event-flag wait, correcting the sibling-project lead in §7. `CDVDMAN`'s own ordinal 13 polls `0xBF40200A` for `0x0A` instead; the two are different registers reaching the same answer |
| `sceCdGetDiskType()` | 12 | `fno 3` | returns `0xBF40200F` raw, mapped onto `enum SCECdvdMediaType` [header] |
| `sceCdSearchFile(file, name)` | 10 | `sid 0x80000597` — §5c | ISO9660 path walk, §4; the filled `sceCdlFILE` goes back by DMA to the EE address at request `+0x120`, the return code through the reply cell |
| `sceCdMmode(media)` | **absent** | — | ordinal `75` in current ps2sdk; **does not exist in this retail image's 62-entry export table** |
| `sceCdStatus()` | 28 | `fno 12` | raw `0xBF40200A` mapped onto `enum SCECdvdDriveState` [header] (`Stop=0`, `ShellOpen=1`, `Spin=2`, `Read=6`, `Pause=0xA`, `Seek=0x12`, `Emg=0x20`) |
| `sceCdStInit`/`StRead`/`StSeek`/`StStart`/`StStat`/`StPause`/`StResume`/`StStop` | 56–61 (`Pause`/`Resume` absent — see §0) | not yet resolved to a specific `fno` (out of scope for M2's boot-time reads; the CD streaming path is not on `SYSTEM.CNF`'s critical path) | the streaming API — `docs/analysis/12` already noted these exist; not chased further here |

## 7. What an emulator needs

PCSX2 and the sibling `PS2e` both emulate the CDVD register block directly
(they do not, for a real-BIOS/real-disc boot, HLE `CDVDMAN` the way PCSX2's
fast-boot path HLEs `EELOAD`'s `host:` calls — `docs/project-state.md` §6).
What the reference actually issues, per §§1–4 above, for the three sequences
M2 needs:

**(a) Init.** `RegisterIntrHandler(irq=2)`, `EnableIntr(2)`, `DPCR |= 0x8000`
(channel 3's DMA enable bit), `CHCR@0xBF8010B8 = 0` (channel reset) — all from
`CDVDMAN`'s entry and `sceCdInit`'s normal-mode branch (§1). `sceCdInit(mode=0)`
additionally spin-waits on `(0xBF402005 & 0xC0) == 0x40` before marking itself
initialised — an emulator's N-command status port must present that pattern
at idle or the boot never leaves this wait.

**(b) A 2048-byte data sector read.** Exactly §3's sequence: arm `MADR`/
`BCR`/`CHCR` on DMA channel 3 (`0x41000200` start value, `BCR = (count<<16)|
size`) *before* the command; write a submode byte to `0xBF402006`; write the
11-byte parameter block to `0xBF402005`; write `6` to `0xBF402004`; raise IRQ
2 once the drive has the sector and the DMA has landed it in IOP RAM; leave a
readable result byte at `0xBF402006` and set bit 0 of `0xBF402008` (the "done,
no retry" path) so the handler's `record+4` word — and therefore
`sceCdSync`'s poll — sees success.

**(c) A `SYSTEM.CNF` lookup.** Two (b)-shaped reads at N-command 6 with
`sectors=1`: LBA 16 (the PVD, `"CD001"` check) and the root directory's own
extent LBA (read from the PVD's embedded root directory record, offset 156)
— then, for this disc, one more (b)-shaped read of `SYSTEM.CNF`'s own extent.
No path-table read, no S-command, no NVM/config access is on this path at
all — an emulator only needs (a) and three iterations of (b) to get a working
title past its own `SYSTEM.CNF` parse.

## 8. Leads carried in from outside

Per `docs/project-state.md` §4's established pattern, the following were
contributed from the sibling `PS2e` project's own notes on this same
SCPH-50000 image, used here as **leads**, checked against this repository's
own tools where possible, and marked accordingly:

| Lead | Status |
| --- | --- |
| N-command params `0x06`/`0x08`: LSN and count as little-endian `u32` at bytes `0..3`/`4..7` | **Confirmed** for command `6` (§3's 11-byte block, offsets `+0..3`/`+4..7`); command `8` (raw DVD read, 2064-byte sectors) was not observed issued anywhere in this pass — `sceCdRead`'s traced branches reach `6` and `0xf`, not `8` directly; `sceCdReadDVDV` (ordinal 20) is a distinct, separately-imported export and is the more likely home for an `0x08`-shaped raw read, not chased to an instruction here |
| IOP DMA ch3 (`0x1F8010B0`) drains the sector data and must be armed before the N-command, or `CDVDMAN` reads zeros | **Confirmed directly** — §3 step 4 shows exactly this ordering inside `sceCdRead` itself |
| Drive status (`0x1F40200A`) must read `0x0A` (PAUSE) when idle with a disc; `sceCdDiskReady` "blocks on an event flag" until it matches | **Register and value confirmed**; **the "event flag" detail is corrected** — the blocking-mode path in this retail `CDVDMAN` is a bare register-polling spin loop (`lbu` from `0xBF40200A`, `bne`-loop), with no `WaitEventFlag`/semaphore call anywhere in the routine, §2b |
| DEC-SET (`0x1F40203A`, IOP-writable) arms drive-side sector decryption; bit 0 XORs with disc-key byte 4, bit 1 rotates | **Not independently verified in this pass** — `sceCdDecSet` (ordinal 36, reachable as `sid 0x80000593` `fno 10`) exists and is exported/imported exactly where expected, but its body was not disassembled; the specific bit semantics are taken on the lead's authority only |
| `N 0x0C` (`sceCdReadKey`) must return a real key, derived from the boot serial, or the OSD/boot rejects the disc; register banks `0x2020`–`0x2034` (XOR-obfuscated with `0x2039`) | **Not verified in this pass** — `sceCdReadKey` (ordinal 35) exists at the expected ordinal and is reachable, oddly, from the same `fno 17` wrapper as `sceCdWriteConfig` (§5, flagged for re-verification); the register-bank mechanism and key-derivation formula are taken on the lead's authority only. Per `docs/clean-room-policy.md` ("Trademarks and compatibility"), this is recorded as an interface a boot path needs to exist, not reproduced as a defeat mechanism |
| The PS2 logo area (LSN 0–11) is stored encrypted; `cdvdman` writes `0x53` to `DEC-SET` before PS2LOGO's read and `0x00` after | **Not verified in this pass** — plausible given `sceCdDecSet`'s existence and position, not traced to a call site |
| A second, interrupt-driven S-command engine exists in some `cdvdman` revisions, with a mailbox and a completion flag polled via `DelayThread` | **Explicitly does not apply to this image** — this retail `CDVDMAN`'s S-command sender (`docs/analysis/26`, `0x2b70`) is the plain FIFO-only, decline-on-busy design; the lead itself notes this is a *different* `cdvdman` revision from the one in this ROM |

## 9. What this pins for the rebuild

- **`CDVDMAN`'s `cdrom` device**: one `IOMAN` registration, name `"cdrom"`
  (no digit — `IOMAN`'s own unit-splitting applies exactly as it does for
  `ROMDRV`'s `"rom"`, `docs/analysis/39` §1), real `open`/`close`/`read`/
  `lseek`, all eleven other ops slots sharing one "log and return `0`" stub.
  No dedicated thread; every hardware access is synchronous on the caller's
  own thread, gated by three semaphores for mutual exclusion.
- **Two independent register blocks**: S-commands at `0xBF402016`–`18`
  (`docs/analysis/26`) and N-commands at `0xBF402004`–`0xBF40200F`, both
  poll-based with no interrupt-driven send path — only the **completion**
  side of an N-command is interrupt-driven (IRQ 2), never the send side.
- **DMA channel 3, poked directly** (`0xBF8010B0`/`B4`/`B8`), armed inside
  `sceCdRead` before the N-command that triggers the transfer, with no
  `dmacman` dependency and no separate DMA-completion interrupt taken.
- **`sceCdRead`/`sceCdApplyNCmd` return "accepted," not "done."** A rebuild's
  `sceCdSync` must busy-poll a completion word an interrupt handler writes,
  with a 1&nbsp;ms sleep between checks in blocking mode — reproducing this
  exactly (rather than a cleaner semaphore wait) matters because a title's
  own timing assumptions may depend on `sceCdSync`'s call-and-poll shape.
  This is a fact about the reference, not a design a rebuild is obliged to
  copy in software terms — but the *observable* behaviour (no completion
  before `sceCdSync` returns, no result before then) must match.
- **`cdrom0:\PATH;1` resolution is two internal routines**: `CD_newmedia`
  (LBA 16, `"CD001"` check) and `sceCdSearchFile` (backslash-split, up to 8
  levels, directory-extent walk). A rebuild's ISO9660 support needs only the
  PVD's root directory record and flat directory-record parsing — no path
  table is required for a disc shaped like `SLPS-25918.iso`, though the
  general case (files nested below `CdlMAXLEVEL` levels, or a disc that
  relies on the path table for performance) should still walk directory
  extents component by component the way the reference does, since nothing
  here confirms the path table is ever actually read by `CDVDMAN`.
- **`CDVDFSV` is five RPC services on two threads**, not one: an init
  service (`0x80000592`), two `fno` tables — the 25-entry one at
  `0x80000593` (`docs/analysis/27` plus this document) and a 14-entry one at
  `0x80000595` (§5d) — and two single-purpose services, `sceCdSearchFile`
  (`0x80000597`) and `sceCdDiskReady` (`0x8000059A`), §5c. Both tables
  reject an out-of-range `fno` by *answering* it, never by dropping it
  (§5b), which is what lets a title built against a later generation's wider
  table run against this one.
- **A rebuild's own `CDVDFSV` needs its `CDVDMAN` to be deeper than the boot
  path alone requires.** `0x80000595` reaches ordinals 5, 7, 9, 15, 20, 35,
  38, 40, 44, 49, 50 and 54 across its fourteen `fno`s (§5d); a driver that
  implements only the read/seek/search core answers thirteen of them with
  nothing.

## 10. Open questions

- **`fno 22`'s internal sub-opcode field** (which request-buffer byte
  selects among `sceCdRead`/`sceCdSync`/`sceCdReadDVDV`/`sceCdCheckCmd`/
  `sceCdReadCDDA`/`sceCdGetReadPos`/etc.) was not decoded. This is the
  single highest-value remaining gap: it is very likely the actual EE-facing
  entry point for a title's data-sector reads.
- **`0x80000595`'s per-`fno` request-word layouts**, and the tails of its
  five helpers at `0x4d8`, `0x15ac`, `0xd8c`, `0x1d5c` and `0x273c` — their
  ordinal lists in §5d are from a bounded scan that did not reach `jr $ra`,
  so they may be missing late calls.
- **`fno 17`'s apparent call to both ordinal 34 (`sceCdWriteConfig`) and
  ordinal 35 (`sceCdReadKey`)** needs re-verification by direct disassembly
  of module offset `0x3c0c` — as reported this looks like two unrelated
  ordinals sharing one wrapper, which would be unusual against every other
  `fno`'s one-wrapper-one-ordinal shape and may be a misreading of a shared
  stub address.
- **The N-command opcode-selection table at `.data 0x5508`** (a 16-entry
  table of code addresses, reached from `sceCdRead`'s tail) was not traced
  entry by entry — only that it exists and is indexed by a value derived
  from `mode->datapattern` and the disk-type byte.
- **`sceCdReadKey`'s register-bank mechanism and key-derivation formula**
  (`0xBF402020`–`34`, XOR-obfuscated with `0xBF402039`) and **`sceCdDecSet`'s
  bit semantics** (`0xBF40203A`) are recorded in §7/§8 only as unverified
  leads from the sibling project; neither was disassembled in this pass.
  Per `docs/clean-room-policy.md`, only the *interface* (that these ordinals
  exist and are reachable) is in scope for the rebuild's boot path; the
  disc-authentication mechanism itself is not a reproduction target.
- **N-command `0x08`** (the sibling lead's "raw DVD read, 2064-byte sectors")
  was not observed issued from any traced branch of `sceCdRead` in this ROM
  — whether it belongs to `sceCdReadDVDV` (ordinal 20, not disassembled
  here) or is simply not used by this particular retail `cdvdman` revision
  is open.
- **Whether DMA channel 3's own completion interrupt (IRQ `0x23`) is ever
  taken** — `CDVDMAN` never registers a handler for it, relying instead on
  CDVD's own IRQ 2 to mean "the transfer is done." Whether this is because
  IRQ 2 is defined to fire only after the DMA has landed the data, or
  because there is a narrow window where it has not, was not established
  from software alone.
- **The second completion-wait scheme at module offset `0x309c`**, reading
  `0xBF40200B`, was traced only as far as it arming a 2-slot flag array;
  what actually blocks past that point (presumably a second semaphore) was
  not traced to an instruction.
- **`sceCdMmode`'s absence**: confirmed absent from this retail image's
  62-entry export table (it is ordinal `75` in current ps2sdk headers), and
  present in `XCDVDMAN`'s 129-entry one, where `XCDVDFSV`'s `fno 0x22`
  reaches it (§5b). A title that calls it against this generation gets the
  acknowledged no-op §5b describes, not a bind failure — which is what
  `SLPS-25918` was observed relying on.
