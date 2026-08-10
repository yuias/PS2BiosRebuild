# The SIF and the ROM File Driver

`ROMDRV`, `SIFMAN`, `SIFCMD` and `SIFINIT` are boot-list entries seventeen,
nineteen, twenty-one and twenty-six
(`docs/analysis/03-iopboot-and-boot-list.md`). The three SIF modules are the
IOP's side of the EE↔IOP interface; `ROMDRV` publishes the ROM archive as a
file system. Together they give the first clear look at how the residency rule
from `docs/analysis/10-module-loading-and-boot-configs.md` is used deliberately
rather than as an error path.

```sh
python3 tools/romdir.py assets/SCPH-50000.bin --extract <outdir>   # outside the repo
for m in ROMDRV SIFMAN SIFCMD SIFINIT; do python3 tools/irxinfo.py <outdir>/$m --imports; done
```

| Module | `.iopmod` | Library | Exports |
| --- | --- | --- | --- |
| `ROMDRV` | `ROM_file_driver` 1.03 | `romdrv` **2.01** | 6 |
| `SIFMAN` | `IOP_SIF_manager` 1.01 | `sifman` 1.01 | 36 |
| `SIFCMD` | `IOP_SIF_rpc_interface` 1.01 | `sifcmd` 1.01 | 32 |
| `SIFINIT` | *(no name)* 0.00 | *(none)* | — |

Two oddities in that table are real, not parsing errors. `ROMDRV`'s module
version (1.03) and its exported library version (2.01) differ — they are
independent fields, and only the library version participates in the
supersession arithmetic of `08`. And `SIFINIT` has **no module name, version
0.00, and exports nothing at all**.

## SIFINIT: a module that is never resident

`SIFINIT` is `0x110` bytes of text and its whole body fits in one listing:

```sh
python3 tools/irxinfo.py <outdir>/SIFINIT --dump-load <outdir>/SIFINIT.text
python3 tools/romdis.py <outdir>/SIFINIT.text --cpu iop --vma 0 --range 0x0 0x98
```

Its import tables put `loadcore` 12 at `0xB4`, `stdio` 4 at `0xD8` and
`sifman` 5 at `0xFC`, which names every call it makes:

```
   8:  jal   0xb4              # loadcore 12, key = 3   (the boot record)
   c:  addiu $a0, $zero, 3
  18:  lw    $v0, 4($v0)
  20:  andi  $v0, $v0, 1       # record flag bit 0
  2c:  addiu $a0, $a0, 0x110   # " Skip SIF init"
  34:  jal   0xd8              # stdio 4
  40:  addiu $v0, $zero, 1     # return 1
  44:  jal   0xb4              # loadcore 12, key = 1
  54:  lhu   $v1, 0($v0)
  5c:  bne   $v1, 1, 0x7c
  68:  addiu $a0, $a0, 0x120   # " Skip SIF init (it is DECI1)"
  78:  addiu $v0, $zero, 1     # return 1
  7c:  jal   0xfc              # sifman 5 -- the actual SIF init
  84:  addiu $v0, $zero, 1     # return 1
```

**Every path returns 1.** `SIFINIT` is designed never to stay resident: it
consults the boot record, either skips the SIF initialisation (printing why) or
performs it through `sifman`, and is then freed by `MODLOAD`. That is why it has
no name and no exports — it is not a library, it is a one-shot action packaged
as a module, and the `return & 3` rule is the mechanism that makes such a
packaging possible.

It also confirms the boot record at absolute `0x3F0` (`09`) as a shared
configuration channel rather than an `EECONF` peculiarity: `EECONF`, `SIFINIT`
and `SIFCMD` all look it up through `loadcore` 12 and test the same low flag
bits, with `SIFINIT` additionally querying key 1.

## SIFMAN: residency as a hardware gate

`SIFMAN`'s entry uses the residency rule as a decision, not a failure path:

```sh
python3 tools/irxinfo.py <outdir>/SIFMAN --dump-load <outdir>/SIFMAN.text
python3 tools/romdis.py <outdir>/SIFMAN.text --cpu iop --vma 0 --range 0x0 0x88
```

```
  14:  bnez  ...  -> 0x7c      # PRId < 0x10          -> return 1
  18:  addiu $v0, $zero, 1
  30:  bnez  ...  -> 0x7c      # 0xBF801450 & 8       -> return 1
  38:  lui   $a0, 0xbd00
  3c:  ori   $a0, $a0, 0x60    # SIF register 0xBD000060
  44:  lw    $v0, 0($a0)
  4c:  beq   $v0, 0x1d000060, 0x68     # exact match -> accept
  54:  lw    $v0, 0($a0)
  5c:  and   $v0, $v0, 0xfffff000
  60:  bnez  ...  -> 0x7c      # otherwise            -> return 1
  68:  addiu $a0, $a0, 0xda0   # its export table
  70:  jal   0xe64             # loadcore 6
  78:  sltu  $v0, $zero, $v0   # 0 on success, 1 otherwise
  7c:  ... return $v0
```

Three things worth keeping:

- The discriminator `PRId < 0x10 || (*(u32*)0xBF801450 & 8)` appears a **third**
  way here. `02` used it to pick a bus table and `06` to select between `P`/`I`
  variants; here it gates residency outright — on that configuration `SIFMAN`
  simply does not stay.
- A genuine **hardware presence check**: the SIF register at `0xBD000060` must
  read back exactly `0x1D000060`, or else have its top twenty bits clear. `SIF`
  registers live at `0xBD00xxxx` (offsets `0x0`, `0x8`, `0x10`, `0x20`, `0x30`,
  `0x40`, `0x60` are the ones this module touches).
- `sltu $v0, $zero, $v0` converts the registration result — 0 on success — into
  exactly the residency code the loader wants. The convention is being used as a
  calling convention, not stumbled into.

`SIFCMD` layers RPC on top: 32 exports, imports eight `sifman` ordinals plus
thread and event services, and carries a `0x12A0` `.bss` — by far the largest
uninitialised area of any module so far, which is where its command and RPC
queues live. Its entry opens with the same `loadcore` 12 boot-record lookup.

## ROMDRV publishes the archive as files

`ROMDRV` registers with `ioman` rather than serving calls directly:

```sh
python3 tools/romdis.py <outdir>/ROMDRV.text --cpu iop --vma 0 --range 0x0 0x50
```

```
   c:  addiu $a0, $a0, 0x760   # its export table
  10:  jal   0x7b4             # loadcore 6
  20:  jal   0x74              # internal init
  2c:  addiu $a0, $a0, 0x86c
  30:  jal   0x838             # ioman 21
  3c:  addiu $a0, $a0, 0x8bc
  40:  jal   0x830             # ioman 20
  4c:  srl   $v0, $v0, 0x1f    # sign bit -> residency code
```

Two descriptor structures are handed to two different `ioman` entry points, and
the module's residency is again derived arithmetically — `srl` by 31 turns a
negative return into 1. Its description string is `ROM/Flash`.

## The self-locating scan, four times over

`ROMDRV` finds the archive the same way the boot ROM does. The marker constants
that `02` identified in the boot block — `0x45534552` (`"RESE"`) with `0x54`
(`"T"`) — appear verbatim in its scan:

```sh
python3 tools/romdis.py <outdir>/ROMDRV.text --cpu iop --vma 0 | grep -A2 0x4553
# 550: lui $t1, 0x4553 / ori $t1, $t1, 0x4552 / addiu $t3, $zero, 0x54
```

Counting the implementations found so far, the same scan is written out **four
times** in this ROM: in the boot block (`02`), in `IOPBOOT` (`03`), in `MODLOAD`
and in `ROMDRV`. No shared routine, no stored pointer — each finds the table by
scanning for a `RESET` entry.

```sh
for m in ROMDRV MODLOAD; do
  echo -n "$m: "; python3 tools/romdis.py <outdir>/$m.text --cpu iop --vma 0 | grep -c 0x4553
done
python3 tools/romdis.py <outdir>/IOPBOOT --cpu iop --vma 0xbfc4a000 | grep -c 0x4553
# 1 each
```

That redundancy is the strongest statement yet that the archive's
self-location is ABI: four independent consumers depend on `RESET` being the
first entry, on the 16-byte entry format, and on offsets being the running sum
of aligned sizes. A rebuild cannot alter any of it.

## What this pins for the rebuild

- A module may legitimately be nameless, versionless and export-free if its
  purpose is a one-shot action; `return & 3` makes that a supported shape.
- The boot record at `0x3F0` is consulted by at least `EECONF`, `SIFINIT` and
  `SIFCMD`, with keys 1 and 3 and flag bits 0 and 1 in use.
- `SIFMAN` gates its own residency on the machine discriminator and on the SIF
  register at `0xBD000060` reading `0x1D000060`.
- The self-locating ROMDIR scan is duplicated in four places and must keep
  working in all of them.

Next in boot order: `IGREETING`, `REBOOT`, `LOADFILE` and the CDVD stack
(`docs/project-state.md` §5).
