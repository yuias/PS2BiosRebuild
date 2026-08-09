# ROM Layout of the Reference Images

All observations in this document were produced with `tools/romdir.py` against
the images in `assets/`; the few raw-byte checks name their command inline.
Offsets are ROM file offsets. (The ROM is mapped at `0xBFC00000` on both CPUs;
per-CPU address treatment is deferred to the boot-sequence analysis.)

## Reference images

| File | Size | ROMVER | SHA-256 |
| --- | --- | --- | --- |
| `SCPH-50000.bin` | 4194304 | `0170JC20030206` | `6c003a0ca1beb501fba2063b12573521c9b3ec8cdcfb7f5e6a310771ebf166da` |
| `SCPH-70000.bin` | 4194304 | `0200JC20040614` | `3b377ce5f7bb8d880260851867a5452b59149aeb8bb189c5df41e3bace7b8e09` |

```sh
sha256sum assets/SCPH-50000.bin assets/SCPH-70000.bin
python3 tools/romdir.py assets/SCPH-50000.bin --romver
```

`ROMVER` decodes as `VVVVRTYYYYMMDD`: BCD version (`0170` → 1.70), region
letter (`J`), console-type letter (`C` on both retail images), build date.
`SCPH-50000` is the primary analysis target; `SCPH-70000` is the comparison
image that keeps observations honest about what is model-specific.

The ROM also carries a human-readable banner in the `VERSTR` file —
`System ROM Version 5.0 02/06/03 J` on `SCPH-50000` — whose version number
(5.0) tracks the OSD's displayed version, not `ROMVER`'s 1.70. The two version
schemes are independent and both will need reproducing (with our own text) at
their own offsets.

## The ROM is a file archive

Unlike the PS1 ROM — one monolithic image with a kernel at a fixed offset — the
PS2 ROM is a flat archive: files stored back to back, each padded to a 16-byte
boundary, described by a table named `ROMDIR`. Everything else in this project
hangs off that fact: analysis proceeds per-file, and a rebuilt image must
assemble the same archive shape.

### ROMDIR entry format

The table is an array of 16-byte entries, terminated by an all-zero entry:

| Offset | Size | Field | Notes |
| --- | --- | --- | --- |
| 0 | 10 | `name` | NUL-padded ASCII; `-` marks an unnamed padding region |
| 10 | 2 | `extinfo_size` | bytes this file owns inside `EXTINFO` (LE) |
| 12 | 4 | `size` | file size in bytes (LE); storage rounds up to 16 |

File offsets are not stored anywhere: a file's position is the running sum of
the 16-byte-aligned sizes of every entry before it, starting at offset 0. The
first entry, `RESET`, describes the boot block at the very top of the ROM, so
the sum starts from the image start. The `-` padding entries exist purely to
advance that sum — on `SCPH-50000` they land `KROMG`/`KROM`/`ROMGSCRT` on
`0x1000`-aligned boundaries and `IOPBOOT` on `0x4A000`.

The table has no pointer to it either. It is found by scanning for the `RESET`
entry — its own storage follows the boot block immediately: `RESET` is 10048
(`0x2740`) bytes on `SCPH-50000`, and the table sits at `0x2740` in both
images. `tools/romdir.py` verifies a candidate by requiring entry 1 to be
`ROMDIR` with a size that is a whole number of entries; on both images that
size also equals (entry count + zero terminator) × 16, and the `ROMDIR` entry's
computed offset equals the location the scan found — the table describes
itself consistently.

```sh
python3 tools/romdir.py assets/SCPH-50000.bin --list
python3 tools/romdir.py assets/SCPH-70000.bin --list
```

`SCPH-50000` has 98 entries with contents ending at `0x3b9d10`; `SCPH-70000`
has 101 ending at `0x3b6b40`. In both, every byte from there to the 4 MiB end
is `0x00`:

```sh
python3 -c "d=open('assets/SCPH-50000.bin','rb').read(); print(set(d[0x3b9d10:]))"
```

### EXTINFO record format

`EXTINFO` is itself one of the files: concatenated per-file metadata, sliced by
the running sum of `extinfo_size` in entry order (the sum over all entries
equals the `EXTINFO` file size exactly, on both images). A slice is a sequence
of records, each a 4-byte header `value:u16 size:u8 type:u8` followed by `size`
payload bytes:

| Type | Meaning | Encoding |
| --- | --- | --- |
| `0x01` | date | payload 4 bytes, little-endian BCD: `DD MM YY YY` |
| `0x02` | version | `value` is the version in BCD (`0x0101` → 1.01) |
| `0x03` | comment | NUL-terminated ASCII, padded to a 4-byte boundary |
| `0x7F` | null | no payload |

```sh
python3 tools/romdir.py assets/SCPH-50000.bin --extinfo SYSMEM
python3 tools/romdir.py assets/SCPH-50000.bin --extinfo all
```

Most modules carry date + version + a short module-name comment (`SYSMEM`:
2002-04-03, v1.01, `System_Memory_Manager`). The `ROMDIR` entry's own slice is
a single 84-byte comment recording the original build: timestamp, the
`ROMconf` tool name, the image's output filename and a user@host build path —
useful as provenance, and a reminder that `EXTINFO` content in a rebuilt image
is free-form text we author ourselves.

## What the archive contains

Grouping the `--list` output by what the names and first bytes establish so
far. Per-file magic checks:

```sh
python3 tools/romdir.py assets/SCPH-50000.bin --extract <outdir>   # outside the repo
file <outdir>/SYSMEM <outdir>/KERNEL <outdir>/OSDSYS
```

| Group | Files | Notes |
| --- | --- | --- |
| Boot block | `RESET` | Raw code at offset 0; both CPUs' reset path. First words are MIPS instructions, not a container format. |
| Archive metadata | `ROMDIR`, `EXTINFO`, `ROMVER` | This document. |
| IOP boot configuration | `IOPBTCONF`, `IOPBTCON2` | ASCII text: `@800` (a load address) then one module name per line, in load order — `SYSMEM`, `LOADCORE`, `EXCEPMAN`, `INTRMANP`, … The list is the IOP boot sequence in data form. |
| IOP kernel modules | `SYSMEM` … `EESYNC` (the `IOPBTCONF` names), plus the `X`-prefixed updates (`XLOADFILE`, `XCDVDMAN`, `XMCMAN`, …) and drivers (`MCMAN`, `PADMAN`, `CDVDMAN`, `LIBSD`, …) | ELF objects (`\x7fELF`) — relocatable IOP modules. The `X` variants are newer revisions selected at run time; several exist alongside their unprefixed originals. |
| EE side | `KERNEL`, `EELOAD`, `EENULL`, `EELOADCNF` | `KERNEL` is raw R5900 code (no container). `EELOAD` starts with zero padding, format TBD. |
| OSD program | `OSDSYS`, `OSDCNF`, `OSDVER`, `OSDSND` | `OSDSYS` is an ELF. |
| PS1 compatibility | `PS1DRV`, `PS1ID`, `PS1VER`, `TZLIST` | The PS2 boots PS1 discs through these. |
| Artwork / resources | `LOGO`, `PS2LOGO`, `FONTM`, `FNTIMAGE`, `KROM`, `KROMG`, `TEXIMAGE`, `ICOIMAGE`, `SNDIMAGE`, `IGREETING`, `VERSTR` | Non-code material; out of bounds for reproduction per `docs/clean-room-policy.md` §3. |
| Test / factory | `TESTMODE`, `TESTSPU`, `TBIN`, `TSIO2MAN`, `TPADMAN` | Name-based guess at this stage. |
| Storage / expansion | `ATAD`, `HDDLOAD`, `XFLASH`, `XFROMMAN` | HDD and flash support; absent from `SCPH-70000` (below). |

Group membership beyond the verified magics is name-based and provisional;
later per-group analyses will firm it up.

## SCPH-70000 differences at the table level

```sh
python3 tools/romdir.py assets/SCPH-50000.bin --compare assets/SCPH-70000.bin
```

- **Removed** (SCPH-50000 → SCPH-70000): `ATAD`, `HDDLOAD`, `XFLASH`,
  `XFROMMAN` — the slim model dropped the HDD expansion — and `PS1VER`.
- **Added**: per-region `PS1VERJ/A/E/C/H`, `XDEV9`, `XDEV9SERV`, `NCDVDMAN`.
- **Changed**: the boot block, OSD resources, PS1 driver, CDVD stack and most
  version/config files differ in size or bytes, as expected across a year of
  revisions.
- **`KERNEL` is byte-identical** in both images (93736 bytes): the EE kernel
  did not change between ROM v1.70 (2003) and v2.00 (2004). The same holds for
  most of the core IOP modules (`SYSMEM`, `LOADCORE`, `THREADMAN`, … — absent
  from the diff output, which prints only differences). The stable core is
  good news for a reimplementation: one implementation covers both targets.

## Consequences for the rebuild

1. The build must produce the archive: a `ROMDIR` table whose computed offsets
   match the storage layout, an `EXTINFO` with our own metadata, a `ROMVER`
   and `VERSTR` with our own text at the formats above.
2. Alignment padding (`-` entries) is load-bearing where consumers expect
   aligned files; which consumers those are is a question for the boot and OSD
   analyses.
3. The trailing region to 4 MiB is zero fill.
