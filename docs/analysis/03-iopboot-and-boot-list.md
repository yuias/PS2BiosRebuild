# IOPBOOT and the Boot List

The IOP reset path ends by locating `IOPBOOT` in the archive and jumping to it
(`docs/analysis/02-boot-block.md` §"IOP reset path"). `IOPBOOT` is the loader
that turns the archive into a running IOP kernel: it reads the ordered
module-name list in `IOPBTCONF` and brings each named module up in turn. This
document establishes `IOPBOOT`'s role and the `IOPBTCONF` format; the mechanics
of loading an individual ELF module (relocation, entry conventions) are the
subject of the module-format survey (`docs/project-state.md` §5, step 2).

## IOPBOOT is raw IOP code, not a module

Unlike the modules it loads, `IOPBOOT` is not an ELF file — its first bytes are
MIPS instructions, and the boot block enters it by a plain `jr` to its ROM
address (`0xBFC4A000` on `SCPH-50000`, i.e. ROM offset `0x4A000`; the `-`
padding entry before it aligns it there — `01-rom-layout.md`). It runs in place
from the ROM window:

```sh
python3 tools/romdir.py assets/SCPH-50000.bin --extract <outdir>   # outside the repo
python3 tools/romdis.py <outdir>/IOPBOOT --cpu iop --vma 0xbfc4a000 --range 0xbfc4a000 0xbfc4a030
```

Its entry sizes a stack from the RAM-size argument the boot block passes in
`$4` (`02` §"IOP reset path", step 6), sets `$gp`, and — like the boot block —
carries its own self-locating ROMDIR search (the `RESET`/`0x54` marker
constants are embedded at file offset `0xCC0`, the routine at `0xBFC4ACB0`). It
uses that search to find `IOPBTCONF` by name; the string is at file offset
`0x1154`:

```sh
python3 -c "d=open('<outdir>/IOPBOOT','rb').read(); print(d[0x1154:0x115d])"
# b'IOPBTCONF'
```

## The IOPBTCONF format

`IOPBTCONF` is a short ASCII text file — one of the archive members — that
`IOPBOOT` parses token by token, whitespace-separated (any byte below `0x20`
ends a token). The first byte of each token selects how it is handled:

| Leading byte | Meaning |
| --- | --- |
| `@` (`0x40`) | Set the base load address: the rest of the token is parsed as hex. `@800` sets the base to `0x800`. |
| `#` (`0x23`) | A directive line. The parser matches one fixed keyword (`"!addr "`, at file offset `0x1120`); it is **not used** by either retail boot list, so its exact effect is left for later. |
| anything else | A module name. It is looked up in the ROMDIR by name; a hit records the module's ROM address into an ordered array, a miss aborts the boot. |

```sh
python3 tools/romdir.py assets/SCPH-50000.bin --extract <outdir>   # outside the repo
cat <outdir>/IOPBTCONF
```

produces, for `SCPH-50000`:

```
@800
SYSMEM
LOADCORE
EXCEPMAN
INTRMANP
INTRMANI
SSBUSC
DMACMAN
TIMEMANP
TIMEMANI
SYSCLIB
HEAPLIB
EECONF
THREADMAN
VBLANK
IOMAN
MODLOAD
ROMDRV
STDIO
SIFMAN
IGREETING
SIFCMD
REBOOT
LOADFILE
CDVDMAN
CDVDFSV
SIFINIT
FILEIO
SECRMAN
EESYNC
```

So the file is exactly what it looks like: a base address (`0x800`, the start
of usable IOP RAM once the exception vectors and low structures below it are
reserved) followed by the modules to load, in load order. The parser records
each resolved module address in sequence; the load/relocate/run step that
consumes that array is deferred to the module-format survey.

## Load order is not storage order

The names in `IOPBTCONF` are the **load order**, which is deliberately distinct
from the order the same modules are stored in the archive. Comparing the boot
list against the `--list` output (`01-rom-layout.md`) shows the reordering:
`DMACMAN` is stored *after* the two timer modules (`SSBUSC`, `TIMEMANP`,
`TIMEMANI`, `DMACMAN` in the archive) but the boot list loads it *before* them
(`SSBUSC`, `DMACMAN`, `TIMEMANP`, `TIMEMANI`); and `EECONF`, stored much later
in the archive, is loaded early (right after `HEAPLIB`).

```sh
diff <(sed -n '2,$p' <outdir>/IOPBTCONF) \
     <(python3 tools/romdir.py assets/SCPH-50000.bin --list | awk 'NR>1{print $1}')
```

The lesson for the rebuild: the archive's file order and the kernel's boot
order are independent, and both matter — storage order for the offsets the
ROMDIR search computes, load order for the sequence `IOPBOOT` brings modules up
in. A rebuilt image must get both right, and they cannot be derived from each
other.

## A second boot list: IOPBTCON2

The archive also carries `IOPBTCON2`, a shorter list (`cat <outdir>/IOPBTCON2`)
that drops the SIF/EE-bridge and file-service modules and adds `ADDDRV`,
`SIO2MAN`, `MCMAN` — a different IOP configuration selected for some boot modes.
Which selector chooses it, and where it is read, is a question for the analysis
of the code that consumes these lists; recorded here so the second list is not
mistaken for dead data.

## What this pins for the rebuild

- `IOPBOOT` must be raw IOP code entered by `jr` from the boot block, carry its
  own ROMDIR search, and resolve `IOPBTCONF` by name — no stored pointers.
- The `IOPBTCONF` parser must honour the `@`-hex base-address token and treat
  every plain token as a ROMDIR module name, aborting on a miss.
- The boot list defines load order independently of archive storage order.

Next: the IOP module (ELF/IRX) format and the load/relocate/export-resolve step
`IOPBOOT` performs per module, starting with `SYSMEM` and `LOADCORE` — the base
the rest of the list links against (`docs/project-state.md` §5, steps 2–3).
