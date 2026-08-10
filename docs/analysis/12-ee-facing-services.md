# EE-Facing Services, and the Shape of the Boot List

This document covers the remaining entries of `IOPBTCONF` — `IGREETING`,
`REBOOT`, `LOADFILE`, `CDVDMAN`, `CDVDFSV`, `FILEIO`, `SECRMAN` and `EESYNC` —
and with them the IOP boot list is completely surveyed
(`docs/analysis/03-iopboot-and-boot-list.md`).

```sh
python3 tools/romdir.py assets/SCPH-50000.bin --extract <outdir>   # outside the repo
for m in IGREETING REBOOT LOADFILE CDVDMAN CDVDFSV FILEIO SECRMAN EESYNC; do
  python3 tools/irxinfo.py <outdir>/$m
done
```

| Position | Module | `.iopmod` name | Library | Exports |
| --- | --- | --- | --- | --- |
| 20 | `IGREETING` | *(none)* 0.00 | *(none)* | — |
| 22 | `REBOOT` | `RebootByEE` 1.01 | *(none)* | — |
| 23 | `LOADFILE` | `LoadModuleByEE` 1.01 | *(none)* | — |
| 24 | `CDVDMAN` | `cdvd_driver` 1.04 | `cdvdman` 1.01 | 62 |
| 25 | `CDVDFSV` | `cdvd_ee_driver` 1.04 | `cdvdfsv` 1.01 | 10 |
| 27 | `FILEIO` | `FILEIO_service` 1.01 | *(none)* | — |
| 28 | `SECRMAN` | `secrman_for_cex` 1.03 | `secrman` 1.03 | 14 |
| 29 | `EESYNC` | `SyncEE` 1.01 | `eesync` 1.01 | 6 |

## The boot list has three phases

Read in order, the twenty-nine entries fall into three groups, and the module
names say so outright:

1. **Kernel core** (1–13, `SYSMEM` … `THREADMAN`): memory, module linking,
   exceptions, interrupts, bus and DMA, timers, the C library, threads.
2. **OS services** (14–18, `VBLANK` … `STDIO`): the facilities the kernel offers
   to modules — vblank, file manager, module loader, ROM driver, stdio.
3. **EE-facing services** (19–29, `SIFMAN` … `EESYNC`): the SIF itself, then
   services that exist so the **EE** can drive the IOP across it.

Phase three is unambiguous from the `.iopmod` names alone: `RebootByEE`,
`LoadModuleByEE`, `cdvd_ee_driver`, `FILEIO_service`, `SyncEE`. Their imports
agree — every one of them pulls in `sifcmd` and/or `sifman`, which no phase-one
module does. The IOP's boot is not "start a kernel" so much as "start a kernel,
then stand up an RPC surface for the other processor".

That shape matters for the rebuild's ordering: phase three cannot move earlier
(it needs `sifman`, which needs the SIF to have been probed), and the phases are
what the alternative configurations of `10` reshuffle — `OSDCNF` extends phase
three, never phase one.

## Modules that exist only to act

`IGREETING` is the second nameless, version-`0.00`, export-free module after
`SIFINIT` (`11` §"SIFINIT: a module that is never resident"), and it has the
same shape: it consults the boot record and prints.

```sh
python3 tools/irxinfo.py <outdir>/IGREETING --imports
# loadcore 12; stdio 4; intrman; ioman; sysmem; sysclib
python3 tools/irxinfo.py <outdir>/IGREETING --dump-load <outdir>/IGREETING.text
python3 tools/romdis.py <outdir>/IGREETING.text --cpu iop --vma 0 --range 0x0 0x40
```

```
   4:  addiu $a0, $zero, 4     # boot record, key 4
  10:  jal   0x924             # loadcore 12
  28:  beqz  $s0, 0xd0
  2c:  addiu $v0, $zero, 1     # nothing found -> return 1
  30:  lhu   $v1, 0($s0)       # branch on the record's value
```

Its strings say what it prints — the boot banner and a diagnostic line:

```sh
strings -a <outdir>/IGREETING | grep -E '[A-Za-z]{4}'
# PlayStation 2 ======== / Hard reset boot / Soft reboot
# Update rebooting.. / Update reboot complete
# , IOP info (CPUID= / ROMGEN= / , CACH_CONFIG=
```

so the record at key 4 distinguishes a hard reset from a soft or update reboot.
The banner text is original branding and is out of bounds for reproduction
(`docs/clean-room-policy.md` §3); what a rebuild must keep is the *behaviour* —
a module that reports boot kind and CPU identification through `stdio` and does
not stay resident.

Counting the boot record's users so far: `EECONF` with key 3 (`09`), `SIFINIT`
with keys 3 and 1, `SIFCMD` with key 3 (`11`), and `IGREETING` with key 4. It is
the kernel's general configuration channel, not a special case anywhere.

## Services without libraries

`REBOOT`, `LOADFILE` and `FILEIO` export **no library at all** — they have
import tables but no export table:

```sh
python3 tools/irxinfo.py <outdir>/REBOOT | grep -c export || true
```

They are resident services rather than callable libraries: they register RPC
handlers with `sifcmd` and then serve the EE. Nothing on the IOP calls them by
ordinal, so there is nothing to export. `REBOOT` additionally imports `modload`,
which matches the reboot strings `10` found in `MODLOAD` — the EE asks, `REBOOT`
relays, `MODLOAD` tears down and reboots the IOP against a named archive.

This completes the taxonomy of module shapes seen in this ROM:

| Shape | Exports | Resident | Example |
| --- | --- | --- | --- |
| Library | yes | yes | `SYSMEM`, `THREADMAN` |
| Multi-library | several | yes | `SYSCLIB` (2), `THREADMAN` (7) |
| Resident service | none | yes | `REBOOT`, `LOADFILE`, `FILEIO` |
| One-shot action | none | **no** | `SIFINIT`, `IGREETING` |

Only the last is distinguished by the `return & 3` rule (`10`); the middle two
differ merely in whether anything on the IOP needs to call them.

## CDVD and SECRMAN

`CDVDMAN` (`cdvd_driver`, 62 exports) is the largest `.bss` in the ROM by a wide
margin — `0x16430`, about 90 KB of buffers — against `0x4D30` of text. `CDVDFSV`
(`cdvd_ee_driver`, 10 exports) is its EE-facing half, importing `sifcmd` and
`sifman` where `CDVDMAN` imports none of either. The split is deliberate: the
driver talks to hardware, and a separate module exposes it over the SIF.

`SECRMAN` (`secrman_for_cex` 1.03, 14 exports) is the disc-authentication
module; `cex` marks the consumer build. It imports `cdvdman`, `ioman` and
`modload`. Per `docs/clean-room-policy.md` §"Trademarks and compatibility",
protection mechanisms are in scope only as far as an emulator's boot path needs
their *interfaces* to exist, so this document records its shape — a 14-entry
library sitting between the CDVD driver and the loader — and stops there.

`EESYNC` (`SyncEE`, 6 exports, `0x140` of text) is the last entry in the list
and the smallest module that still exports something: it imports `sifman` 24 and
`loadcore` 6, 12, 20. Being last is its function — it is what tells the EE the
IOP has finished coming up.

## What this pins for the rebuild

- The boot list's three-phase shape is load-bearing: EE-facing services depend
  on the SIF having been probed and registered, and alternative configurations
  extend phase three only.
- Four module shapes must all be expressible, including export-free residents
  and one-shot actions distinguished solely by their return value.
- The boot record is a general configuration channel with at least keys 1, 3
  and 4 in use across four modules.
- `CDVDMAN`'s `0x16430` `.bss` is the largest single memory demand of the boot
  list and constrains where the IOP's heap boundary can sit.

The IOP boot list is now fully surveyed. Next: the EE side — the boot block's EE
path (`02` §"EE reset path"), `EELOAD`, and the `KERNEL` image that is
byte-identical across both reference ROMs (`docs/project-state.md` §5).
