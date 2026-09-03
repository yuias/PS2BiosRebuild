# Module Loading and Boot Configurations

`VBLANK`, `IOMAN` and `MODLOAD` are boot-list entries fourteen to sixteen
(`docs/analysis/03-iopboot-and-boot-list.md`). `MODLOAD` is the one that matters
structurally: it performs the load-relocate-start cycle for every module loaded
after boot, and reading it closes **both** questions
`docs/project-state.md` had open — what makes a module non-resident, and how an
alternative boot list is selected.

```sh
python3 tools/romdir.py assets/SCPH-50000.bin --extract <outdir>   # outside the repo
for m in VBLANK IOMAN MODLOAD; do python3 tools/irxinfo.py <outdir>/$m --imports; done
```

| Module | Library | Exports | Notable imports |
| --- | --- | --- | --- |
| `VBLANK` | `vblank` 1.01 | 10 | `thbase` 41; `thevent` 4, 7, 9, 10 |
| `IOMAN` | `ioman` 1.02 | 25 | `sysclib` ×7 |
| `MODLOAD` | `modload` 1.01 | 16 | `loadcore` ×12, plus `ioman`, `thbase`, `thevent`, `thsemap` |

`MODLOAD` imports twelve `loadcore` ordinals — by far the widest use of it —
including ordinal 8, the import binder `docs/analysis/06-exceptions-and-interrupts.md`
found. It is the module that turns `LOADCORE`'s primitives into a usable loader.

## Starting a module, and the residency rule

The call site is at `MODLOAD` offset `0xB10`:

```sh
python3 tools/irxinfo.py <outdir>/MODLOAD --dump-load <outdir>/MODLOAD.text
python3 tools/romdis.py <outdir>/MODLOAD.text --cpu iop --vma 0 --range 0xaf0 0xb90
```

```
 af0:  lw    $t0, 0x14($s5)     # module record +0x14: the module's gp
 af8:  move  $gp, $t0           # install it
 afc:  move  $a0, $s2           # argc
 b00:  move  $a1, $s3           # argv
 b04:  move  $a2, $zero
 b08:  lw    $v0, 0x10($s5)     # module record +0x10: the entry point
 b10:  jalr  $v0
 b14:  move  $a3, $s5           # 4th argument: the module record
 b18:  move  $s0, $v0           # keep the return value
 b1c:  move  $gp, $s7           # restore MODLOAD's own gp
 ...
 b38:  andi  $v0, $s0, 3        # <-- the residency test
 b40:  beqz  $v0, 0xb84         # (ret & 3) == 0  -> stays resident
 b48:  jal   0x1828             # otherwise: tear the module down
```

So a module entry is called as `entry(argc, argv, 0, module_record)` with the
module's own `$gp` installed, and **residency is decided by the low two bits of
its return value**: clear means resident, set means unload.

That is the answer to the open question, and it is a mask rather than an
equality test — the `1` returned by the `P`/`I` variants (`06`), by `EECONF`
(`09`) and by `THREADMAN` on registration failure (`09`) are all just one value
that satisfies it.

The teardown path is explicit about what "non-resident" costs:

```
 b48:  jal 0x1828              # intrman 17  -- critical section
 b50:  lw  $a0, 0x18($s5)
 b54:  lw  $a1, 0x1c($s5)
 b58:  jal 0x1730              # loadcore 9
 b60:  jal 0x1748              # loadcore 17, with the module record
 b68:  srl $a0, $s5, 8
 b6c:  jal 0x16cc              # sysmem 5, base rounded down to 0x100
 b70:  sll $a0, $a0, 8
 b78:  jal 0x1830              # intrman 18  -- leave the critical section
 b80:  move $v0, $zero
```

Two things fall out of this. **`sysmem` ordinal 5 is the deallocator**, the
counterpart of the ordinal 4 allocator identified in
`docs/analysis/05-sysmem-and-loadcore.md` — and the address is rounded down to a
`0x100` boundary before being freed, matching the `0x100` granularity `SYSMEM`'s
init imposes. And the module record's layout is partly pinned: `+0x10` entry,
`+0x14` gp, `+0x18`/`+0x1C` the pair `loadcore` 9 consumes.

## Boot configurations are nested archives

The second open question — which selector chooses `IOPBTCON2` over `IOPBTCONF` —
turns out to rest on a false premise. Searching the whole ROM for both names:

```sh
python3 - <<'PY'
d = open('assets/SCPH-50000.bin', 'rb').read()
for name in (b'IOPBTCONF', b'IOPBTCON2'):
    hits, at = [], -1
    while True:
        at = d.find(name, at + 1)
        if at < 0:
            break
        hits.append(hex(at))
    print(name.decode(), hits)
PY
# IOPBTCONF ['0x27a0', '0x491c0', '0x4b154', '0x4b190', '0xb3e58']
# IOPBTCON2 ['0x27b0']
```

`IOPBTCON2` occurs **once**, in the ROMDIR table itself: no *literal* copy of
the name exists anywhere else. That is true and it is also a trap — see the
correction at the end of this section.  `IOPBTCONF`, by contrast, occurs in `IOPBOOT` (which resolves it, per
`03`), in `UDNL`, and — the interesting ones — inside `EELOADCNF` and `OSDCNF`.

Those two are not configuration text. They are **complete, nested ROMDIR
archives**, each carrying its own `IOPBTCONF`:

```sh
python3 tools/romdir.py <outdir>/EELOADCNF --list
python3 tools/romdir.py <outdir>/OSDCNF --list
```

```
name          offset      size extinfo
RESET            0x0         0       8
ROMDIR           0x0        80      84
EXTINFO         0x50       100       0
IOPBTCONF       0xc0       236       8
```

The same format as the ROM's own archive (`01-rom-layout.md`), with one
adaptation: `RESET` has **size 0**, so the running offset sum puts the ROMDIR
table at offset 0 — where it is. The self-locating scan finds it by exactly the
same rule. (Parsing these is what turned up a too-strict bounds check in
`tools/romdir.py`: a nested archive ends at its last file's final byte, with the
alignment padding omitted.)

Extracting the embedded lists shows what they are for:

```sh
python3 tools/romdir.py <outdir>/EELOADCNF --extract <outdir>/eeloadcnf
python3 tools/romdir.py <outdir>/OSDCNF --extract <outdir>/osdcnf
diff <outdir>/IOPBTCONF <outdir>/eeloadcnf/IOPBTCONF
diff <outdir>/IOPBTCONF <outdir>/osdcnf/IOPBTCONF
```

- **`EELOADCNF`** differs from the ROM's list by exactly two substitutions:
  `LOADFILE` → `XLOADFILE` and `CDVDMAN` → `XCDVDMAN`. Same length, newer
  variants of two modules.
- **`OSDCNF`** substitutes five (`SIFCMD`, `LOADFILE`, `CDVDMAN`, `CDVDFSV`,
  `FILEIO` for their `X` forms), inserts `ADDDRV`, and appends the peripheral
  set the shell needs: `RMRESET`, `CLEARSPU`, `XSIO2MAN`, `XMTAPMAN`, `XMCMAN`,
  `XMCSERV`, `XPADMAN`, `XRMMAN2`, `OSDSND`.

The `X`-prefixed modules `01-rom-layout.md` noted as "newer revisions selected at
run time" are therefore selected *here* — by which archive's list is booted, not
by a per-module decision.

So alternative IOP configurations are not selected by a flag at all: they are
**delivered as archives**, and rebooting the IOP against one of them replaces
the module set wholesale. `MODLOAD`'s own strings show that side of it —
`Reboot fail! need file name argument`, `updater '%s' can't load`,
` ReBootStart: Terminate resident Libraries` — a reboot names the image to boot
from and tears down the resident libraries first.

**Correction (`docs/analysis/45`): `IOPBOOT` does name it — by building the
name, not by holding it.** `IOPBOOT+0xd0`..`+0x110` copies its own
`"IOPBTCONF"` onto the stack and overwrites the ninth byte with `'0' + mode`
(`addiu $2, $17, 0x30` / `sb $2, 0x98($sp)`), looking the result up and
falling back to the unmodified name when it is absent. A reboot carrying an
argument enters `IOPBOOT` with mode `2`, so it boots `IOPBTCON2`. A search
for the literal string could never have found this; the lesson is that a name
absent from the bytes is evidence about the bytes, not about the mechanism.

`IOPBTCON2`'s contents fit the use: it keeps `CDVDMAN`, `SIO2MAN`, `MCMAN`
and `ADDDRV`, and drops everything that talks to the EE (`SIFCMD`, `SIFINIT`,
`EESYNC`, `REBOOT`, `LOADFILE`, `CDVDFSV`, `FILEIO`, `EECONF`) — an
intermediate kernel that can read a disc and nothing else.

## What this pins for the rebuild

- A module entry is `entry(argc, argv, 0, module_record)`, called with the
  module's `$gp` installed, and **`return & 3` decides residency** — a mask, not
  an equality.
- Non-residency frees the module through `sysmem` ordinal 5 at `0x100`
  granularity, inside an `intrman` 17/18 critical section.
- Module record offsets `+0x10` (entry) and `+0x14` (gp) are ABI.
- The archive format must nest: `EELOADCNF` and `OSDCNF` are ROMDIR archives
  whose `RESET` entry has size 0, and a rebuild must produce them that way for
  the self-locating scan to land on the table.

Next in boot order: `ROMDRV`, `SIFMAN` and `SIFCMD` — the ROM file driver and
the EE/IOP interface (`docs/project-state.md` §5).
