# Bus and DMA: SSBUSC and DMACMAN

`SSBUSC` and `DMACMAN` are boot-list entries six and seven
(`docs/analysis/03-iopboot-and-boot-list.md`), the first modules that exist to
drive hardware rather than to run the kernel itself. They are also the first
that let us put names to ordinals: what they import, and where they call it,
identifies functions in the libraries analysed earlier.

Module inspection now goes through `tools/irxinfo.py`, which reports the
`.iopmod` fields and the library tables and derives export extents from the
contiguous relocation run (`docs/analysis/05-sysmem-and-loadcore.md`):

```sh
python3 tools/romdir.py assets/SCPH-50000.bin --extract <outdir>   # outside the repo
python3 tools/irxinfo.py <outdir>/SSBUSC --imports
python3 tools/irxinfo.py <outdir>/DMACMAN --imports
```

| Module | Library | Exports | Imports |
| --- | --- | --- | --- |
| `SSBUSC` | `ssbusc` 1.01 | 18 | `loadcore` 6; `intrman` 17, 18 |
| `DMACMAN` | `dmacman` 1.02 | 36 | `sysmem` 14; `loadcore` 6; `intrman` 17, 18 |

## Naming ordinals from their use

An import stub's address is fixed by its table: stubs begin at table + `0x14`
and are 8 bytes each (`05` §"Import tables"). So a `jal` in the disassembly can
be matched to an ordinal without guesswork. For `SSBUSC`, whose `loadcore`
table is at `0x2D0` and `intrman` table at `0x2F4`, the stubs are `0x2E4`
(`loadcore` 6), `0x308` (`intrman` 17) and `0x310` (`intrman` 18) — and its
entry calls all three:

```sh
python3 tools/irxinfo.py <outdir>/SSBUSC --dump-load <outdir>/SSBUSC.text
python3 tools/romdis.py <outdir>/SSBUSC.text --cpu iop --vma 0 --range 0x0 0x40
```

```
   0:  addiu $sp, $sp, -0x20
   8:  jal   0x308            # intrman 17, with $a0 = &saved_state
   c:  addiu $a0, $sp, 0x10
  10:  lui   $a0, 0
  14:  addiu $a0, $a0, 0x260  # its own export table
  18:  jal   0x2e4            # loadcore 6
  ...
  28:  lw    $a0, 0x10($sp)   # the saved state
  2c:  jal   0x310            # intrman 18
```

Two identifications follow, both corroborated rather than assumed:

- **`loadcore` ordinal 6 is the export-library registration function.** `05`
  reached that by disassembling `LOADCORE` offset `0x898` (validates the
  `0x41C00000` magic, walks the registry, links the library in). Here the same
  ordinal is called with the module's own export table as its argument, and
  every module surveyed so far imports exactly this ordinal from `loadcore`.
- **`intrman` ordinals 17 and 18 are a critical-section pair.** 17 takes the
  address of a caller-provided word and 18 takes that word's value back; the
  registration happens between them. This also explains the aliasing `06`
  noticed — `intrman` slots 19 and 20 repeat the addresses of 17 and 18, i.e.
  two published names for one disable/restore pair.

`DMACMAN`'s entry (offset `0xB38`) uses the same three, in a different order —
it registers first and returns 1 if registration fails, then takes the critical
section for its hardware setup.

## SSBUSC: a register-table accessor

`SSBUSC` is tiny (text `0x320`, no `.bss`). Its `.data` is the interesting part:

```sh
python3 - <<'PY'
import struct
d = open('<outdir>/SSBUSC', 'rb').read()
shoff, = struct.unpack_from('<I', d, 32)
es, n, sx = struct.unpack_from('<HHH', d, 46)
stroff, = struct.unpack_from('<I', d, shoff + sx * es + 16)
for i in range(n):
    nm, typ, fl, addr, off, size, *_ = struct.unpack_from('<10I', d, shoff + i * es)
    if d[stroff + nm:].split(b'\0')[0] == b'.data':
        print([hex(struct.unpack_from('<I', d, off + 4 * k)[0]) for k in range(size // 4)])
PY
```

The first two words are the module-info pair the `.iopmod` header points at — a
name pointer and the version `0x0101` (`04` §"The `.iopmod` metadata"). The rest
is a sparse, index-addressed table of bus-controller registers in two banks,
`0xBF801000`–`0xBF80101C` and `0xBF801400`–`0xBF801420`, with `0` in the unused
slots. The 18 exports are accessors over that table, which is why the module has
almost no code: the holes are what make an out-of-range or unmapped index
distinguishable at run time.

The two banks matter for the rebuild — the second one (`0xBF8014xx`) has no
PS1 counterpart, so anything reasoning by analogy with the older machine will
miss half the table.

## DMACMAN: both DMA banks, unconditionally

`DMACMAN` is larger (text `0x15D0`) and its `.data` (`0xE0`) is again mostly a
register table: after the same two-word module-info pair come **51 hardware
register addresses**, ending in three zero words.

```sh
python3 - <<'PY'
import struct
d = open('<outdir>/DMACMAN', 'rb').read()
shoff, = struct.unpack_from('<I', d, 32)
es, n, sx = struct.unpack_from('<HHH', d, 46)
stroff, = struct.unpack_from('<I', d, shoff + sx * es + 16)
for i in range(n):
    nm, typ, fl, addr, off, size, *_ = struct.unpack_from('<10I', d, shoff + i * es)
    if d[stroff + nm:].split(b'\0')[0] == b'.data':
        ws = [struct.unpack_from('<I', d, off + 4 * k)[0] for k in range(size // 4)]
        regs = [w for w in ws if 0xBF800000 <= w < 0xBF802000]
        print(len(regs), hex(min(regs)), hex(max(regs)))
PY
# 51 0xbf801080 0xbf8015f0
```

The addresses span **both** IOP DMA banks — the familiar `0xBF8010xx` channel
block and a second block at `0xBF8015xx` — and include the control and interrupt
registers of each (`0xBF8010F0`/`0xBF8010F4`, `0xBF801570`/`0xBF801574`). Within
the first bank the addresses step by `0x10` per channel in the usual
MADR/BCR/CHCR pattern; the later part of the table is not uniformly strided, so
the exact per-channel grouping is left for the specification rather than guessed
here.

The structural point is the contrast with `06`: interrupts needed a `P`/`I`
variant pair because only one variant knows about the second bank, whereas
`DMACMAN` is a **single module that always covers both**. Whatever the `P`/`I`
discriminator selects, it is not simply "machine has the second bank".

After registering, `DMACMAN`'s entry writes `0x07777777` twice and `0x777` once
through helpers at `0x9E8`, `0xA18` and `0xA48` — the repeating-nibble priority
fields of the DMA control registers — then loops over channel indices calling
its own offset `0x0` with `(channel, 0)`, resetting each channel in turn.

```sh
python3 tools/irxinfo.py <outdir>/DMACMAN --dump-load <outdir>/DMACMAN.text
python3 tools/romdis.py <outdir>/DMACMAN.text --cpu iop --vma 0 --range 0xb38 0xb90
```

## What this pins for the rebuild

- Both modules register through `loadcore` ordinal 6 and bracket their hardware
  setup with `intrman` 17/18; the ordinal assignments are load-bearing, since
  every later module binds by number.
- `SSBUSC` must expose an index-addressed table covering both bus-controller
  banks, holes included.
- `DMACMAN` must cover both DMA banks in one module, and its init must set the
  priority fields and reset every channel before anything uses DMA.

Next in boot order: `SYSCLIB` and `HEAPLIB` — the C library and heap the later
modules are written against (`docs/project-state.md` §5).
