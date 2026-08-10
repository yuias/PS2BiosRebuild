# Specification: The ROM Archive

Derived from `docs/analysis/01-rom-layout.md` and
`docs/analysis/10-module-loading-and-boot-configs.md`.

This is the outermost structure of the image: everything else the build produces
is a file inside it. It is specified first because four independent consumers in
the reference ROM depend on its exact shape — the boot block, `IOPBOOT`,
`MODLOAD` and `ROMDRV` each locate the table by scanning
(`docs/analysis/11-sif-and-rom-driver.md` §"The self-locating scan, four times
over") — so nothing here is a free choice.

## ARC-1: Image size and tail

The image is exactly **4194304 bytes** (4 MiB). Archive contents begin at offset
0 and occupy a prefix of the image; every byte from the end of the last file to
the end of the image is `0x00`.

The reference images leave roughly 280 KiB of tail. There is no requirement to
match their exact content end, only to zero-fill whatever remains.

## ARC-2: Entry format

The table is an array of 16-byte entries, little-endian throughout:

| Offset | Size | Field | Notes |
| --- | --- | --- | --- |
| 0 | 10 | `name` | ASCII, NUL-padded. A 10-character name has no terminator. |
| 10 | 2 | `extinfo_size` | Bytes this entry owns inside `EXTINFO`; may be 0. |
| 12 | 4 | `size` | File size in bytes; may be 0. |

The array is terminated by a **16-byte all-zero entry**. A parser stops at the
first entry whose `name` field begins with a NUL.

## ARC-3: Offsets are implied, never stored

A file's offset is not recorded anywhere. It is the running sum, starting at 0,
of `align16(size)` over every preceding entry:

```
offset(0)   = 0
offset(i+1) = offset(i) + ((size(i) + 15) & ~15)
```

A build must lay files out to match this computation exactly. Any padding
inserted between files must be declared as an entry (ARC-5), never left implicit.

## ARC-4: The table locates itself

The first entry is named `RESET`, and the table is stored immediately after the
`RESET` file — which ARC-3 makes automatic, since `RESET` is entry 0 and the
`ROMDIR` file is entry 1.

Consumers find the table by scanning the image for a candidate `RESET` entry and
validating it. The build must therefore satisfy the validation the reference
consumers perform:

- entry 0 is named exactly `RESET`;
- entry 1 is named exactly `ROMDIR`;
- entry 1's `size` is a whole number of 16-byte entries;
- `offset(1)` — computed by ARC-3 — equals the address at which the table was
  found.

**ARC-4a:** The `ROMDIR` entry's `size` is `(entry_count + 1) * 16`, counting the
all-zero terminator of ARC-2 but not itself twice. Verified on both reference
images and on the nested archives (`98` entries → `1584`; `4` entries → `80`).

## ARC-5: Padding entries

A file may need to start at a boundary coarser than 16 bytes. Alignment is
achieved by inserting an entry named `-` (a single hyphen) whose `size` is the
number of padding bytes; ARC-3 then accounts for it like any other file.

Padding entries are not addressable by name and must be skipped when resolving a
name. There may be several; the reference image uses five.

## ARC-6: EXTINFO

`EXTINFO` is a file in the archive holding per-entry metadata, concatenated in
entry order. Entry `i` owns the `extinfo_size(i)` bytes beginning at the running
sum of `extinfo_size` over entries `0..i-1`.

**ARC-6a:** The `EXTINFO` file's `size` equals the sum of `extinfo_size` across
all entries.

**ARC-6b:** Within an entry's slice, records are a 4-byte header followed by
`size` payload bytes:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 2 | `value` |
| 2 | 1 | `size` (payload length) |
| 3 | 1 | `type` |

| `type` | Meaning | Encoding |
| --- | --- | --- |
| `0x01` | date | 4-byte payload, little-endian BCD `DD MM YY YY` |
| `0x02` | version | carried in `value`, BCD (`0x0101` → 1.01) |
| `0x03` | comment | NUL-terminated ASCII, payload padded to a 4-byte multiple |
| `0x7F` | null | no payload |

**ARC-6c:** Where an entry is an IOP module, its `0x03` comment and `0x02`
version must equal the module's own `.iopmod` name and version. The reference
keeps the two consistent and a rebuild must too — they are produced from one
source, not two.

## ARC-7: ROMVER

`ROMVER` is a 16-byte file whose content is 14 ASCII characters followed by
`\n` and `\0`:

```
VVVVRTYYYYMMDD\n\0
```

`VVVV` is a BCD version, `R` a region letter, `T` a console-type letter, and the
rest an ISO date without separators. Tools identify an image by this file, so the
layout is fixed even though the values are ours to choose.

**ARC-7a — deviation:** the values must not impersonate a retail ROM. The version,
region and type characters identify this project's image. See
`docs/clean-room-policy.md` §3.

## ARC-8: Archives nest

A file inside the archive may itself be a complete archive in this format. The
reference uses this for boot configurations (`EELOADCNF`, `OSDCNF`), each of
which carries its own `IOPBTCONF`.

**ARC-8a:** In a nested archive the `RESET` entry has `size` **0**, so ARC-3 puts
the table at offset 0 — where the scan of ARC-4 finds it. There is no separate
mechanism; the same rules produce the different placement.

**ARC-8b:** The alignment padding of the *final* file may be omitted: a nested
archive ends at its last file's last byte. A parser must bound-check content
rather than content-plus-padding.

## ARC-9: Name resolution

Names are matched over the full 10-byte field, NUL padding included. Resolution
returns the first matching entry in storage order. Duplicate names are not used
by the reference and a build must not create them.

## ARC-10: What the build must not reproduce

The archive's *structure* is dictated by the machine and is reproduced exactly.
Its *contents* are not, where they are expression rather than interface:

- the identification and copyright text in the boot block and `VERSTR`;
- the `EXTINFO` build-provenance comment on the `ROMDIR` entry, which records
  the original build host and path — our own text goes there;
- the artwork, font, sound and OSD resource files listed in
  `docs/clean-room-policy.md` §3.

File *names* are interface — consumers resolve by name — and are reproduced.

## Verification

`tools/mkromdir.py` builds an archive from a manifest, and
`tools/romdir.py --list` parses it. The round-trip check is that building an
archive from the reference's own extracted files reproduces the reference's
table region byte for byte: same entry order, same `extinfo_size` and `size`
fields, same computed offsets. That exercises ARC-2 through ARC-6a against real
data without any of it entering the repository.
