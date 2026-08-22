# The Kernel's TLB

The M1 program's `SifInitRpc` returned on PS2e and not on PCSX2 with the same
image, and PCSX2's log said why: `TLB Miss, pc=0x10ff10 addr=0x20124450`,
fifty times over. The SDK's SIF client keeps its packet buffer and its RPC
tables behind the **uncached window** — `0x20000000` plus the physical address
— and reads the packet that completes `SifInitRpc` through it. PCSX2 walks the
TLB for KUSEG and answers an unmapped read with zero, so the client saw no
packet; PS2e folds an unmapped address onto memory with a warning, so it did.
Our kernel had mapped the scratchpad (`spec/04` EE-1a) and nothing else. This
reads what the reference maps.

```sh
python3 tools/eesim.py assets/SCPH-50000.bin --tlb
python3 tools/eesim.py assets/SCPH-70000.bin --tlb
```

`docs/analysis/22` already had the kernel announcing `TLB spad=0 kernel=1:12
default=13:30 extended=31:38` on its console; `--tlb` prints every `tlbwi`
the boot makes, with its index and the four registers, in order. Both images
write the same 88 entries, and the output is byte-identical between them.

## 1. The sequence

1. The reset path writes index 0: the scratchpad (EE-1a).
2. The kernel writes all 48 entries **invalid**: `EntryHi = 0xE0000000 +
   index × 0x2000`, `PageMask` and both `EntryLo` zero. Index 0 is among them,
   so the scratchpad mapping is gone until step 3 restores it.
3. It writes indices 0–38 from a table, with `tlbwi`, and leaves `Wired` at
   `0x1F` — so `Random`, which SYS-7b's slot `0x09` takes its index from, runs
   over 31–47 and never touches the fixed entries. (The console's
   `extended=31:38` names the last eight table entries, which sit *above*
   `Wired` and are therefore replaceable by a program's own writes.)

## 2. The table

`EntryLo` values end in `C << 3 | D << 2 | V << 1 | G`: `0x1F` is cached
(`C = 3`), `0x17` uncached (`C = 2`), `0x3F` uncached accelerated (`C = 7`),
and `0x13` uncached with **`D` clear** — read-only.

| Index | `PageMask` | `EntryHi` | `EntryLo0` | `EntryLo1` | What it is |
| --- | --- | --- | --- | --- | --- |
| 0 | `0` | `0x70000000` | `0x80000007` | `0x00000007` | scratchpad, 4 KiB, `S` bit |
| 1 | `0x6000` | `0xFFFF8000` | `0x1E1F` | `0x1F1F` | physical `0x78000`, two 16 KiB pages, cached |
| 2–9 | `0` | `0x10000000` + 8 KiB steps | `0x00400017` + | | hardware registers `0x10000000`–`0x1000FFFF`, 4 KiB pages, uncached |
| 2 | `0` | `0x10000000` | `0x00400017` | `0x00400053` | `0x10001000`: **`EntryLo1` has `D` clear** |
| 8 | `0` | `0x1000C000` | `0x00400313` | `0x00400357` | the page with the SIF channels' registers: **`EntryLo0` has `D` clear** |
| 10 | `0x1E000` | `0x11000000` | `0x00440017` | `0x00440415` | VU memory, 64 KiB pages; the odd page has `V` clear |
| 11 | `0x1E000` | `0x12000000` | `0x00480017` | `0x00480415` | GS privileged registers, likewise |
| 12 | `0x1FFE000` | `0x1E000000` | `0x00780017` | `0x007C0017` | the ROM, two 16 MiB pages, uncached |
| 13–15 | `0x7E000` | `0x00080000`, `0x00100000`, `0x00180000` | physical `= virtual`, `0x1F` | | RAM cached, 256 KiB pages |
| 16–18 | `0x1FE000` | `0x00200000`, `0x00400000`, `0x00600000` | | | 1 MiB pages |
| 19–21 | `0x7FE000` | `0x00800000`, `0x01000000`, `0x01800000` | | | 4 MiB pages |
| 22–30 | as 13–21 | `0x20080000` … `0x21800000` | physical as 13–21, `0x17` | | the same memory uncached |
| 31–38 | as 14–21 | `0x30100000` … `0x31800000` | physical as 14–21, `0x3F` | | the same memory uncached accelerated; no entry for `0x30080000` |

Three things in it a rebuild would not have guessed:

- **The first 512 KiB of KUSEG is not mapped.** Entries 13 and 22 start at
  `0x80000`. A program lives above that (the SDK links at `0x100000`) and the
  kernel reaches low memory through KSEG0, so nothing needs `0x00000000`
  through `0x0007FFFF` as a KUSEG address — and a program that touched it
  would take a TLB refill. PCSX2 nevertheless serves that range without a
  mapping, which is why our image ran programs at all before this table.
- **Two hardware pages are read-only.** `0x1000C000`, where the SIF
  channels' registers are — channel 5 at `0x1000C000`, channel 6 at
  `0x1000C400` — and `0x10001000`. A program reads `D5_CHCR` itself (the SDK
  does, before deciding to call slot `0x78`) and writes it only through the
  slots of SYS-13. The odd 64 KiB pages of entries 10 and 11 are not valid
  at all: VU memory and the GS registers each get one page.
- **`Wired` is the protection SYS-7b asked about.** `spec/05` left open
  whether slot `0x09` could overwrite the scratchpad mapping; `Wired = 31`
  settles it from the hardware side, not from the index arithmetic.

## 3. What the image does with it

`src/kernel/tlb.cpp` carries the table and performs the sequence of §1, called
from the kernel entry before the handshake with the IOP; `spec/04` EE-12
specifies it. With it in place PCSX2's log has no `TLB Miss` and the M1
program's `SifInitRpc` returns there as it does on PS2e.

## 4. Not read here

- The kernel's own use of entry 1 (`0xFFFF8000`, physical `0x78000`): what it
  keeps in those 32 KiB, and which of its routines run under the alias.
- Whether `0x09`'s write also adjusts `Wired`, or only trusts `Random`.
