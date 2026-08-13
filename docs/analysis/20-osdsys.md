# OSDSYS

`OSDSYS` is the largest file in the archive at 336808 bytes, and the one the
boot ends at: `docs/analysis/15-ee-syscall-groups.md` showed syscall `0x7B`
launching `rom0:OSDSYS` with `argv = { "BootBrowser" }`. It was left until last
deliberately — with the syscall groups characterised its call sites are
readable — and it turns out to need much less analysis than its size suggests.

## It is an EE executable, not a module

```sh
python3 tools/romdir.py assets/SCPH-50000.bin --extract <outdir>   # outside the repo
readelf -h <outdir>/OSDSYS
readelf -l <outdir>/OSDSYS
```

| Field | Value |
| --- | --- |
| `e_type` | `ET_EXEC` — one of the four EE executables of `spec/02` |
| `e_flags` | `0x20924001` — `noreorder`, **5900**, `eabi64`, `mips3` |
| entry | `0x00100008` |
| segments | one `PT_LOAD`, `0x5217C` bytes at `0x00100000`, RWE |

`0x00100000` is where an EE program is loaded — the same address the loader of
syscall `0x06` places a program at — so `OSDSYS` is an ordinary user program
that happens to ship in the ROM, not a kernel component. Note also that
`readelf` names the R5900 in `e_flags` here, where the IRX modules of
`spec/02` carry no arch flags at all.

## Ninety-nine percent of it is one compressed blob

The section table is the whole story:

```sh
readelf -S <outdir>/OSDSYS
```

| Section | Size | Share |
| --- | --- | --- |
| `.text` | `0xAF8` (2808) | 0.8% |
| `.text.Expand` | `0x34` | |
| `.text.ExpandMain` | `0x12C` | |
| `.text.ExpandSetBlock` | `0x88` | |
| `.text.ExpandInit` | `0x1C` | |
| **`.data`** | **`0x513FC` (332796)** | **99%** |

All the code there is amounts to `0xCFC` bytes — 3324 — of which 484 are the
four `Expand` sections. **`OSDSYS` is a small decompressor wrapped around a
large compressed payload**, and the real OSD program is inside `.data`.

The entry does what that implies:

```sh
python3 - <<'PY'
d = open('<outdir>/OSDSYS', 'rb').read()
open('<outdir>/OSDSYS.seg', 'wb').write(d[0x80:0x80 + 0x5217c])
PY
python3 tools/romdis.py <outdir>/OSDSYS.seg --cpu ee --vma 0x00100000 --range 0x100008 0x100040
```

```
  100008  lui   $v0, 0x15
  100010  addiu $v0, $v0, 0x2180     # 0x152180
  100014  addiu $v1, $v1, 0x2364     # 0x152364
  100018  <sq>                       # 16 bytes at a time
  100024  sltu  $at, $v0, $v1
  10002c  bnez  $at, 0x100018
  100030  addiu $v0, $v0, 0x10
  100034  lui   $a0, 0x16            # then set up the expander's arguments
  100038  lui   $a1, 0x0
  10003c  lui   $a2, 0x10
```

— clear a range with quadword stores, then call the expander with a destination
above the loaded image and the image itself as source.

`.data` opens with a plausible header rather than code:

```sh
python3 -c "
d = open('<outdir>/OSDSYS','rb').read(); print(d[0xe00:0xe10].hex(' ', 4))"
# d49f0b00 40001820 00c00000 2c00023c
```

The first word is `0x000B9FD4` — 761812 — against 332796 bytes stored, a ratio
of about 2.3:1. That is consistent with an expanded size, and the byte
distribution of `.data` supports the reading: all 256 values occur, with the
commonest (`0x00`) at only 7.6%, which is what compressed data looks like and
what neither code nor artwork does.

Both readings are **inferences from shape**, not decoded facts. Establishing
the compression format would mean working through `ExpandMain`, and the next
section is the argument for not doing that.

(`docs/analysis/27-osdsys-payload-and-config.md` later confirmed both without
decoding anything: it *runs* the expander under `tools/eesim.py`, and the size
it returns is this header word exactly.)

## What a rebuild actually owes here

The payload is the OSD: menus, the browser, fonts, icons, sounds. Almost all of
it is the material `docs/clean-room-policy.md` §3 puts out of bounds —
`FONTM`, `FNTIMAGE`, `TEXIMAGE`, `ICOIMAGE`, `SNDIMAGE` and `OSDSND` are its
sibling files in the archive for the same reason. **A rebuild is not going to
reproduce this payload and should not try.**

What it does owe is the **contract around it**, and that contract is small:

- a file named `OSDSYS` in the archive, reachable as `rom0:OSDSYS`
  (`docs/analysis/11-sif-and-rom-driver.md`);
- an `ET_EXEC` ELF for the R5900 with one `PT_LOAD` at `0x00100000`;
- an entry that accepts `argc`/`argv` as the loader of syscall `0x06` passes
  them, and understands at least the argument `"BootBrowser"` that the default
  boot supplies (`spec/04` EE-9c).

Everything else — what the program *shows* — is ours to design. The
decompressor exists because the original needed to fit 761 KB of resources into
333 KB of ROM; a rebuild whose OSD is a fraction of that size does not need one
at all, and may ship a plain uncompressed program at the same contract.

## What this pins for the rebuild

- `OSDSYS` is an ordinary EE user program: `ET_EXEC`, R5900 flags, one
  `PT_LOAD` at `0x00100000`, entry `0x00100008`.
- It must be launchable as `rom0:OSDSYS` with `argv = { "BootBrowser" }`.
- The compression is an implementation detail of the original's size problem,
  not part of the interface, and need not be reproduced.
- Its payload is out-of-bounds material; only the container and entry contract
  are reproduced.

With this, every major component of the image has been examined. What remains
is depth — the per-slot behaviour of the EE syscalls, and the implementation
itself (`docs/project-state.md` §5).
