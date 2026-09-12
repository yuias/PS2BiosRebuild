# MCMAN: Is a Formatted Card in the Slot, and What Is in Its Root Directory

Reference module `MCMAN` (unprefixed, `.iopmod` version **1.01**, export tag
`mcman` v1.01, 43 entries; entry `0x128`, gp `0x13a50`, text `0xb7f0`, data
`0x270`, bss `0x13bf0`). Scope: exactly what a rebuild needs so that the EE can
learn whether a formatted PS2 memory card is in a slot and read its root
directory. Writing, creating, deleting, formatting, the FAT allocation path,
PS1-format cards, the ECC algorithm itself, `MCSERV`'s request layouts beyond
§5, and `XMCMAN` beyond one line are out of scope and were not read; §6 lists
the regions never entered.

```sh
python3 tools/romdir.py assets/SCPH-50000.bin --extract <outdir>          # outside the repo
python3 tools/irxinfo.py <outdir>/MCMAN --exports --imports
python3 tools/irxinfo.py <outdir>/MCMAN --dump-load <outdir>/MCMAN.text
python3 tools/romdis.py <outdir>/MCMAN.text --cpu iop --vma 0            # whole module, 0x0-0xb7f0
readelf -S <outdir>/MCMAN
xxd -s 0xb7f0 -l 0x270 <outdir>/MCMAN.text                               # .rodata/.data as literals
# cross-checks used below
python3 tools/irxinfo.py <outdir>/MCSERV --exports --imports
python3 tools/irxinfo.py <outdir>/MCSERV --dump-load <outdir>/MCSERV.text
python3 tools/romdis.py <outdir>/MCSERV.text --cpu iop --vma 0 --range 0x40 0x320
python3 tools/romdis.py <outdir>/MCSERV.text --cpu iop --vma 0 --range 0x730 0xa64
python3 tools/irxinfo.py <outdir>/XMCMAN --imports
```

Addresses are `MCMAN.text` offsets (module vaddr, base 0). `.text` is
`0x0`-`0xb7f0`, `.rodata` `0xb7f0`-`0xb880` (the tag name, a 16-entry jump
table at `0xb800`, a 28-byte format identifier at `0xb840`, the two
conventional entry names `.` and `..` at `0xb860`/`0xb864`, a version string,
the device description and the two-letter device name at `0xb87c`), `.data`
`0xb880`-`0xba60` (a banner, the export header, the **command table** at
`0xb8a0`, a 256-byte ECC parity table at `0xb8c4`, the cluster-cache size
`0x24` at `0xb9f0`, the ioman operation table at `0xba00` and the device
record at `0xba44`), `.sbss` `0xba60`, `.bss` `0xba70`-`0x1f650`. Import
stubs are `jr $ra`/`addiu $zero,$zero,ORD` pairs (`0xb5d0`-`0xb7f0`), so
every call target below was identified by the ordinal in the delay slot:

| library | stub -> ordinal | name used below |
|---|---|---|
| `loadcore` | `0xb5e4` -> 6 | `RegisterLibraryEntries` |
| `intrman` | `0xb608` -> 9 | `CpuEnableIntr` (37) |
| `sio2man` | `0xb62c` -> 24, `0xb634` -> 25 | `sio2_mc_transfer_init`, `sio2_transfer` (40 §4) |
| `sysclib` | `0xb658` 12 `memcpy`, `0xb660` 14 `memset`, `0xb668` 20 `strcat`, `0xb670` 22 `strcmp`, `0xb678` 23 `strcpy`, `0xb680` 27 `strlen`, `0xb688` 29 `strncmp`, `0xb690` 30 `strncpy` | (08) |
| `thbase` | `0xb6b4` -> 33 | `DelayThread` |
| `thsemap` | `0xb6d8` 4, `0xb6e0` 5, `0xb6e8` 6, `0xb6f0` 8 | `CreateSema`, `DeleteSema`, `SignalSema`, `WaitSema` (38) |
| `timrman` | `0xb714` 4, `0xb71c` 5, `0xb724` 7, `0xb72c` 10 | `AllocHardTimer`, `ReferHardTimer`, `SetTimerMode`, `GetTimerCounter` (44) |
| `modload` | `0xb750` -> 13 | path-check callback setter |
| `ioman` | `0xb774` 20, `0xb77c` 21 | `AddDrv`, `DelDrv` |
| `secrman` | `0xb7a0` 4, `0xb7a8` 5, `0xb7b0` 6 | `SecrSetMcCommandHandler`, `SecrSetMcDevIDHandler`, `SecrAuthCard` (48) |
| `cdvdman` | `0xb7d4` -> 51 | the clock read (§6) |

Every name for something inside `MCMAN` is coined here. Where a sentence is an
inference rather than a reading it says so.

## 0. Cross-references established elsewhere, not re-derived

- `docs/analysis/40` §4: the `sio2_transfer` descriptor `td` layout (`stat6c +0`,
  `port_ctrl1[4] +4`, `port_ctrl2[4] +0x14`, `stat70 +0x24`, `regdata[16]
  +0x28`, `stat74 +0x68`, `in_size +0x6c`, `out_size +0x70`, `in +0x74`, `out
  +0x78`, `in_dma {addr,size,count} +0x7c`, `out_dma +0x88`), and that
  `SIO2MAN`'s push writes `port_ctrl1[0..3]` and `regdata[0..15]` from `td`,
  never `port_ctrl2`, then `sceSetSliceDMA(11, in_dma)` / `(12, out_dma)`.
- `docs/analysis/48`: `secrman` 4/5 are void setters that must accept NULL;
  6 answers exactly 0 or 1 and its ladder runs through the handler ordinal 4
  stored, using a descriptor on `SECRMAN`'s own stack.
- `docs/analysis/54` §4.3: `PADMAN`'s `regdata` is `port | 0x40 | txlen<<8 |
  rxlen<<18`; the port bits select `port_ctrl1[port]`.
- §0.1 below: the superblock and directory-entry layouts, read directly out
  of two real card images rather than out of this binary. The rest of this
  document only says which of those fields the code reads and when.
- `docs/analysis/10` line 145: the reference OSD loads the X pair (`XMCMAN`,
  `XMCSERV`), not this module. §6 says what that does to the dynamic witness.

### 0.1 The on-card layout, read from two real images

Two card images written by the title's own driver through the emulator's
`--memcard`, kept untracked. Both 8650752 bytes = 16384 pages x 528, where
528 is 512 of data plus 16 the image keeps after it. Walked with a throwaway
Python reader rather than with any of this binary's code.

#### Superblock, page 0

| Offset | Field | Both images |
|---|---|---|
| `+0x00` | a 40-byte format identifier ending in a version number | same |
| `+0x28` | page length, u16 | 512 |
| `+0x2a` | pages per cluster, u16 | 2 |
| `+0x2c` | pages per erase block, u16 | 16 |
| `+0x30` | clusters on the card, u32 | 8192 |
| `+0x34` | first allocatable cluster, u32 | 41 |
| `+0x38` | last allocatable cluster, u32 | 8135 |
| `+0x3c` | the root directory's cluster, u32, relative to `+0x34` | 0 |
| `+0x40` | two spare erase blocks, u32 each | 1023, 1022 |
| `+0x50` | the indirect-FAT cluster list, u32 each, zero-terminated | one entry: 8 |

A cluster is therefore 1024 bytes, and cluster `n` of the allocatable area is
absolute cluster `41 + n`, which is page `2 * (41 + n)`, which is image offset
`528 * 2 * (41 + n)`.

#### The FAT is two levels

`+0x50`'s list names clusters that themselves hold cluster numbers. Cluster 8
of both images begins `9, 10, 11, 12, 13, 14, 15, 16`, so the FAT proper lives
in clusters 9 upward. With 256 entries to a cluster, FAT entry `n` is word
`n % 256` of the cluster named by indirect entry `n / 256`.

An entry with bit 31 clear ends the chain. Otherwise its low 31 bits are the
next cluster, and the value `0xFFFFFFFF` is the end marker -- masking it
without checking for it yields `0x7FFFFFFF` and walks off the card, which is
the first thing this walker got wrong.

#### Directory entries are 512 bytes, two to a cluster

| Offset | Field |
|---|---|
| `+0x00` | mode, u32; bit 5 marks a directory |
| `+0x04` | length: for a directory, the number of entries in it |
| `+0x08` | created: unused, second, minute, hour, day, month, then year as u16 |
| `+0x10` | the entry's first cluster, u32 |
| `+0x14` | the slot this entry occupies in its parent |
| `+0x20` | attributes, u32 |
| `+0x40` | the name, up to 32 bytes, NUL-terminated |

Checked against both images. A freshly formatted card's root holds exactly two
entries, a one-character and a two-character name, both directories, and the
root's own length field reads 2. The card with a save on it reads 3, the third
being a directory with a twelve-character name -- a save is a directory, and
its name is a product code, so it is referred to here by its length rather
than reproduced.

A slot the directory's length does not cover reads as `0xFF` throughout, so a
reader that walks every slot of every cluster in the chain instead of counting
to the length field sees a sentinel entry with every field at `0xFFFFFFFF`.

## 1. The entry, traced end to end

`_module_start` (`0x128`) overwrites `$a0` at `0x130` before any read, so
`argc`/`argv` are ignored. In order:

1. `RegisterLibraryEntries(0x0)` — nonzero -> return 1, nothing else done.
2. `CpuEnableIntr()` (`0x148`).
3. `0x1430`: zero the **PS2 transfer descriptor** `td` at `0x1f330` (`0x94`
   bytes) and seed the parts that never change again: `port_ctrl1[2] =
   0xff020405`, `port_ctrl1[3] = 0xff030405`, `port_ctrl2[2] = port_ctrl2[3]
   = 0x0005ffff` (bytes `{5,4,port,0xff}` and `{0xff,0xff,5,0}` then `& 0xfcffffff`,
   `0x147c`-`0x14b0`); `in_dma.addr = 0xba70`, `in_dma.size = 0x24`,
   `out_dma.addr = 0xc0a0`, `out_dma.size = 0x24` (`0x14cc`-`0x14e8`). Then
   `SecrSetMcCommandHandler(0x25cc)` and `SecrSetMcDevIDHandler(0x25f8)`
   (§2.3), return values ignored.
4. `0x6c90`: the **PS1 descriptor** at `0xd328`: `port_ctrl1[0..3] =
   0xffc00505`, `port_ctrl2[0..3] = 0x000201f4`, `in = 0xd3c0`, `out = 0xd450`,
   DMA fields zero. Out of scope beyond §6's note that the empty-slot fallback
   uses it.
5. `0x95f0`: the **cluster cache** — 36 (`0xb9f0`) 16-byte records at
   `0x16570` `{cluster +0, data +4, slot +8, dirty +0xa, port +0xb, +0xc}`,
   each pointing at a 1 KiB buffer from `0xd570`, an MRU pointer list at
   `0x167b0`, with the two bases kept at `0x1e864`/`0x1e868`; four port records
   get `+0x174/+0x178/+0x17c = -1`; the **root-chain cache** at `0x1a840`
   (two ports x four slots x 513 words) is filled with `-1` and word 0 of each
   set to 0.
6. `0xad50`: `modload` 13 with `0xab28` (a callback that parses a path string;
   **return ignored**, and nothing on the read path calls the callback);
   `DelDrv(name)`; `AddDrv(0xba44)`. The device record is `{name 0xb87c,
   type 0x10, version 1, desc 0xb870, ops 0xba00}`: a filesystem device under
   the two-letter name every title addresses as `mc0:`. Its 17-entry
   operation table at `0xba00` is `init 0xab20` (returns 0), `deinit 0xadd0`,
   `format 0xb01c`, `open 0xae00`, `close 0xae94`, `read 0xaf4c`, `write
   0xafb4`, `lseek 0xaee4`, `ioctl 0xb59c`, `remove 0xb084`, `mkdir 0xb0f8`,
   `rmdir 0xb16c`, `dopen 0xb1e0`, `dclose 0xb264`, `dread 0xb2b4`, `getstat
   0xb30c`, `chstat 0xb38c`. If `AddDrv` answers 0: `CreateSema({attr 1,
   option 0, init 1, max 1})` -> `0x1e878`; else `0x52c` (close every open
   handle) and answer 1. **The entry ignores `0xad50`'s answer.**
7. `ReferHardTimer(1, 0x20, 0, 0x6309)` -> `0xba60`. If the answer is not
   `-150`, it is kept as the timer handle and the entry returns 0. If it is
   `-150`: `AllocHardTimer(1, 0x20, 1)` -> `0xba60`, and if that is `> 0`,
   `SetTimerMode(id, 0)`. Return 0 (resident).

So there are **no threads, no event flags, no interrupt handlers**; one
semaphore, taken only by the ioman operations (§5.2); one hardware timer,
read only on the PS1 path (§6). Nothing after step 1 can make the entry fail.

### 1.1 The per-port record

Everything about a slot lives in `R(p) = 0x1ed30 + p*0x180`, `p` in 0..3,
where the memory-card ports are **`p = (port & 1) + 2`** — every export
converts its `port` argument this way (`andi 1; addiu 2`), and `p` is also
the SIO2 port index the descriptor carries (§3). `R+0x000..0x150` is a copy
of superblock page 0 (§4.3); the driver's own state follows:

| Off | Field | Written by |
|---|---|---|
| `+0x150` | card type byte: 0 none, 1 PS1, 2 PS2, 3 PS1-pocket | export 5 |
| `+0x151` | flags byte from the geometry reply (§2.4): bit 0 = card carries a 16-byte tail per page, bit 3 and bit 4 used by the recovery/scan paths | `0x2184` |
| `+0x154` | cluster size, always `0x400` | `0x27b0` |
| `+0x158` | FAT entries per cluster, always `0x100` | `0x27b0` |
| `+0x15c` | clusters per erase block | `0x27b0` |
| `+0x160` | **mounted**: 0 unknown, 1 mounted, -1 set by `0x2e10` (a path not read, plausibly format) | `0x2acc` (1), `0x12ec` (0), `0x2eb4` (-1) |
| `+0x164`, `+0x168` | current directory (cluster, entry index) — seeded **from the still-zeroed record**, so 0/0 | `0x27b0`, chdir |
| `+0x16c`, `+0x170` | allocation-scan cursor and bound | `0x27b0`, `0x2dec` |

## 2. Card detection and the authentication order

Export 5 (`0x5ac`, "detect") is the only way in; every other export that
touches a card calls it first (§5). Its PS2 branch is `0x2374(p, slot)`.

### 2.1 Export 5 (`0x5ac`)

```
p = (port & 1) + 2; R = R(p)
type == 0 or 2:  v = 0x2374(p, slot)                     // PS2 first
                 v >= -9 -> R+0x150 = 2, return v
                 else    -> PS1 probe 0x714c, then 0x7380 (out of scope) ...
type 1 or 3:     PS1 probe first, PS2 second (mirror image, 0x698-0x774)
either branch, both probes failed (< -9):
                 R+0x150 = 0; 0x4e0(p) closes handles; 0x9718(p, slot)
                 invalidates the cluster cache; return the PS1 probe's code
```

Two consequences worth stating plainly. **Every failure below -9 from the PS2
branch — -11, -12, -13, the mount codes -45..-49 and the -90 from a refused
authentication — is masked**: the caller gets the PS1 probe's answer (-11
when nothing answers its `81 52` frame, §6) and type 0. The -90 that
`docs/analysis/48` §3 saw surface from `MCMAN.IRX` 2.30 never leaves v1.01.
And an empty slot always costs the PS1 probe too.

### 2.2 The PS2 ladder `0x2374(p, slot)`

Every frame below is one batch of §3's shape with one sub-transfer; "ok"
means `(td->stat6c & 0xf000) == 0x1000` after `sio2_transfer`.

1. **Probe** `0x17ec(p)`: `81 11 00 00` (tx/rx 4), up to 5 times. Answer
   `rx0[3] == 0x66` or not ok -> try again; anything else at `rx0[3]` ->
   success, **skip to step 5**. Five misses -> -1 and continue.
2. **Reset** `0x22f8(p)`: `81 f3 00 00 00` (5), once; ok -> 0, else
   **return -11**. The reply bytes are not examined.
3. `cnum = 0x25f8(p, slot) = (p & 1) << 3`; **`SecrAuthCard(p, slot, cnum)`**
   (`0x23d8`). Zero -> **return -90**. Note `p` is already the SIO2 port
   (2/3), so `SECRMAN` sees 2 or 3, which matches 48 §3's `port + 2`.
4. Remember "authenticated in this call" (`$19 = 1`).
5. **Presence** (`0x23ec`): `81 28 00 00 00` (5), up to 5 times: ok and
   `rx0[4] != 0x66` -> success; five misses -> **return -12**.
6. If `rx0[3] == 0x5a` (the terminator we set on an earlier visit, step 8):
   `R+0x160 > 0` -> **return 0** (same card, still mounted); `R+0x160 < 0`
   -> return -2; `== 0` -> fall through (card known but not mounted, e.g.
   unformatted).
7. If step 4 did not run: `SecrAuthCard(p, slot, cnum)` now; zero -> **-90**.
   Then `0x9718(p, slot)` drops every cluster-cache record of this slot.
8. **Terminator** (`0x24f0`): `81 27 5a 00 00` (5), up to 5 times, success
   = ok and `rx0[4] == 0x5a`; five misses -> **-13**. (The reply is expected
   to carry the value in force; a card that echoes the old one takes two
   rounds, one that echoes the new one takes one. The binary is a retry
   loop, not a fixed two-shot.)
9. **Mount** `0x2acc(p, slot)` (§4.3): 0 -> **return -1** ("card changed":
   a card was just mounted), else its code (-2 unformatted, -45..-49).

So from "nothing known" to "mounted": `0x11` (x1..5) -> `0xf3` -> the
`SECRMAN` exchange through the handler (§2.3) -> `0x28` -> `0x27` -> `0x26`
(§2.4) -> page 0 -> two journal pages -> root page(s) (§4.3). On the next
call for the same card: `0x11` once, `0x28` once, return 0. Reproduce the
frame order: `python3 tools/romdis.py <outdir>/MCMAN.text --cpu iop --vma 0
--range 0x17ec 0x2604`.

**What our stand-in gives it.** With `secrman` 4/5 storing the pointer and 6
answering 1, step 3 passes with no SIO2 traffic and the ladder proceeds to
`0x28`. With 6 answering 0 (handler never registered), step 3 returns -90,
export 5 masks it into the PS1 probe (§2.1), and the caller sees the probe's
code with type 0: no hang either way. `MCMAN` registers the handlers at
entry (step 3 of §1) before any card call, so the 0 case cannot arise unless
`secrman` itself is a whole-tag miss.

### 2.3 The two handlers `SECRMAN` gets

`0x25cc(port, slot, td)`: `sio2_mc_transfer_init(); return sio2_transfer(td)`
— two calls, no check, `td` is `SECRMAN`'s (48 §2.1), so whatever shape the
authentication uses on the wire is `SECRMAN`'s business, not this module's.
`0x25f8(port, slot)`: `return (port & 1) << 3`, no memory access. Neither is
reached by the stand-in.

### 2.4 The geometry query `0x2184`, and what it fixes

Called from `0x27b0` at the top of the mount: `81 26` (13; byte 2 is stale, §3), up to 5
times, success = ok, `rx0[0xc] == 0x5a` and `XOR(rx0[3..0xa]) == rx0[0xb]`;
five misses -> -1 (mount answers -49). Outputs: `R+0x151 = rx0[2]` (flags),
`R+0x28 = rx0[3] | rx0[4]<<8` (page length), `R+0x2c = rx0[5] | rx0[6]<<8`
(pages per erase block), and a u32 card size in pages from `rx0[7..0xa]`.
`0x27b0` then derives `R+0x2a = 0x400 / page_len` (pages per cluster),
`R+0x154 = 0x400`, `R+0x158 = 0x100`, `R+0x15c = R+0x2c / R+0x2a`,
`R+0x30 = (size / R+0x2c) * R+0x15c`, `R+0x2e = 0xff00`, and zeroes the
allocation cursors. The superblock copy of §4.3 later overwrites `+0x28`,
`+0x2a`, `+0x2c`, `+0x2e`, `+0x30` with the card's own values; `+0x154`,
`+0x158`, `+0x15c` stay derived. The **tail** flag `R+0x151` bit 0 decides
whether a page read asks for the 16 trailing bytes at all (§4.1).

## 3. The SIO2 transfer shape

One descriptor `td` at `0x1f330` (`0x20000 - 0xcd0`; eleven `addiu …,
-0xcd0` sites, all in the PS2 path), used **DMA-only**: `in_size`, `out_size`,
`in`, `out` are zeroed at entry and no instruction ever writes `td+0x6c`,
`+0x70`, `+0x74`, `+0x78` (grep of the `-0xc64..-0xc58` offsets finds no store).
Every batch goes through one builder, `0x150c(p, cmd, arg)`:

- `cmd == -1`: `in_dma.count = 0` — start a batch.
- `cmd == -2`: `regdata[n] = 0` (terminator), `out_dma.count = in_dma.count`
  — close the batch. `n` is the number of sub-transfers so far.
- otherwise, with `n = in_dma.count`, refused if `n >= 11`: `in_dma.count =
  n + 1`; look up the **command table** at `0xb8a0 + 2*cmd` (`cmd` is an
  index, not a byte): `{byte, len}` pairs, read as literals:

  | idx | byte | len | idx | byte | len | idx | byte | len |
  |---|---|---|---|---|---|---|---|---|
  | 0 | `0x11` | 4 | 6 | `0x25` | 4 | 12 | `0x81` | 4 |
  | 1 | `0x12` | 4 | 7 | `0x26` | 13 | 13 | `0x82` | 4 |
  | 2 | `0x21` | 9 | 8 | `0x27` | 5 | 14 | `0x42` | 6 (+n) |
  | 3 | `0x22` | 9 | 9 | `0x28` | 5 | 15 | `0x43` | 6 (+n) |
  | 4 | `0x23` | 9 | 10 | `0x42` | 134 | 16 | `0xbf` | 5 |
  | 5 | `0x24` | 134 | 11 | `0x43` | 134 | 17 | `0xf3` | 5 |

  Then `regdata[n] = (p & 3) | 0x70 | (len << 8) | (len << 18)` (`0x1598`-
  `0x15c8`) — tx and rx lengths equal, and **`0x70` where `PADMAN` uses
  `0x40`**; the transmit block `T = 0xba70 + n*0x90` gets `T[0] = 0x81`,
  `T[1] = byte`, and the payload per the jump table at `0xb800` (index
  `cmd-1`, 16 cases): idx 2/3/4 -> `T[2..5] = arg` as four little-endian
  bytes, `T[6] = XOR(T[2..5])` (`0x1624`); idx 5 -> `T[2] = arg` byte; idx 8 ->
  `T[2] = 0x5a`; idx 10 -> `T[2] = 0x80`, 128 bytes from `arg` at `T[3..]`,
  their XOR at `T[0x83]` (write path); idx 11 -> `T[2] = 0x80`; idx 14/15 ->
  `T[2] = L` where `L = 0x2780(p) = (R+0x28 + 31) >> 5` (16 for 512-byte
  pages), and both length fields of `regdata[n]` grow by `L` (`0x1760`-
  `0x17b4`); idx 1 -> `T[2] = 0`; the rest carry no payload.

Only `T[0]`, `T[1]` and the payload are written: **bytes past a command's
payload are whatever the block last carried**, which is why both traces show
`81 26 5a 00` for the payload-less `0x26` — the `0x5a` is left over from the
`0x27` frame that used block 0 just before.

The DMA pair is therefore: channel 11 from `0xba70`, **`0x24` words (144
bytes) per block, `n` blocks, one sub-transfer per block**; channel 12 into
`0xc0a0`, the same geometry. `MCMAN` reads reply `k` at `0xc0a0 + k*0x90`
(every `rx` offset in §2 and §4 is of that form), so it expects the hardware
to leave each reply at its own block start — whether the hardware pads is the
emulator's side of that contract, not this module's. The two buffers are
`0x630 = 11 * 0x90` apart, matching the `n >= 11` refusal. `port_ctrl1[2..3]`,
`port_ctrl2[2..3]` are the constants of §1 step 3 and are **never rewritten**;
by 40 §4 the `port_ctrl2` words never reach hardware through `SIO2MAN` v1.01
at all. `stat70`/`stat74` are never read; the only status test anywhere in
the PS2 path is `(stat6c & 0xf000) == 0x1000`.

Per batch the calls are `sio2_mc_transfer_init()` then `sio2_transfer(td)`
(15 paired sites), nothing else from `sio2man`. There is no delay, no timer
and no thread on the PS2 path; the module blocks inside `sio2_transfer`.

### 3.1 The outside lead, claim by claim

| Lead (PS2e `hw-notes`, "SIO2 memory-card transfers") | Verdict from this binary |
|---|---|
| one DMA block per sub-transfer, `0x24` words x N, command at block start, reply padded to a block | **Holds** for the transmit side (`0x150c`); on the receive side MCMAN *reads* block `k` at `k*0x90`, which is the padding the lead describes from the other end |
| RECV1 must read "present" `0x1100` | MCMAN tests `(stat6c & 0xf000) == 0x1000` only; bit 8 is never examined (no `0x1100` literal in the module) |
| card acknowledges with `0x2b` | **Never checked**: no `0x2b` literal outside error codes. The trace's reply head shows `0x2b` at `rx0[7]` just before the `0x5a` MCMAN does check at `rx0[8]` |
| terminator set by `0x27`, reply carries the old one, issued twice | A retry loop of up to five on `rx0[4] == 0x5a` (`0x2534`-`0x2578`); two rounds only if the card echoes the old value. Both traces show one |
| `0x66` = busy/absent | Tested at `rx0[3]` after `0x11` (`0x1870`) and at `rx0[4]` after `0x28` (`0x2454`); nowhere else |
| page read = `0x23` (page + XOR), `0x43` x 128 + 16-byte tail = 528, `0x81` | **Holds** exactly (§4.1), with the tail conditional on `R+0x151` bit 0 |

## 4. Reading one page, and what the mount reads

### 4.1 Export 18 `0x1dc4(p, slot, page, dst)`

`page` is absolute. With `C = R+0x28 / 128` chunks (4) and `L = 0x2780(p)`
(16), up to 5 attempts; on every attempt after the first, the `0x11` probe
`0x17ec` runs first (`0x1e48`). One batch:

| block | frame | tx/rx len | what MCMAN checks in the reply |
|---|---|---|---|
| 0 | `81 23 p0 p1 p2 p3 X 00 00`, `X = p0^p1^p2^p3` | 9 | `rx0[8] == 0x5a` |
| 1..C | `81 43 80 00 …` | 134 | data = `rx[4..0x83]`; `XOR(data) == rx[0x84]` (`0x1f58`-`0x1f80`) |
| C+1 (only if `R+0x151 & 1`) | `81 43 10 00 …` | 22 | tail = `rx[4..4+L-1]`, **its XOR byte is not checked** |
| C+2 | `81 81 00 00` | 4 | the byte at **`0xc403` = rx block 6 byte 3** `== 0x5a` |

The last check is at a fixed offset, so it lands on the `0x81` block only when
the tail block is present; a card whose geometry flags clear bit 0 would have
MCMAN check a stale block. Both real images and both traces carry the tail.
After the checks the `C` chunks are copied to `dst`.

**ECC on read**: if the last tail byte is `0xff` the page counts as erased and
returns 0 without a check. Otherwise export 20 (`0x2604`, 128 bytes -> 3
bytes, the parity table at `0xb8c4`; algorithm not read) is compared per
chunk by `0x2678` against `tail[3i..3i+2]`; it answers 0 (match), -1 (one data
bit corrected in `dst`), -2 (the stored code itself differs in one bit), -3
(neither). The minimum over chunks drives the outcome: 0 -> return 0; -1/-2 ->
retry, but **accepted on the fifth attempt**; -3 -> retry, and after five
**return -2**. A batch that fails its wire checks five times returns -1.

### 4.2 Reading a cluster `0xa278(p, slot, abs_cluster, &rec)`

Cache lookup `0x98b4`; on a miss the least-recent record is reused (flushed
first if dirty — write path) and filled with `R+0x2a` page reads at
`abs_cluster * R+0x2a + i` into `data + i*R+0x28`; any read failure ->
`-21`. Nothing here adds `+0x34`: callers pass absolute clusters.

### 4.3 The mount `0x2acc(p, slot)`, and the superblock fields in the order read

1. `memset(R, 0, 0x180)`.
2. `0x27b0`: the geometry query of §2.4; failure -> **-49**.
3. Export 18 for **page 0** into `0x1e880`: -2 -> **-2**; other nonzero ->
   **-48**.
4. `strncmp(0xb840, page0, 0x1c)`: the **28-byte identifier at `+0x00`**
   differs -> **-2**.
5. Copy `0x150` bytes of page 0 into `R+0` (`0x2b90`-`0x2c28`).
6. `0x6618(p, slot)`: read the two pages `R+0x44 * R+0x2c` and `+1` — the
   first two pages of the **second spare erase block at `+0x44`** — and take
   word 0 of each (bit 4 of `R+0x151` maps a zero word to -1; bit 31 is
   masked otherwise). Cases (`0x6718`-`0x6748`): both `-1` (both real images:
   the block is all `0xff`) -> 0; first valid and second `-1`, or either read
   failed -> **erase that block** through export 17 (`0x690c`) and answer its
   result; both valid, or first `-1` and second valid -> a journal replay
   that reads the clusters of the block at **`+0x40`** (`0x6748` onward, not
   read further: write-path recovery). Nonzero -> **-47**.
7. `0xa3d0(p, slot, 0, 0, &e)` — entry 0 of the directory at **relative
   cluster 0** (§4.4): failure -> **-46**; `strcmp(e+0x40, ".")` differs ->
   **-2**.
8. The same for entry 1 and `..`: failure -> **-45**; mismatch -> **-2**.
9. `R+0x160 = 1`, `R+0x150 = 2`; then a scan from **`+0x34`** upward over
   roughly `+0x30` clusters that skips clusters whose erase block appears in
   the 16-word list at **`+0xd0`** (`0x2d0c`-`0x2dd8`) and stores the bound in
   `R+0x170`. That bound feeds export 38 (free clusters); not part of the
   read path.

So "not formatted" (-2) is any of: page 0 uncorrectable after five reads, the
identifier prefix mismatch, or either of the first two root entries not being
`.` / `..`. **The mount never reads `+0x3c`, and nothing uses it to locate
the root**: a path with a leading `/` starts at relative cluster 0
(`0x4714`-`0x4720`), an empty directory part starts at the current directory
`R+0x164/+0x168` (0/0 until a chdir, seeded by `0x27b0` from a record that is
still zero), and `0xa3d0` adds `+0x34` (`0xa5c0`: `lw $6, -0x129c` =
`R+0x34`). The one reader of `+0x3c` is export 12's start-of-listing path
(§5.1), which compares it with the resolved directory's first cluster to
decide whether to hide `.` and `..`. `+0x38` did not appear on any path read
here.

### 4.4 Fetching a directory entry `0xa3d0(p, slot, dir_cluster, index, &out)`

Entries per cluster = `R+0x154 >> 9` = 2. The chain ordinal `index/2` is
walked from the cached position with the **FAT lookup** `0xa86c(p, slot, c,
&next)`: `next = data(data(R+0x50[c >> 16])[(c >> 8) & 0xff])[c & 0xff]`,
both cluster reads absolute (`0xa91c`, `0xa948`), answering -78/-79 on a failed read, which `0xa3d0` turns into -70; bit
31 is masked and `0xffffffff` ends the chain (-4). The root chain is
memoised per (port, slot) in the 513-word table of §1 step 5 (`0xa584`-
`0xa590`). Then `0xa278(p, slot, chain_cluster + R+0x34, &rec)` (-71 on
failure) and `*out = rec->data + (index % 2) * 512`. For the root's first two
entries this is cluster 41 -> pages 82 and 83, as the ground truth predicts;
for entries 2 and 3 it is the first FAT lookup, which reads cluster
`+0x50[0]` (8) and then cluster `data[0]` of it (9).

### 4.5 What the listing reads from an entry

Export 12's body `0x5fd4` (§5.1) and the ioman `dread` body `0x3c74` read:
`+0x00` mode (bit 15 = slot in use, bit 5 = directory, bits 0-3/0x800/0x1000
copied, bit 13 hides an entry unless the PS1 flag is set), `+0x02` (u16,
copied), `+0x04` length, `+0x08` created (8 bytes), `+0x10` first cluster,
`+0x14` parent slot (only to resolve `..`), `+0x18` an 8-byte field with the
same shape as `+0x08` (the ground-truth table does not list it; MCMAN copies
it as the second timestamp), `+0x20` attributes, `+0x40` name (compared with
the pattern by the `?`/`*` matcher `0x37c`).

## 5. The smallest useful export surface

### 5.1 Exports and argument shapes

| Ord | Addr | Read? | Shape and role |
|---|---|---|---|
| 0, 4 | `0x128` | §1 | entry (slot 4 aliases it) |
| 1-3, 21-28, 31-35, 41-42 | `0xc4` | yes | bare `jr $ra` |
| **5** | `0x5ac` | §2 | `detect(port, slot)` -> 0 same card, **-1 card changed (just mounted)**, **-2 present but unformatted**, else the masked failure of §2.1 |
| 6 | `0x7c4` | glue only | `open(port, slot, name, flags)` -> handle; body `0x4dbc` not read; flag `0x2000` stripped unless the PS1 flag is set |
| 7 | `0x88c` | no | `close(handle)` |
| 8 | `0xa98` | no | `read(handle, buf, len)` |
| 9 | `0xbcc` | no | write |
| 10 | `0x9c4` | no | `seek(handle, off, whence)` |
| 11 | `0x11b0` | no | format |
| **12** | `0xcf4` | §5.1 | `getdir(port, slot, path, mode, maxent, out)` -> entries written |
| 13 | `0x1104` | no | delete |
| 14 | `0x924` | no | flush |
| 15 | `0x1060` | no | chdir (writes `R+0x164/+0x168`) |
| 16 | `0xf88` | no | set entry info |
| 17 | `0x18b4` | shape | erase block: `81 82` after a `0x21` page set; checks `rx0[8]` and `rx1[3] == 0x5a` |
| **18** | `0x1dc4` | §4.1 | `read_page(port, slot, page, dst)` -> 0 / -1 / -2 |
| 19 | `0x1b28` | no | write page |
| 20 | `0x2604` | §4.1 | `ecc(src128, dst3)` |
| 29, 30 | `0x6f38`, `0x6d44` | no | PS1 page read/write |
| 36 | `0x124c` | yes | unformat: detect, then `R+0x160 = 0` |
| 37 | `0x9710` | yes | returns 0, does nothing |
| 38 | `0x234` | yes | free clusters: 0 unless `R+0x160`, else the scan `0x3a18`; -3 when none |
| **39** | `0x1fc` | yes | `type(port)` -> `R+0x150`: 0 none, 1 PS1, 2 PS2, 3 |
| 40 | `0x224` | yes | `set_ps1_flag(v)` -> the `.data` word `0xb890` (1 at load) |

Exports take the caller's `port` (0/1) and convert internally; none of them
takes the semaphore. **(a) present and formatted**: export 5 answering 0 or
-1, and export 39 answering 2. Export 5's -2 is "present, PS2, unformatted";
anything else is "no usable card" with type 0. **(b) the root's entries**:
export 12 with `path` = `/` followed by the pattern (`/*`), `mode = 0` to
start, then `mode != 0` to continue; `out` receives `maxent` records of
`0x40` bytes:

| Off | Field | Source |
|---|---|---|
| `+0x00` | created, 8 bytes | entry `+0x08` |
| `+0x08` | the second timestamp, 8 bytes | entry `+0x18` |
| `+0x10` | length (files only) | entry `+0x04` |
| `+0x14` | mode u16 | entry `+0x00` |
| `+0x16` | u16 | entry `+0x02` |
| `+0x18` | attributes | entry `+0x20` |
| `+0x20` | name, 32 bytes | entry `+0x40`, or `.`/`..` synthesised for a subdirectory's first two entries |

Export 12 first calls export 5 and gives up only below -1 (a "card changed"
answer does not stop a listing). With `mode == 0` it splits the path at its
last `/` — `/*` becomes an empty directory part plus the pattern `*`, so
`0x46a0` takes its current-directory branch; `/dir/*` takes the leading-`/`
branch; both reach relative cluster 0 for the root — resolves the directory
(`0x46a0`, read only to its head), rejects a non-directory (-4), and **starts
at entry 2 when the directory's first cluster (entry `+0x10`) equals `R+0x3c`
and its `+0x14` is 0** (`0x6100`-`0x6144`), so the root's `.` and `..` are
not returned. Entries whose mode bit 15 is clear or whose name fails the
pattern are skipped without counting. It answers the number written, so 0
means the end.

The ioman route is the same code under a lock: `dopen("mc0:/")` -> `0xb1e0`
-> export 6 with flags 0; `dread` -> `0xb2b4` -> `0xdc8(handle, dirent)` ->
export 5, then `0x3c74` fills an ioman `io_dirent_t` (`0x12c` bytes: mode
bits at `+0`, attributes `+4`, length `+8`, `+0xc` and `+0x1c` the two
timestamps, name at `+0x28`) and answers 1 per entry, 0 at the end; this
route returns `.` and `..` as well — inferred, since where `dopen` -> export
6 (`0x4dbc`, unread) seeds the handle's position word was not read. Its
handle table has three `0x30`-byte
records at `0x1eca0` (the loops at `0x4e0`/`0x52c` scan three); the unit
number is split into port (low nibble) and slot (`0xacb0`).

### 5.2 How `MCSERV` reaches them

`MCSERV` v1.01 (8 exports, 23 `mcman` imports: 5-20, 29, 30, 36-40) creates
one thread (`0xc0`, priority `0x68`, stack `0x1000`) that runs the
`sceSifCheckInit`/`Init`/`InitRpc`/`SetRpcQueue`/`RegisterRpc`/`RpcLoop`
sequence of 54 §2 with **sid `0x80000400`**, dispatcher `0x144`, request
buffer `0x3248` (bss, `0x418` bytes). The dispatcher takes `fno - 0x70`,
refuses `>= 0x11`, jumps through the table at `0x11d8`, calls a handler with
the request buffer, stores the handler's answer at `0x1240` and **returns
`0x1240` as the reply** (so word 0 of every reply is the `mcman` result;
`0x1248` on is staging). The map: `0x70` -> 40 then 37 (init: the PS1 flag is
set when request word 4 equals a magic), `0x71` -> 6, `0x72` -> 7, `0x73` ->
8 (data pushed by `sceSifSetDma`), `0x74` -> 9 (data pulled by `sifcmd` 23),
`0x75` -> 10, **`0x76` -> 12**, `0x77` -> 11, **`0x78` -> 5, 39, 38**, `0x79`
-> 13, `0x7a` -> 14, `0x7b` -> 15, `0x7c` -> 16, `0x7d` -> 39/17/20/19/30,
`0x7e` -> 39/18/29, `0x7f` -> 20/19, `0x80` -> 36. The two that matter:
`0x78` (`0x954`) reads words 1/2 as port/slot, calls export 5, then export 39
if word 3 is set and export 38 if word 4 is set and the detect answered `>=
-1`, DMAs a `0x40`-byte record `{type, free, …}` to the EE address in word 7
(`sceSifSetDma` under `CpuSuspendIntr`, then polls `sceSifDmaStat` with
`DelayThread(100)`), and answers export 5's code (or 38's if negative); the
third word of that record is not written in this build. `0x76` (`0x730`)
reads port/slot/mode/maxent/EE address at words 0-4 and the path at `+0x14`,
and loops **one entry per export-12 call** (`maxent 1`, `out 0x1248`), DMAing
each `0x40`-byte record to the EE address and advancing it by `0x40`; it
answers the count. So an EE client needs: bind `0x80000400`, one `0x78` call
(answer 0/-1 plus type 2 = formatted card), one or more `0x76` calls with
`/*` — or, on the IOP side, exports 5, 39 and 12 directly with no RPC at
all. The reference OSD itself uses `XMCSERV`/`XMCMAN` (§0), so a decision
to talk to a rebuilt `MCSERV` is not a decision to match the OSD's own path.

## 6. What a rebuild must implement, and what is open

For "the EE learns a formatted card is present and lists its root":

1. A `mcman` v1.01 export table of 43 entries in §5.1's order; on the read
   path only 5, 39, 12 (and 18, 20 beneath them) need bodies, the rest may be
   `jr $ra` where the reference's are, and stubs answering an error where they
   are not.
2. The entry of §1: register, seed `td` at its constants, register the two
   `secrman` handlers, `AddDrv` the filesystem device with the 17-slot table
   (the ioman route is optional for the RPC path but `dread` is the cheapest
   test hook), one semaphore for the ioman glue, no threads.
3. §3's descriptor exactly: DMA only, `0x24`-word blocks from `0xba70`-shaped
   transmit and receive areas, `regdata = p | 0x70 | len<<8 | len<<18` per
   sub-transfer with a zero terminator, `port_ctrl1[2..3] = 0xff020405 / 0xff030405`,
   `sio2_mc_transfer_init` then `sio2_transfer`, and the single test
   `(stat6c & 0xf000) == 0x1000`. **This is the first real user of our
   `SIO2MAN`'s `in_dma`/`out_dma` push and of `DMACMAN`'s slice transfer with
   `size 0x24, count n`**. Whether our `sceSetSliceDMA` treats those two the
   way the reference does was the first thing to check, and it does:
   `src/iop/dmacman.cpp`'s ordinal 28 writes `BCR = (count << 16) | size`, so
   `size` is words per block and `count` the number of blocks, which is the
   reading this section needs. It also already sets the to-memory bit on the
   receiving channel, with a comment recording that one emulator acts on that
   channel only when its control word is exactly right -- so the receive half
   has had a pass of attention before now, even though nothing has driven it.
4. §2.2's ladder with its codes, including the `0x11` short-cut and the
   `R+0x160` memo, and the `secrman` 6 call with `(p, slot, (p&1)<<3)`.
5. §4.1's page read with the four wire checks and the per-chunk XOR; ECC can
   be a stub that answers "match" only if the rebuild controls the card image
   (both real images carry real codes and non-`0xff` tails, so a faithful
   read must either compute the 3-byte code or skip the compare). Then
   §4.3's mount: identifier prefix, the two journal pages of `+0x44`, root
   entries 0/1 by name, all with relative cluster 0 + `+0x34`.
6. §4.4's FAT walk for root directories longer than one cluster (three or
   more entries), with the `+0x50` two-level lookup and the `0xffffffff`
   end; the cluster cache is an optimisation, not a contract.
7. Export 12 with the `0x40`-byte record, the root's skip of `.`/`..`, the
   bit-15 and pattern filters.
8. Kernel facilities: `secrman` 4/5/6 as already built; `ioman` 20/21;
   `thsemap`; `sysclib` 12/14/20/22/23/27/29/30; `intrman` 9. `modload` 13
   and `cdvdman` 51 are **harmless as stubs on this path**: 13's answer is
   ignored and its callback is never invoked by anything read here; 51 is
   only reached from `0x1308`, whose five callers (`0x3544`, `0x4608`,
   `0x548c`, `0x7aa4`, `0x88c0`) are all create/write/format/PS1 sites (it
   retries five times and then uses whatever is on its stack as a timestamp).
9. **An empty slot is not free**: export 5's fallback runs the PS1 probe
   `0x714c` — `81 52 00` through the **byte-mode** descriptor at `0xd328`
   (`in_size = out_size = 3`, `in 0xd3c0`, `out 0xd450`, `regdata 0xc0340 |
   p`), five times, each paced by `GetTimerCounter` on the handle from §1
   step 7 and `DelayThread` of at most `0x2710`/`0x4e20`. So our `SIO2MAN`'s
   byte loop is exercised alongside the DMA path, and the `timrman` handle
   matters: our `referHardTimer` (`src/iop/timemani.cpp`) answers a real id
   or `-1`, never `-150`, so MCMAN keeps that answer; with `-1` the pacing
   reads through a bogus id (a wrong sleep, not a hang, because the delay is
   bounded). Verify which of the two we get.

Open questions:

- **Block padding on receive.** MCMAN expects reply `k` at `0xc0a0 + k*0x90`;
  whether the SIO2 hardware writes replies at block boundaries or `SIO2MAN`/
  the DMA engine does the spacing is not decidable from this module. The
  sibling trace's `DMA out bytes = 1008` for a seven-block page read is
  consistent with either.
- **`regdata` bits 4-6** (`0x70` here, `0x40` in `PADMAN`) and the
  `port_ctrl1` constants are carried as opaque words.
- **The lone `0x11` per poll cycle** in the reference trace is `XMCMAN`'s
  (`docs/analysis/10` line 145: the OSD loads the X pair); in v1.01 a
  successful `0x11` is followed by `0x28` (§2.2 step 5). Not traced for this
  binary.
- **No locking on the export surface.** The ioman operations take
  `0x1e878`; the exports do not, and every PS2 batch shares `td` and the two
  DMA buffers. `MCSERV`'s single thread and an ioman caller could interleave.
- **The fixed-offset `0x81` check** (§4.1) with a card whose geometry flags
  clear bit 0.
- **`ReferHardTimer(1, 0x20, 0, 0x6309)`'s intent** (a shared timer?) and
  the meaning of `0x6309`.
- **Export 6's body `0x4dbc`** and the exact handle count on open.
- What `+0x38` is for (never read here), and whether `+0x3c` is ever
  anything but 0 given that only export 12's `.`/`..` decision reads it.

## 7. The mount, observed

A title's own card driver mounting a card with a save on it, traced through
the sibling emulator's serial log:

```sh
ps2e --bios <reference> --disc <title> --memcard <card image> --cycles 8e9 \
     --log 'warn,ps2_core::iop::sio2=debug'
```

filtered to sub-transfers whose first command byte is `0x81` (a card). 8791 of
them over the run; the mount is the first forty, in order:

| Frames | Command | Length | Reading |
|---|---|---|---|
| 5 | `81 11 00 00` | 4 | probe -- repeated, so the answer is polled |
| 2 | `81 f3 00 00` | 5 | |
| 1 | `81 f7 01 00` | 5 | |
| 21 | `81 f0 <n> 00`, `n` = `0x00`..`0x14` | 5 or 14 | the authentication exchange, one sub-step per frame; the fourteen-byte ones are those that carry eight data bytes back |
| 1 | `81 28 00 00` | 5 | |
| 1 | `81 27 5a 00` | 5 | set the terminator byte |
| 1 | `81 26 5a 00` | 13 | the card's own geometry, twelve bytes back |
| 1 | `81 23 00 00` | 9 | set the page to read: page 0 |
| 4 | `81 43 80 00` | 134 | 128 bytes each: 512 of data |
| 1 | `81 43 10 00` | 22 | the sixteen bytes after the page |
| 1 | `81 81 00 00` | 4 | end the read |
| | `81 23 e0 3f` then the same five frames | | page `0x3fe0` = 16352 |
| | `81 23 e1 3f` then the same five frames | | page 16353 |
| | `81 23 52 00` then the same five frames | | page `0x52` = **82** |

Three things this settles.

- **The page-read shape in the sibling project's notes holds exactly**: set the
  page with `0x23`, take it in `0x43` chunks of 128 with a 16-byte tail, end
  with `0x81`. A frame's length is its payload plus six.
- **The three pages read at mount are the superblock and two others.** Page 0
  is the superblock. Pages 16352 and 16353 are the first two of erase block
  1022, which is the first of the two spare blocks the superblock names at
  `+0x40` (§0.1).
- **Page 82 is the root directory**, and the ground truth predicts exactly
  that: the superblock's first allocatable cluster is 41, a cluster is two
  pages, and the root directory's cluster is 0 relative to that. `2 * 41 = 82`.
  The static reading and the on-image reading agree without either being
  fitted to the other.

Over the whole run the command mix is `0x42` (4080, writes), `0x43` (1920),
`0x81` (1200), `0x22` (816), `0x23` (384), then the mount's own handful. The
read path is the small part of it; everything above `0x42`'s count is the
title saving.


Dynamic witnesses, and their limits: a serial trace of the reference BIOS
under the sibling emulator) shows, for port 0, `0x11` x5, `0xf3`, the
`SECRMAN` exchange, `0x28`, `0x27`, `0x26`, then `0x23`, `0x43 80` x4, `0x43
10`, `0x81` with the table's lengths, and for the empty port 1 exactly
§2.1's fallback (`0x11` x5, `0xf3`, the 3-byte `81 52` x5). That run is
`XMCMAN`'s, so it corroborates the frame shapes and the fallback order, not
this binary's addresses. §7 above (a title's 2.30 driver) adds the journal pages and page 82 in the order §4.3 predicts.

Regions never entered: `0x4dbc`-`0x5fd4` (open/create/handles), `0x1b28`
(write page) and `0x18b4` (erase) beyond their frame shape, `0x11b0`/`0x2e10`/
`0x28e4` (format, bad-block scan), `0x3f14`/`0x4100`/`0x56d8` (read/set-info/
chdir bodies), `0x6d44`-`0x95f0` (the PS1 driver), the ECC internals of
`0x2604`/`0x2678`, `0x46a0` past its head, `0x6748`-`0x690c` (journal
replay), `0xb01c`-`0xb59c` (format/remove/mkdir/rmdir/chstat/ioctl glue).
