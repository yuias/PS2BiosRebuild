# EE Syscalls: Initialisation and Configuration

This finishes the group survey of the EE syscall table begun in
`docs/analysis/15-ee-syscall-groups.md`. The remaining slots do not form a
contiguous run — they sit past `0x8000BD58`, at the far end of the image — but
they share a subject: bringing hardware up, identifying it, and reading and
writing the kernel's own configuration.

```sh
python3 tools/romdir.py assets/SCPH-50000.bin --extract <outdir>   # outside the repo
python3 tools/romdis.py <outdir>/KERNEL --cpu ee --vma 0x80000000 --range 0x8000d9b8 0x8000d9e0
```

| Slot | Handler | Subject |
| --- | --- | --- |
| `0x01` | `0x8000D9B8` | hardware initialisation, driven by a bitmask |
| `0x02` | `0x8000BD58` | three signed 16-bit parameters |
| `0x3B`, `0x4D`, `0x4E`, `0x4F` | `0x8000CF58`–`0x8000D260` | kernel data structures |
| `0x4A`, `0x4B` | `0x8000D470`, `0x8000D3C0` | a configuration word, set and get |
| `0x4C` | `0x8000CD18` | GS identification |
| `0x6E`, `0x6F` | `0x8000D350`, `0x8000D2B8` | kernel data structures |

## Slot 0x01: the initialisation list is a syscall

`docs/spec/04-ee-kernel.md` EE-10 lists the hardware the kernel brings up,
derived from its `# Initialize ...` diagnostics. Those messages belong to a
**callable syscall**, not to a fixed boot sequence:

```
8000d9b8  addiu $sp, $sp, -0x30
8000d9c0  move  $s0, $a0           # the argument is kept for the whole body
8000d9c8  andi  $v0, $s0, 0x1      # test bit 0
8000d9cc  beqz  $v0, ...           # skip this subsystem if clear
8000d9d4  lui   $a0, 0x8001
8000d9d8  jal   0x800073e0         # the print routine
8000d9dc  addiu $a0, $a0, 0x6148   # "# Initialize DMAC ...\n"
```

`$a0` is a **bitmask selecting which subsystems to initialise**, tested bit by
bit down the body:

```sh
python3 tools/romdis.py <outdir>/KERNEL --cpu ee --vma 0x80000000 \
    --range 0x8000d9b8 0x8000dab8 | grep 'andi.*\$16'
# bits 0x1, 0x2, 0x8, 0x10, 0x40 ... each tested twice
python3 -c "
d = open('<outdir>/KERNEL','rb').read(); print(d[0x16148:0x16160].split(b'\0')[0])"
# b'# Initialize DMAC ...\n'
```

Each bit is tested twice — once to print the announcement and once to guard the
work itself. So a caller can bring up any subset of DMAC, VU0/VU1, VIF0/VIF1,
GIF, GS, IPU, INTC, TIMER, FPU, user memory and the scratchpad, and the
diagnostics are not incidental logging: they are emitted per selected bit.

For the rebuild this converts EE-10 from "the kernel initialises this list" into
a **parameterised interface**: the bit assignment is part of the contract, and a
caller passing a mask must get exactly those subsystems and no others.

## Slot 0x4C: GS identification

```
8000cd18  lui   $v1, 0x1200
8000cd1c  ori   $v1, $v1, 0x1000   # 0x12001000, a GS privileged register
8000cd20  ld    $v0, 0x0($v1)      # read it as a doubleword
8000cd24  dsrl  $v0, $v0, 0x10     # bits 16..23
8000cd28  andi  $v0, $v0, 0xff
8000cd2c  sltiu $v0, $v0, 0x19     # compare against 0x19
8000cd30  bnez  $v0, ...           # older revisions take a different path
```

The GS revision is read from the privileged register at `0x12001000`, extracted
from bits 16–23, and compared against `0x19`. Two things follow. The register
must be read with `ld` — a 64-bit access, not two words — because GS privileged
registers are doubleword-wide. And the kernel **branches on the revision**, so a
rebuild that hard-codes one behaviour will be wrong on whichever machines take
the other branch.

## Slots 0x4A and 0x4B: a configuration word, both ways

The two are near-mirror images over a kernel word at `0x80022590`:

```
             slot 0x4A (0x8000D470)          slot 0x4B (0x8000D3C0)
  lw   $v0, 0($a0)                    lw   $v1, 0($a0)
  lw   $v1, 0($a1=0x80022590)         lw   $v0, 0($a1=0x80022590)
  andi $v0, $v0, 0x1                  and  $v1, $v1, ~0x1
  and  $v1, $v1, ~0x1                 andi $v0, $v0, 0x1
  or   $v1, $v1, $v0                  or   $v1, $v1, $v0
```

Both merge a caller-supplied structure with the stored word, but in opposite
directions — one takes the caller's bit and keeps the kernel's remainder, the
other the reverse. They are the **set and get** of one packed configuration
word, and the run of masks that follows (`~0x1`, `~0x6`, `~0x8`, `~0x10`)
shows it holds several independent fields.

A rebuild must keep the two directions distinct. They are adjacent slot numbers
with near-identical code, which makes them exactly the kind of pair that gets
merged by accident — and merging them would make one direction silently
overwrite the other's fields.

## Slot 0x02: the argument convention is the requirement

```
8000bd5c  sll   $a0, $a0, 0x10
8000bd60  sll   $a1, $a1, 0x10
8000bd64  sll   $a2, $a2, 0x10
8000bd6c  sra   $a0, $a0, 0x10      # sign-extend from 16 bits
8000bd74  sra   $a1, $a1, 0x10
8000bd7c  sra   $t1, $a2, 0x10
```

Three arguments are **sign-extended from 16 bits** before use. What the call
then does is not established here, but the convention is itself a requirement:
callers pass small signed values and the handler must narrow them, so a rebuild
taking the registers at full width would accept out-of-range arguments the
reference rejects by truncation.

## The survey is complete

With this group, every band of the syscall table has been characterised:

| Group | Slots | Document |
| --- | --- | --- |
| exceptions and interrupts | `0x0D`–`0x1F` | `16` |
| scheduler | `0x07`, `0x21`–`0x44` | `17` |
| cache and CP0 | `0x60`–`0x6A` | `18` |
| initialisation and configuration | `0x01`, `0x02`, `0x3B`, `0x4A`–`0x4F`, `0x6E`, `0x6F` | this document |
| program loading and the OSD | `0x06`, `0x7B` | `15` |
| undefined / retired | thirteen slots | `14` |

What remains is the *behaviour* of individual slots within each group — the
arguments and return values one by one — rather than any unexplored region.

## What this pins for the rebuild

- Slot `0x01` takes a **bitmask** selecting subsystems to initialise; the bit
  assignment is part of the contract and each selected bit both announces and
  acts.
- Slot `0x4C` reads the GS revision from `0x12001000` with a **64-bit** load,
  from bits 16–23, and branches on whether it is below `0x19`.
- Slots `0x4A`/`0x4B` are the set and get of one packed configuration word at
  `0x80022590` and must not be merged.
- Slot `0x02` sign-extends three arguments from 16 bits.

Next: `OSDSYS`, the last unexamined component (`docs/project-state.md` §5).
