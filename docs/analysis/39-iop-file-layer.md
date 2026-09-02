# The IOP File Layer: IOMAN, ROMDRV, MODLOAD and LOADFILE

`docs/analysis/34-sif-command-protocol.md` §6 and `docs/spec/03-boot-chain.md`
BOOT-12e already pinned the **client**'s side of `SifLoadModule("rom0:NAME")`:
a 512-byte RPC request to server `sid = 0x80000006` — `arg_len` at `+0`, the
path at `+8`, arguments at `+0x104` — answered with an 8-byte reply, `{ id_or_
error, modres }`. What was left open is everything on the **IOP** side of that
call: how the path becomes a file, how the file becomes a running module, and
what actually answers the RPC. That is four modules — `IOMAN` (the file-
manager framework), `ROMDRV` (the ROM archive published as a device), `MODLOAD`
(load–relocate–start), and `LOADFILE` (the RPC front end) — and this document
reads all four. `docs/analysis/10-module-loading-and-boot-configs.md` already
covers the generic loaded-module entry/teardown convention (`entry(argc,argv,0,
module_record)`, `return&3` residency, module-record fields `+0x10`/`+0x14`)
and is not repeated; `docs/analysis/11-sif-and-rom-driver.md` already covers
`ROMDRV`'s boot-time registration call shape and the ROMDIR scan; `docs/
analysis/05-sysmem-and-loadcore.md` covers `LOADCORE`'s registration and
import-binding algorithms structurally. This document is downstream of all
three.

```sh
python3 tools/romdir.py assets/SCPH-50000.bin --extract <outdir>   # outside the repo
for m in IOMAN ROMDRV MODLOAD LOADFILE LOADCORE; do
  python3 tools/irxinfo.py <outdir>/$m --exports --imports
done
python3 tools/irxinfo.py <outdir>/IOMAN    --dump-load <outdir>/IOMAN.text
python3 tools/irxinfo.py <outdir>/ROMDRV   --dump-load <outdir>/ROMDRV.text
python3 tools/irxinfo.py <outdir>/MODLOAD  --dump-load <outdir>/MODLOAD.text
python3 tools/irxinfo.py <outdir>/LOADFILE --dump-load <outdir>/LOADFILE.text
python3 tools/romdis.py <outdir>/IOMAN.text    --cpu iop --vma 0 --range START END
```

ps2sdk's headers name the ordinals and structure fields cited below —
`iop/system/ioman/include/ioman.h`, `iop/system/modload/include/modload.h`,
`iop/system/loadcore/include/loadcore.h`, `iop/fs/romdrv/include/romdrv.h` and
`iop/kernel/include/kerr.h` — every such use is marked "[header]"; the binary
is the authority, and several places below correct or narrow what the header
alone would suggest.

## 0. Ordinal maps

Own export tables (ordinal → module offset), from `irxinfo --exports`,
cross-checked against the headers:

**`IOMAN`** (25 exports; `ioman.h` [header]) — `0` entry, `1–3`/`19`/`22–24`
share one `jr $ra` reserved stub at `0x115c`, `4` open `0xe8`, `5` close
`0x3a4`, `6` read `0x28c`, `7` write `0x318`, `8` lseek `0x1bc`, `9` ioctl
`0x434`, `10` remove `0x690`, `11` mkdir `0x758`, `12` rmdir `0x820`, `13`
dopen `0x4c0`, `14` dclose `0x600`, `15` dread `0x584`, `16` getstat `0x8e8`,
`17` chstat `0x9c0`, `18` format `0xaa4`, `20` AddDrv `0xe8c`, `21` DelDrv
`0xf44`. Every ordinal lands on a distinct body and the reserved slots share
one address — the same corroboration pattern `docs/analysis/34` §0 used for
`SIFCMD`/`SIFMAN`.

**`ROMDRV`** (6 exports of its own `romdrv` v2.01 library; `romdrv.h` [header]
documents only ordinals 4/5, for a newer `romdrv`/`romdrvX` split — this
retail table is settled below by disassembly, not by the header): `0` →
`0x74`, `1–3` share a stub at `0x790`, `4` → `0xe4`, `5` → `0x1a4`.

**`MODLOAD`** (16 exports; `modload.h` [header]) — `0` entry, `1–3` share a
stub at `0x15a8`, `4` ReBootStart `0x1518`, `5` LoadModuleAddress `0x1bc`, `6`
LoadModule `0x1f0`, `7` LoadStartModule `0x26c`, `8` StartModule `0x358`, `9`
LoadModuleBufferAddress `0x214`, `10` LoadModuleBuffer `0x248`, `11`
LoadStartKelfModule `0x440`, `12` SetSecrmanCallbacks `0x1278`, `13`
SetCheckKelfPathCallback `0x1290`, `14` GetLoadfileCallbacks `0x12a0`, `15`
IsIllegalBootDevice `0xbb8`. **Correction to the header**: ps2sdk's
`modload.h` names ordinal 3 `GetModloadInternalData`, but here it is the
*same* address as the reserved slots 1–2 (`0x15a8`) — this retail `MODLOAD`
1.01 does not implement it; ordinal 3 is a plain reserved slot.

**`LOADFILE`** exports nothing (0 entries) — it is a pure RPC service; no
other module imports from it, and its only public surface is the SIF RPC
server bound at `sid = 0x80000006` (`docs/analysis/34` §3, §6).

**`LOADCORE` ordinals `MODLOAD` imports** (12 stubs; `loadcore.h` [header]):
`3` GetLibraryEntryTable/GetLoadcoreInternalData (the header itself gives this
ordinal two names across versions), `4` FlushIcache, `6` RegisterLibraryEntries,
`8` LinkLibraryEntries, `9` UnLinkLibraryEntries, `12` QueryBootMode, `16`
RegisterModule, `17` ReleaseModule, `20` AddRebootNotifyHandler, `21`
SetCacheCtrl, `22` ProbeExecutableObject, `23` LoadExecutableObject.
`docs/analysis/05` covers `LOADCORE`'s own bodies for registration (ord 6) and
import-binding (ord 8); this document only pins which of these `MODLOAD` calls
and in what order (§3).

## 1. IOMAN: the driver framework

`IOMAN`'s entry (`0x0`) registers its own export table (loadcore ord 6),
zeroes a 16-slot driver table at `0x1390` (`0x40` bytes) and a 16-entry
file-descriptor table at `0x13d0` (`0x100` bytes), then installs a console
pseudo-device (`DelDrv("tty")`, `AddDrv(&ttyDescriptor@0x1360)`, opens
`tty00:` twice for the initial stdin/stdout fds) and unconditionally returns
`0` — it never inspects `argc`/`argv`/the module record and does not use the
`return&3` convention, being a statically-present core module rather than one
that opts into staying resident.

### The device descriptor and its ops table

`AddDrv` (ord 20, `0xe8c`) takes an `iop_device_t*`, keeps the **pointer**
(no struct copy) in the next free slot of the 16-entry driver table at
`0x1390` (stride 4 bytes — a pointer array, not descriptor storage), and
validates the driver by calling `ops->init(device)` (loaded at `device+0x10`
then `ops+0x00`) — a negative return rolls the slot back and `AddDrv` returns
`-1`. `type`/`version`/`desc` (offsets `0x4`/`0x8`/`0xC`) are never read by any
code IOMAN executes; they are corroborated only by the static `tty` descriptor
literal's data layout (`0x1360`: name-ptr, `1`, `1`, desc-ptr, ops-ptr), not by
an executed load — flagged as an inference, not a disassembled fact.
`DelDrv` (ord 21, `0xf44`) linear-scans the same table comparing
`driverTable[i]->name` (offset `0`) against the argument, calls `ops->deinit`
(`ops+0x04`) on a match, clears the slot and always returns `0` regardless of
`deinit`'s own result; no match returns `-1`.

Every dispatcher's `ops+N` load pins the slot order exactly as `ioman.h`
[header] declares it — confirmed by grepping the `lw ; jalr` pair inside each:

| field | ops+ | field | ops+ | field | ops+ |
| --- | --- | --- | --- | --- | --- |
| init | `0x00` | write | `0x18` | dclose | `0x34` |
| deinit | `0x04` | lseek | `0x1c` | dread | `0x38` |
| format | `0x08` | ioctl | `0x20` | getstat | `0x3c` |
| open | `0x0c` | remove | `0x24` | chstat | `0x40` |
| close | `0x10` | mkdir | `0x28` | | |
| read | `0x14` | rmdir | `0x2c` | dopen | `0x30` |

No deviation anywhere — this is the same struct as `iop_device_ops_t` in
`ioman.h` [header], field for field.

### The file-descriptor table and path parsing

`open` (`0xe8`) allocates from the 16-entry table at `0x13d0`: each 16-byte
`iop_file_t` record is "free" when its `device` field (offset `8`) is zero, and
the allocator (`0xb98`) claims the first such slot by writing a non-null
sentinel there. On success `open` returns the **fd index**
(`(entry_ptr − 0x13d0) >> 4`, i.e. the raw table slot number, not index plus an
offset); on a full table it logs `"out of file descriptors"` and returns `-24`.

Path parsing (`0xd28`) finds the `:` separator (`strchr`), copies the device
name up to it into a local buffer, then walks **backward** from the colon over
any trailing decimal digits and — this is the mechanism that turns `rom0:`
into device `rom` unit `0` — parses that digit run with `strtol(...,10)` into
`*outUnit` (default `0` if there is no trailing digit) and **truncates those
digits out of the copied name** before the device-table lookup
(`0xc80`, a `strcmp` against each `driverTable[i]->name`). This is confirmed
against real data: `ROMDRV` registers the device name `"rom"` (four bytes,
`"rom\0"`), not `"rom0"` (§2) — `rom0:`/`rom1:` are the same device, split by
unit number entirely inside `IOMAN`'s path parser, before the driver ever sees
a digit.

`open`'s dispatch to the driver (`0x174`): `a0` is the freshly-allocated
`iop_file_t*` with `mode`/`unit`/`device` already filled in (`0x158`-`0x160`),
`a1` is the **remainder pointer the path parser returned** — the tail of the
*original* string past the colon (not a copy, and not the full `"dev:tail"`
string), `a2` is `IOMAN`'s own `flags` argument passed straight through. This
matters for `ROMDRV`'s own lookup in §2: it receives exactly the post-colon
name, nothing before it. `read`/`lseek`/`close` (`0x28c`/`0x1bc`/`0x3a4`) all
validate the fd via a shared helper (`0xc3c`: range-check `fd<16`, then
`device==0` means "not open") and dispatch through the same ops slots with the
caller's own arguments; `lseek` additionally range-checks its `whence`
argument (`0..2`) before dispatching, rejecting anything else with its own
error rather than calling the driver.

### Error codes

An unrecognized device name (no `:` in the path, or no driver matches) makes
`open` return **`-19`** — a small code that does not match any `kerr.h`
[header] constant (`KE_UNKNOWN_MODULE −202`, `KE_NOFILE −203`, `KE_NO_ROMDIR
−162` are all in a different numeric family); `19` happens to equal the
POSIX `ENODEV` value, noted as an observation rather than a label found
anywhere in this binary. When the **driver's own** `ops->open` returns
negative, `IOMAN` passes that value back to its caller completely unmodified
(only a side "last errno" global at `0x1380` records the positive magnitude)
and releases the fd slot it had provisionally claimed — the same
unmodified-passthrough shape recurs in `remove`/`mkdir`/`rmdir`/`getstat`/
`chstat`/`dopen`/`format`.

### The STDIO link

`docs/analysis/08-c-library-and-heap.md` already established that `STDIO`
imports `ioman` ordinals 6 and 7; the export table above confirms those are
genuinely `read`/`write`.

## 2. ROMDRV: the ROM archive as a device

`docs/analysis/11-sif-and-rom-driver.md` already disassembled `ROMDRV`'s
entry (`0x0`-`0x50`): register the `romdrv` export table (loadcore ord 6),
call an "internal init" at `0x74`, build two descriptor pointers at `+0x86c`
and `+0x8bc`, hand them to `ioman` ordinals **21 then 20**, and turn the
sign bit of the return into the residency code. Resolving what those two
pointers actually are settles that ordering: `+0x86c` is not a descriptor at
all — it is the four bytes `"rom\0"`, i.e. `DelDrv("rom")` runs first,
defensively removing any stale registration, and `+0x8bc` is the real
`iop_device_t`, registered second by `AddDrv`. Its fields, read directly from
the module's data (all values relocated by adding the load base, elided
below since it is 0 here):

```
+0x8bc: name    = 0x86c   ("rom\0")
+0x8c0: type    = 0x10    (IOP_DT_FS [header])
+0x8c4: version = 1
+0x8c8: desc    = 0x860   ("ROM/Flash\0\0\0" — already noted by doc 11)
+0x8cc: ops     = 0x878
```

### Units, not names: `rom0`/`rom1` are one device

Grepping the extracted file for `"rom0"`, `"rom1"` and `"rom:"` finds none of
them — only the one four-byte string `"rom\0"` at `0x86c`. So `rom0:`/`rom1:`
are never two device registrations; they are `IOMAN`'s unit-number parsing
(§1) applied to the single `"rom"` device. `ROMDRV`'s own `open` (ops slot
`0xc` → module offset `0x228`) reads `f->unit` (`iop_file_t+0x4`) right away
and range-checks it `<4` (else `-6`), then indexes a four-entry
`struct RomImg romImages[4]` array at `0x910` (`{ImageStart, RomdirStart,
RomdirEnd}`, 12 bytes each — `romdrv.h`'s [header] `struct RomImg`, size
`0x30` matching the `.bss` region the "internal init" zeroes) by that unit
number. This is exactly the model `romAddDevice(int unit, const void *image)`
[header]'s signature implies: one device, up to four unit-numbered images.
`ROMDRV`'s own "internal init" (`0x74`-`0xe4`) zeroes `romImages[]` (`0x30`
bytes), an 8-entry, 12-byte "matched file" table at `0x940` (`0x60` bytes,
`ROMDRV_MAX_FILES=8` [header]) and an 8-entry, 8-byte "open handle" table at
`0x8d0` (`0x40` bytes), then **self-registers unit 0** directly — it calls the
self-locating ROMDIR scan (`0x540`, the same `"RESE"`/`0x54` constants `doc
11` already found, here over the window `0xBFC00000..0xBFC40000` — 256 KiB,
narrower than the boot block's 512 KiB window from `BOOT-6b`) and stores the
result straight into `romImages[0]`. Units 1–3 are left empty for a *later*
caller of the exported `romAddDevice` (ordinal 4, see below) to fill — `rom1:`
would exist only if some other module registers an image there; nothing in
this ROM does.

### `open`/`read`/`lseek`/`close`

`open` (`0x228`), after the unit check: also requires `flags==1` (else `-13`
— matching `MODLOAD`'s own `open(name, flags=1, ...)` call in §3), then
linear-scans the ROMDIR entries of `romImages[unit]` for a name match
(`docs/analysis/01-rom-layout.md`'s `{name[10], ExtInfoEntrySize:u16,
size:u32}`, 16-byte, zero-terminated format — not re-derived here), claims a
free slot of the 8-entry open-handle table (critical-section-guarded,
intrman ord 17/18), and on a match stores the matched ROMDIR record into
`f->privdata` (`iop_file_t+0xC`); no match returns `-2`, a full handle table
returns `-12`. `read` (`0x3e8`) clamps the requested length to
`filesize − position`, `memcpy`s from `romImages[unit].ImageStart + position`
via the `sysclib` copy stub, advances the stored position and returns the
actual (possibly short) byte count — never refuses a read past EOF, just
truncates it. `lseek` (`0x49c`) implements all three POSIX-style modes
(`SEEK_SET`/`CUR`/`END`, `0`/`1`/`2` — anything else is `-22`), clamps the
result to `[0, filesize]`, and returns the new absolute position. `close`
(`0x388`) validates the handle index (`<8`, else `-9`) and requires the slot
to currently be marked in-use (else also `-9`), then clears it and returns
`0`. None of `-2`/`-6`/`-9`/`-12`/`-13`/`-22` match `kerr.h` [header]'s
kernel-error range; they read as a small IOMAN/`io_common`-style file-error
family distinct from `KE_*`, and their exact canonical names were not found
in this pass.

The remaining nine ops slots (`ioctl`, `remove`, `mkdir`, `rmdir`, `dopen`,
`dclose`, `dread`, **`getstat`**, **`chstat`**) all point at the *same*
two-instruction `jr $ra; move $2,$zero` stub at `0x64` — a flat, read-only
device implements none of them, including `getstat`/`chstat`; nothing in this
driver reports file size except by opening and seeking. `write` (`0x494`) is
its own dedicated two-instruction stub, `jr $ra; addiu $2,$zero,-5` — distinct
from the shared `0x64` stub, i.e. writing is a deliberate hard error rather
than a silent no-op.

### The `romdrv` library's own six exports

Resolved directly against `romdrv.h` [header]'s error constants, which appear
verbatim at the exact call sites — the strongest possible confirmation:

- **Ordinal 0** (`0x74`) is the "internal init" function itself, re-exported —
  the same address the entry calls directly. Not documented by ps2sdk's
  header at all; a leftover raw entry point, consistent with `docs/analysis/
  05`'s note that "slot 0 is the entry" fails for some libraries.
- **Ordinals 1–3** share the reserved stub at `0x790`.
- **Ordinal 4** (`0xe4`) is `romAddDevice(unit, image)` [header]: range-checks
  `unit<4` (else returns **`-160`**, `ROMDRV_ADD_FAILED` [header, exact
  match]), checks the slot is empty (else also `-160`), re-runs the
  self-locating scan against the caller's image and stores it into
  `romImages[unit]` on success (`0`) or returns **`-162`**,
  `ROMDRV_ADD_BAD_IMAGE` [header, exact match] if the scan doesn't find a
  valid `RESET`-headed archive.
- **Ordinal 5** (`0x1a4`) is `romDelDevice(unit)` [header]: same range check,
  clears `romImages[unit].ImageStart` on success, or returns **`-161`**,
  `ROMDRV_DEL_FAILED` [header, exact match] for an out-of-range or already-
  empty unit.

The three exact constant matches (`-160`/`-161`/`-162`) confirm the ordinal
identification beyond reasonable doubt; `romGetDevice` (ordinal 6 in the
newer `romdrvX` header table) does not exist in this retail `romdrv` 2.01 —
the table stops at 6 entries (ordinals 0–5).

## 3. MODLOAD: load, relocate, link, start

`ReBootStart`(4)/`LoadModuleAddress`(5)/`LoadModule`(6)/`LoadStartModule`(7)/
`StartModule`(8)/`LoadModuleBufferAddress`(9)/`LoadModuleBuffer`(10) are not
seven independent implementations. `LoadModule` and `LoadModuleBuffer` are
one-line wrappers that call the `…Address` variant with `addr=offset=0`; all
four `…Address`/`LoadStartModule`/`StartModule` ordinals build a small
request struct on the stack and call one shared dispatcher (`0x5a0`) that
switches on a mode word (`1`=LoadModuleAddress, `2`=StartModule, `3`=
LoadStartModule, `4`=LoadModuleBufferAddress). Mode `2` locates an
already-loaded module by id (`0x70c`); modes `1`/`3`/`4` load from a name or
buffer (`0xd24`); modes `2` and `3` converge on the same tail
(`0x6a4` → `0x9ac`, the argc/argv-and-entry-call step, itself the code
`docs/analysis/10` already disassembled at `0xB10`).

### Reading the file: no ROM shortcut

The file-read routine (`0xe00`, called from the load core at `0xd24`+`0xd80`)
goes through **ordinary `IOMAN` ordinals only** — `open`(4)/`lseek`(8)/
`read`(6)/`close`(5) — with no special-cased ROM path anywhere in `MODLOAD`'s
text:

```
e24: jal <ioman open>     ; open(name, flags=1, ...)
e30: bgez $16, ...        ; fd<0 -> KE_NOFILE, see below
e48: jal <ioman lseek>     ; lseek(fd, 0, 2)   SEEK_END -> file size
e84: jal <sysmem alloc>      ; alloc(mode=1, size=filesize)     [sysmem ord 4]
ebc: jal <ioman lseek>         ; lseek(fd, 0, 0)   rewind
ecc: jal <ioman read>            ; read(fd, buf, size) -- one call, whole file
ed0/ef0: jal <ioman close>
```
So `rom0:NAME` is resolved by `MODLOAD` exactly the way any user program's
`open("rom0:NAME",...)` would be — through `IOMAN`'s path parser and
`ROMDRV`'s driver, with the file size obtained via `lseek`-to-end rather than
`getstat` (which `ROMDRV` doesn't implement anyway, §2), and the whole file
read in a single call into a `sysmem`-ordinal-4 allocation sized exactly to
the file.

### Validating and placing the ELF: Probe sizes, Load places

No ELF magic-number comparison exists anywhere in `MODLOAD`'s own text — that
check is entirely `LOADCORE`'s. The core probe/alloc/load/link/register
routine (`0xf38`, called from `0xd24`+`0xdac`) calls loadcore ordinal **22**
(`ProbeExecutableObject`) *before* any allocation, on the raw file buffer, to
parse headers and compute a size/type; a returned type outside `{1,2,3,4}` is
rejected as `KE_ILLEGAL_OBJECT`. Only after that does it call `sysmem`
ordinal 4 to allocate the destination, then loadcore ordinal **23**
(`LoadExecutableObject`) on the *same* raw image pointer plus the now-
completed struct (destination included) — confirming ordinal 22 only sizes
and ordinal 23 is the one that actually copies/relocates sections into their
final address. The temporary file-read buffer is freed (`sysmem` ord 5)
immediately after `LoadExecutableObject` returns, confirming the raw bytes
and the final relocated module live in separate allocations.

### Linking, registering, and the module id

Order is **link, then flush, then register** — and, contrary to what
`docs/analysis/05`'s "an out-of-range import degrades to a `jr $ra`, not a
crash" might suggest about error-*tolerance*, `MODLOAD` **does** check
`LinkLibraryEntries`'s own return value:

```
10b0: jal <loadcore 8>    ; LinkLibraryEntries(loaded_base, &probe_struct)
10b8: bgez $2, 0x10e4      ; negative -> free the allocation, return KE_LINKERR (-200)
10e4: jal <loadcore 4>       ; FlushIcache()
10f8: jal <loadcore 16>        ; RegisterModule(module_record)
```
`RegisterModule`'s own return value is discarded — the function returns the
locally-computed record pointer regardless. The module record is the first
`0x30` bytes of the allocation (the image body starts at `alloc+0x30`), and
its layout matches `loadcore.h`'s `ModuleInfo_t` [header] exactly, confirmed
by the one field this pass exercises directly — the **module id**, a 16-bit
value read straight from **record `+0xC`**:

```
6a8: lhu $3, 0xc($4)   ; id
6b0: sw  $3, 0(result)   ; *result = id, on both the LoadStartModule and StartModule success paths
```
This is `ModuleInfo_t.id` [header] precisely (the struct's field order —
`next, name, version, newflags, id, flags, entry, gp, text_start, text_size,
data_size, bss_size` — puts `id` at byte offset `0xC`, and `entry`/`gp` at
`0x10`/`0x14`, matching `docs/analysis/10`'s already-confirmed module-record
offsets exactly). **No counter of any kind exists in `MODLOAD`'s own `.data`/
`.bss`** (which holds only a thread-owner/semaphore bookkeeping struct for
the recursion guard around the shared dispatcher) — the id must be assigned
somewhere inside `ProbeExecutableObject`, `LoadExecutableObject` or
`RegisterModule` itself, all `LOADCORE`-internal and out of scope for this
pass (`docs/analysis/05`). This is consistent with, but does not directly
confirm, ids counting every module loaded since boot (the observed `id=25`
for `SIO2MAN`, `docs/analysis/34` §6) — `loadcore.h`'s own `lc_internals_t`
[header] carries `module_count`/`module_index` fields that are the natural
place for such a counter to live, cited here as a plausible mechanism, not a
disassembled one.

### argc/argv, and `*result`

The entry-call helper (`0x9ac`, called with `module_record, name, arglen,
args, &result`) walks `args` twice using the `sysclib` `strlen` stub: once to
count NUL-terminated tokens up to `arglen` bytes (argc starts at `1`, for the
name itself), once to copy the module's own name string as `argv[0]` and
`memcpy` the whole `args` blob immediately after it, then re-walk the copy
building an `argv[]` pointer array (both buffers carved from `MODLOAD`'s own C
stack, not `sysmem`), NUL-terminating `argv[argc]`. After the entry call
(`0xB10`, per `docs/analysis/10`), `*result` receives the entry function's raw
return value completely unmodified — the same value `return&3` tests for
residency.

### Error codes

| Constant | Value | Site | Trigger |
| --- | --- | --- | --- |
| `KE_ILLEGAL_OBJECT` | −201 | `0xd64` | `IsIllegalBootDevice` (ord 15) rejects the path |
| `KE_ILLEGAL_OBJECT` | −201 | `0xf98`/`0xfb4` | `ProbeExecutableObject`'s type code not in `{1,2,3,4}` |
| `KE_NOFILE` | **−203** | `0xe38` | `open` (ioman ord 4) returned negative — the reference's observed `rom0:NOSUCH -> -203` (`docs/analysis/34` §6) originates exactly here |
| `KE_FILEERR` | −204 | `0xe5c` | `lseek(fd,0,SEEK_END)` returned ≤0 |
| `KE_FILEERR` | −204 | `0xeec` | `read()` returned fewer bytes than requested |
| `KE_NO_MEMORY` | −400 | `0xea0` | `sysmem` ord 4 alloc for the raw file buffer failed |
| `KE_NO_MEMORY` | −400 | `0x108c` | `sysmem` ord 4 alloc for the final image failed (both allocation strategies) |
| `KE_MEMINUSE` | −205 | `0x1008` | a fixed-address allocation failed and a follow-up `sysmem` ord 9 query found the target range already occupied |
| `KE_UNKNOWN_MODULE` | −202 | `0x65c` | `StartModule`'s by-id lookup (`0x70c`) found nothing |
| `KE_LINKERR` | −200 | `0x10d8` | `LinkLibraryEntries` (loadcore ord 8) returned negative — checked, not ignored |

## 4. LOADFILE: the RPC front end

`LOADFILE`'s `.iopmod` name is `LoadModuleByEE`, and it exports nothing — its
whole public surface is the SIF RPC server `docs/analysis/34` §3/§6 already
bound to `sid = 0x80000006`. Its entry (`0x0`-`0xc4`) does not run the server
itself: it builds an IOP `ThreadParam` (priority `0x58`, stack `0x1000`,
entry `0xc8`) and hands it to `thbase` ordinals 4/6 (`CreateThread`/
`StartThread`) — the RPC service, including its initial handshake, runs on a
dedicated thread, not the boot thread.

### Boot registration: one thread, one server, one loop

The spawned thread body (`0xc8`-`0x14c`) makes exactly four `sifcmd` calls, in
an order and with argument shapes that match the well-known ps2sdk
`sifcmd.h` numbering — `InitRpc`=14, `RegisterRpc`=17, `SetRpcQueue`=19,
`RpcLoop`=22 — though that exact ordinal-to-name binding rests on shape and
call order here, not on a disassembled `SIFCMD` in this pass (`docs/analysis/
34` §0 gives `SIFCMD`'s own offsets for these, but not a second, independent
ordinal confirmation):

```
d0: la $4, "Load File service.(99/11/05)\n"
d8: jal <stdio ord4>              ; printf(...)
e0: jal <sifcmd ord14>            ; InitRpc(0)
e8: jal <thbase ord20>            ; -> $2, kept as this thread's id
fc: jal <sifcmd ord19>            ; SetRpcQueue(queue=0x1c20, threadid=$2)
10c: lui $5,0x8000 ; ori $5,$5,6  ; $5 = 0x80000006 -- the sid, a literal immediate
114: la $6, 0x4c4                 ; func = the fno dispatch wrapper
120: la $7, 0x1c80                ; buf  = the request/reply scratch area
124: sw $zero, 0x10($sp)          ; cfunc = NULL
128: sw $zero, 0x14($sp)          ; cbuf  = NULL
12c: jal <sifcmd ord17>           ; RegisterRpc(sd=0x1c38, sid, func, buf, cfunc, cbuf, qd=0x1c20)
134: jal <sifcmd ord22>           ; RpcLoop(queue=0x1c20) -- never returns
```
`queue` (`0x1c20`), `sd` (`0x1c38`) and `buf` (`0x1c80`) are all in `LOADFILE`'s
own `.bss`; there is exactly one `CreateThread`/`StartThread` pair and exactly
one `RpcLoop` call in the whole module — this is the entire server, not one
of several.

### The `fno` dispatch table

The registered `func` (`0x4c4`) receives `(fno, buf, size)` per `docs/
analysis/34` §3; it drops `size` entirely, range-checks `fno<6` (`sltiu
$2,$4,0x6`; out of range returns `$2=0` — no reply at all, not an error code),
and tail-calls a table entry with only `buf`:

| `fno` | target | what it does |
| --- | --- | --- |
| 0 | `0x150` | loads an IOP module via `MODLOAD` — §"fno 0" below |
| 1 | `0x240` | loads an **EE** ELF via `LOADFILE`'s own file-reading code, not `MODLOAD` — see below |
| 2 | `0x420` | "set val add %x type %x" — pokes a byte/half/word at a caller-given address |
| 3 | `0x364` | "get val add %x type %x" / "ret %x" — peeks a byte/half/word from a caller-given address |
| 4 | `0x1fc` | `modload` ord 11 `LoadStartKelfModule(path=buf+8, arglen=buf+0, args=buf+0x104, &buf_reply)` — same request shape as `fno 0`, no `IsIllegalBootDevice` check |
| 5 | `0x2fc` | an internal helper at `0x143c`, sibling of `fno 1`'s `0x13cc` — not decoded in this pass |

`fno` 2 and 3 are a raw memory debug backdoor reachable over the SIF RPC link
from the EE — worth flagging on its own; nothing in this pass found any
privilege check gating them.

### The newer `LOADFILE` a title actually talks to, and `fno 0xff`

Every retail title reboots the IOP from its own `IOPRP` image before it loads
a module, so the `LOADFILE` its `sceSifLoadModule` reaches is that image's,
not `rom0`'s. `SLPS-25918`'s `MODULES/IOPRP310.IMG` carries one:

```sh
# ISO9660: root -> MODULES -> IOPRP310.IMG;1, then the archive inside it
python3 tools/romdir.py <outdir>/IOPRP310.IMG --extract <outdir>/ioprp
python3 tools/irxinfo.py <outdir>/ioprp/LOADFILE --dump-load <outdir>/lf.text
python3 tools/romdis.py --cpu iop --vma 0 <outdir>/lf.text
```

It is the same module one revision on — `.iopmod` name `LoadModuleByEE`,
version `2.02` against `rom0`'s — and its dispatcher differs in two ways.
The table is eleven entries at `0x1ca8`, not six, so the range check reads
`sltiu $2,$4,0xb`. And the out-of-range arm is no longer unconditional: at
`0x5b0` it tests `fno == 0xff` and, when it matches, copies the word at
`0x1c9c` into a four-byte answer buffer at `0x1f80` and returns that buffer.
Every other out-of-range `fno` still returns `$2 = 0` — no reply at all —
so §4's statement above holds for everything but this one number.

The word at `0x1c9c` is four ASCII digits, `3100`. It is the tail of the
`PsII<name><release>` tag every module in that image carries
(`PsIIloadfile3100`), and the tag's last four characters are the **release
number of the IOP kernel the module belongs to**. `fno 0xff` is therefore a
version query: it answers with the release of the kernel serving the call.

**What a title does with it.** `SLPS-25918`'s `sceSifLoadFileInit`
(`0x00180eb0` in `SLPS_259.18;1`) binds `sid 0x80000006` and immediately
sends `fno 0xff` with a zero-byte request and a four-byte receive buffer,
then keeps the answer. `sceSifLoadModule` (`0x00181078`) calls that, then a
gate at `0x00180fb0`, and only then builds an `fno 0` request. The gate
compares the four bytes against the release in the title's own
`PsIIlibkernl3100` tag (the literal `3100` at `0x00281ffc`), against a
release the title stores at `0x002820bc`, and those two against each other;
if none match, `sceSifLoadModule` returns `0xfffefffc` without sending
anything. A `LOADFILE` that does not implement `fno 0xff` therefore blocks
every module load, silently — the RPC itself completes normally.

The release is **per title**, not per console: `SLPS-25918` ships
`IOPRP310.IMG` and tags its libraries `3100`, `SLPS-25418` ships
`IOPRP280.IMG` and tags them `2800`. So the honest answer is the release of
whichever image the title's reset request named, which only a real `UDNL`
merge (`docs/analysis/45`) can know.

```sh
grep -a -o 'PsII[A-Za-z_ ]\{8\}[0-9]\{4\}' <outdir>/SLPS_259.18 | sort -u
```

### `fno 0`, in full — and where `-203` actually comes from

```
150: ... ; buf = the 512-byte request docs/analysis/34 §6 already decoded
160: addiu $17, $18, 0x8      ; path = buf+8
16c: jal <modload ord15>      ; IsIllegalBootDevice(path)
174: bnez $2, 0x1d8           ; illegal -> $3 = -0xc9 (-201), skip LoadStartModule entirely
188: addiu $16, $18, 0x104    ; args = buf+0x104
18c: lw $6, 0x0($18)          ; arg_len = *(buf+0)
1ac: jal <modload ord7>       ; LoadStartModule(path, arg_len, args, &0x1e84)
1c0: move $5, $2              ; $2 = LoadStartModule's own return: id | error
1c8: sw $5, -0x4($16)         ; word0 @ 0x1e80 = id | error
1d4: move $2, $16             ; return &0x1e80 (the reply pointer)
```
This confirms every offset `docs/analysis/34` §6 pinned from the client's
side — `arg_len@+0`, `path@+8`, `args@+0x104` — and that `LOADFILE` passes
`path` to `LoadStartModule` completely unmodified, no prefix, no rewrite.
`LoadStartModule` writes the module's own return value (`modres`) directly
into the reply's second word via its own `&0x1e84` out-parameter (`docs/
analysis/34`'s `{id_or_error, modres}` answer, confirmed end to end); this
handler then stores its own `$2` (the id-or-error) into the first word,
`0x1e80`.

The illegal-boot-device short-circuit here answers **`-201`**
(`KE_ILLEGAL_OBJECT`) — the *same* code and the *same* `IsIllegalBootDevice`
call `§3`'s error table already found `MODLOAD` making internally, at its own
offset `0xd64`, as part of `LoadStartModule`'s own "resolve source" step. So
the check genuinely runs twice for this path — once here, before
`LoadStartModule` is even called, once again inside `LoadStartModule` itself
(which is reachable directly by other IOP callers that never go through
`LOADFILE`) — and both give the identical answer. The **`-203`**
(`KE_NOFILE`) the reference actually returns for `rom0:NOSUCH` (`docs/
analysis/34` §6) is a *different* code from a *different* site: it is
`MODLOAD`'s own fixed substitution for an `IOMAN` `open` failure (`§3`,
`0xe38`) — a real filename that parses fine as a boot device but is not
present in the archive. `LOADFILE` itself never manufactures `-203`; it only
relays whatever `LoadStartModule` returns.

### The reply

All six `fno` handlers write their answer into the same fixed 8-byte `.bss`
area (`0x1e80`/`0x1e84`) and return only a pointer to it (`lui $2,0; addiu
$2,$2,0x1e80; jr $ra`) — no handler, nor the `0x4c4` wrapper, sets any
register that looks like a distinct reply *size*. That is consistent with
every handler's answer always being exactly these same two words, matching
the client's fixed `rsize=8` (`docs/analysis/34` §6), but exactly how
`SIFCMD`'s `sceSifExecRequest` turns "a returned pointer, no size" into an
8-byte `RPC_END` body was not traceable from `LOADFILE`'s own binary in this
pass — `docs/analysis/34` §3 describes that mechanism from `SIFCMD`'s side in
general terms (`size==0` meaning "answered by side channel") but a size of
*8*, not `0`, is what a fixed-size reply like this would need, and this
pass could not confirm which of the two that is here.

### The direct `IOMAN` imports: a second, parallel loader for `fno 1`

`LOADFILE` imports `ioman` open/close/read/lseek directly, and they are not
dead — `fno 1` ("loadelf") and `fno 5` back an internal ELF-loading engine
(roughly `0xf00`-`0x1420`, plus smaller call sites from `0x7dc`) that
`LOADFILE` implements **itself**: it opens the named file, validates the ELF
magic and `e_ident`/`e_type`/`e_machine` fields by hand (`0x510`-`0x608`;
strings `"File is not ELF format(%d)\n"` at `0x1a60`, `"Cannot openfile\n"`/
`"…heap buffer\n"`/`"…read buffer\n"` around `0x1b3c`-`0x1b70`), and calls
`modload` ordinal 14 (`GetLoadfileCallbacks`) partway through (`0x1264`) to
fetch a pair of optional function pointers it `jalr`s later
(`0x12d0`/`0x1300`/`0x1328`, not decoded further — plausibly the
signed/KELF-content hooks `modload.h`'s [header] `SetLoadfileCallbacks_
struct_` family names). This is a genuinely separate file-reading and
ELF-validating implementation from `MODLOAD`'s own (§3) — it exists because
`fno 1`'s job is loading an **EE**-side ELF across SIF, something `MODLOAD`
(which only ever loads IOP modules into IOP memory) has no reason to know how
to do.

## 5. What this pins for the rebuild

The chain from `SifLoadModule("rom0:NAME")` to a running module is, in call
order: `LOADFILE`'s `fno 0` handler (§4) → `MODLOAD`'s `LoadStartModule`
(§3) → `IOMAN`'s `open`/`lseek`/`read`/`close` (§1) → `ROMDRV`'s device ops
(§2) → `LOADCORE`'s `ProbeExecutableObject`/`LoadExecutableObject`/
`LinkLibraryEntries`/`RegisterModule` (§3, `docs/analysis/05`) → the
`entry(argc,argv,0,module_record)` call `docs/analysis/10` already pinned at
offset `0xB10`. A rebuild's minimal set for that path, module by module:

- **`IOMAN`** needs a driver table (any fixed small capacity — the retail
  image uses 16 slots, §1) storing raw `iop_device_t*` pointers, an
  fd table of `iop_file_t` records (retail: 16 slots, 16 bytes each, `mode`/
  `unit`/`device`/`privdata` at `0`/`4`/`8`/`0xC`, §1) allocated by "first
  record whose `device` field is zero", and `open`'s path parser: split at
  `:`, strip *trailing* decimal digits from the device-name token into a unit
  number (default `0`), match the digit-stripped name against the driver
  table, and call `ops->open(file, remainder_after_colon, flags)` — exactly
  three arguments, the fourth this document's task description guessed at
  does not exist. `AddDrv`/`DelDrv` need no name-collision detection; the
  retail image has none.
- **`ROMDRV`** needs to register exactly one device named `"rom"` (no digit,
  §2), self-locate and bind unit 0's archive at its own module init (the
  `RESET`/ROMDIR scan `docs/analysis/11` already covers), and expose the
  4-unit `RomImg[]` table so a later `romAddDevice` call could add `rom1:`
  if anything ever needs it (nothing in this ROM does). `open`/`read`/`lseek`/
  `close` need real bodies (read-only, `flags` must be exactly `1`); every
  other ops slot — `getstat`/`chstat` included — can be the shared "return
  0" stub, and `write` should be a dedicated "return `-5`" stub rather than
  the same no-op.
- **`MODLOAD`**'s `LoadStartModule` needs no ROM-specific code at all — it is
  three ordinary `IOMAN` calls (`open`, `lseek`-to-end for size, one `read`
  sized to the whole file, `close`) into a `sysmem`-ordinal-4 allocation,
  followed by `LOADCORE`'s probe/allocate-final/load/link/flush/register
  sequence (§3) and the already-pinned `0xB10` entry call. The module id is
  assigned somewhere inside that `LOADCORE` sequence, not by `MODLOAD` itself
  (§3) — a rebuild's counter belongs in `LOADCORE`'s internal state, matching
  `lc_internals_t.module_index` [header].
- **`LOADFILE`** needs a dedicated thread running exactly one `sceSifRegisterRpc`
  server on `sid = 0x80000006` whose dispatch reads `fno` from the call and,
  for `fno 0`, extracts `path@+8`/`arglen@+0`/`args@+0x104` from the request,
  calls `IsIllegalBootDevice` first (short-circuiting to `-201` on a rejected
  device string), otherwise calls `MODLOAD`'s `LoadStartModule` unmodified and
  writes back `{id_or_error, modres}` as an 8-byte reply. A rebuild aiming
  only at `SifLoadModule` does not need `fno` 1-5 (EE-ELF loading, the memory
  peek/poke backdoor, and KELF loading) — those exist for `EELOAD`/the OSD's
  own boot path, not for the IOP module loader `SifLoadModule` drives.
  It **does** need `fno 0xff`, the version query the newer `LOADFILE` added
  (§4): a title asks it before its first `fno 0` and refuses to send one if
  the four bytes do not name a release its own libraries were built against.
- **The two observed failure answers** are now both pinned to a specific
  site: `rom0:NOSUCH` → **`-203`**, from `MODLOAD`'s own fixed substitution
  for any `IOMAN open` failure (§3, `0xe38`) — the real `IOMAN`/`ROMDRV`
  error code is discarded, not passed through. A rejected boot-device string
  (whatever `IsIllegalBootDevice`'s own rule is — not derived in this pass)
  → **`-201`**, from either `LOADFILE`'s pre-check (§4) or `MODLOAD`'s own
  internal one (§3), whichever runs; a rebuild only needs one working copy of
  that check, called from both places, to reproduce both observed answers
  correctly.

## 6. Unresolved

- **`IsIllegalBootDevice`'s own rule** was never disassembled — only its two
  call sites (`LOADFILE` `fno 0`/`fno 1`, `MODLOAD`'s internal resolve step)
  and their shared `-201` answer are pinned. What makes a path "illegal" was
  out of scope for this pass.
- **The module-id counter's exact home** is narrowed to somewhere inside
  `LOADCORE`'s `ProbeExecutableObject`/`LoadExecutableObject`/`RegisterModule`
  (§3 confirms `MODLOAD` itself holds no such state) but not disassembled to
  an instruction — `docs/analysis/05`'s `LOADCORE` coverage is structural, not
  exhaustive, and `lc_internals_t.module_index` [header] is cited as the
  plausible location, not a confirmed one.
- **Several small negative return codes have no confirmed canonical name**:
  `IOMAN`'s "device not found" `-19` (§1); `ROMDRV`'s `-2`/`-6`/`-9`/`-12`/
  `-13`/`-22` (§2, its own KE_*-shaped family but none an exact `kerr.h`
  [header] match). They read as a small file-I/O errno family distinct from
  the kernel `KE_*` range, but this pass found no header or string in either
  binary naming them.
- **Which release a rebuild should answer `fno 0xff` with** is a title's
  question, not a console's: the number belongs to the `IOPRP` image the
  title asked the reboot to load, and reading it out of that image is part of
  the `UDNL` merge (`docs/analysis/45`) rather than of `LOADFILE`.
- **`LOADFILE`'s `fno 2`/`3` memory peek/poke and `fno 5`** were identified by
  behavior and offset only; `fno 5`'s target (`0x143c`) and the two extra
  reply words `fno 1`/`fno 5` produce beyond the `{id_or_error, modres}` pair
  were not decoded to field level — plausibly a loaded ELF's address/size,
  by shape, not confirmed.
- **How `SIFCMD` turns `LOADFILE`'s bare "pointer, no size" return into an
  8-byte `RPC_END` reply** was not settled — `docs/analysis/34` §3 describes
  the `size==0` "side channel" case from `SIFCMD`'s own side in general, but
  this pass could not confirm from `LOADFILE`'s binary alone whether a fixed
  reply size is implied by the server registration's `size2`/`buf` fields
  (§4) or read from somewhere else at reply time. A `SIFCMD`-focused pass
  (parallel to `docs/analysis/34`'s own unresolved `LOADFILE` item, which
  this document otherwise closes) would settle it.
- **Who calls `ROMDRV`'s exported `romAddDevice` for units 1-3** (`rom1:`
  etc.) is unresolved — nothing in the four modules this document covers
  does; it is not needed for `SifLoadModule("rom0:...")` and was not chased.
