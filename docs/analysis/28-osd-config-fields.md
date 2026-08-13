# What the OSD's configuration record actually holds

`docs/analysis/26-cdvd-nvm-and-config.md` established the transport and its
checksum; `docs/analysis/27-osdsys-payload-and-config.md` expanded the payload
and found the record's shape — two 15-byte blocks, block 0 passed through
whole, block 1's bytes decoded into bit fields by a routine at `0x203698`. This
document names the fields.

Everything below reads the expanded image at `0x200000`, produced as `27`
describes and kept outside the repository.

## Measuring the decoder instead of reading it

The decoder is a long stretch of shift-and-mask. Reading it is possible and
error-prone; running it is neither. `27` left `tools/eesim.py` able to call one
routine in a loaded image, so the whole mapping falls out of a sweep: set one
bit of the input, call the decoder, see which bits of the output moved.

```sh
python3 - <<'PY'
import sys; sys.path.insert(0, "tools")
from eesim import Bus, callProgram

img = open("<outdir>/OSDSYS.expanded", "rb").read()
IN, OUT, N = 0x1000000, 0x1010000, 64
bus = Bus(b"")
buf, start = bus.region(0x200000)
buf[start:start + len(img)] = img
ram, _ = bus.region(0)

def decode(block):
    ram[IN:IN + 30] = bytes(block)
    ram[OUT:OUT + N] = bytes(N)
    cpu, result = callProgram(bus, 0x203698, (OUT, IN), steps=200000)
    assert result is not None
    return int.from_bytes(ram[OUT:OUT + N], "little")

seed = bytearray(30); seed[15] = 0xE0            # see "the gate", below
base = decode(seed)
for byte in range(15, 30):
    for bit in range(8):
        t = bytearray(seed); t[byte] ^= 1 << bit
        moved = decode(t) ^ base
        if moved:
            print(f"blk1 +{byte - 15:2d} bit {bit} -> "
                  f"{[i for i in range(N * 8) if moved >> i & 1]}")
PY
```

Only block 1 moves anything; every bit of block 0 leaves the output untouched,
which is `27`'s "passed through whole" confirmed from the other direction.

| Block 1 byte | bits | Struct bits |
| --- | --- | --- |
| +0 | 0 | 0 |
| +0 | 1–2 | 1–2 |
| +0 | 3 | 3 |
| +1 | 0–4 | 4–8 |
| +2 | 0–2 | 17–19 |
| +2 | 3 | 29 |
| +2 | 4 | 30 |
| +2 | 5–6 | 32–33 |
| +3 | 0–7 | 9–16 |
| +4 | 0 | 28 |
| +4 | 7, 6, 5, 4 | 34, 35, 36, 37 |
| +5 | 0–7 | 20–27 |

Two things are visible immediately. The byte `+4` row is **bit-reversed** — its
top nibble lands back to front. And the fields are not byte-aligned in the
struct: `+3` and `+2`'s low bits are adjacent there, as are `+5` and `+4` bit 0.

## The accessors agree, independently

`OSDSYS` carries a bank of one-field getter/setter pairs over the same struct,
at `0x203E70` onwards, each a `lw`/shift/mask of the word at `0x2BA800`:

```sh
python3 tools/romdis.py <outdir>/OSDSYS.expanded --cpu ee --vma 0x00200000 \
        --range 0x203e70 0x2042e0
```

| Struct bits | width | getter |
| --- | --- | --- |
| 0 | 1 | `0x203E70` |
| 1–2 | 2 | `0x203EA4` |
| 3 | 1 | `0x203F10` |
| 4–8 | 5 | `0x204060` |
| 9–19 | 11 | `0x2041A0` |
| 20–28 | 9 | `0x2041E8` |
| 29 | 1 | `0x20427C` |
| 30 | 1 | `0x2042C0` |

Those widths were derived from the code; the table above was derived from
execution. They agree exactly, including the two that straddle input bytes: the
11-bit field is `+3` in its low eight bits and `+2`'s low three above them, and
the 9-bit field is `+5` plus `+4` bit 0. Neither method was told about the
other.

## The 5-bit field is the language, and it is gated

The 5-bit field at struct bits 4–8 has one consumer, at `0x204B38`:

```sh
python3 tools/romdis.py <outdir>/OSDSYS.expanded --cpu ee --vma 0x00200000 \
        --range 0x204b38 0x204b78
```

```
204b48  jal   0x203f50            # read the field
204b50  lui   $3, 0x27
204b54  sll   $4, $2, 0x2         # index it, four bytes per entry
204b58  addiu $3, $3, 0x69e8      # a table at 0x2769E8
204b68  lw    $3, 0x0($4)
204b70  sw    $3, 0x6a08($5)      # store the chosen entry at 0x276A08
```

The table's entries are string tables, and following one string through each of
them settles what the index means:

| Index | first differing string | Language |
| --- | --- | --- |
| 0 | `\x83g\x83\x89\x83b\x83N %d` (Shift-JIS) | Japanese |
| 1 | `Track %d` | English |
| 2 | `Plage %d` | French |
| 3 | `Pista %d` | Spanish |
| 4 | `Titel %d` | German |
| 5 | `Brano %d` | Italian |
| 6 | `Track %d` | Dutch |
| 7 | `Faixa %d` | Portuguese |

**The table has eight entries.** The word that looks like a ninth, at
`0x276A08`, is the destination of the `sw` above — the *currently selected*
table, which is why it reads back as a copy of one of them.

### The gate, and why it is a version marker

The decoder does not take the 5-bit field unconditionally:

```
2036ec  lbu   $3, 0xf($12)        # block 1 byte +0
2036f0  srl   $2, $3, 0x5         # its top three bits
2036f4  beqzl $2, 0x20371c        # zero -> the other branch
2036fc  lbu   $2, 0x10($12)       # non-zero -> take +1
203708  andi  $2, $2, 0x1f        #             five bits of it
...
20371c  andi  $3, $3, 0x10        # zero -> one bit of +0 instead
```

So the record has two generations in one layout. **When the top three bits of
block 1 byte +0 are zero, byte +1 is not read at all** and the language comes
from a single bit of byte +0 — struct bit 4, which selects index 0 or 1,
Japanese or English. When they are non-zero, the full five-bit index in byte +1
is used.

That gate is what a hand-made block gets wrong in both directions at once. Left
zero, a carefully chosen language byte is ignored. Set, the five-bit field is
believed — and **the lookup above has no bounds check**, so an index of 8 or
more reads past the eight-entry table. Index 9 lands on a word that is zero, so
the OSD proceeds with a null string table: it does not crash, it stops having
anything to draw.

## The 11-bit field is a timezone in minutes

Its consumers do arithmetic that leaves little open:

```sh
python3 tools/romdis.py <outdir>/OSDSYS.expanded --cpu ee --vma 0x00200000 \
        --range 0x20c2c0 0x20c340
```

```
20c2c4  addiu  $17, $zero, 0x3c   # 60
20c2d0  mult   $2, $2, $17        # x 60
20c2d4  jal    0x2041a0           # the 11-bit field
20c2d8  dsubu  $16, $16, $2       # (delay slot: the previous result)
20c2dc  mult   $2, $2, $17        # x 60 again, on this one
20c2e0  jal    0x204280           # the bit-29 field
20c2e4  daddu  $16, $2, $16
20c2e8  daddiu $3, $16, 0xe10     # 3600
20c2f0  movz   $3, $16, $2        # keep it or not, on that bit
```

`romdis` shows the two multiplies and the conditional move as `<unknown>`:
LLVM has no R5900 target, so its three-operand `mult` (raw `0x00511018`, funct
`0x18` with a destination register) and `movz`/`movn` (funct `0x0A`/`0x0B`)
have to be decoded from the raw words, as `22` warned.

Multiplying by 60 turns minutes into seconds, and 11 bits covers a whole day of
them. **Struct bits 9–19 are a timezone offset in minutes**, and **bit 29
selects a one-hour shift** on top of it through a `movz`/`movn` pair — the shape
of a daylight-saving flag, named here by what it does rather than by a string.

The 9-bit field at bits 20–28 is read alongside the timezone at `0x208E6C` and
passed with it into `0x206DD0`, so it belongs to the same date-and-time group;
which member of it is not settled here.

## Bit 30 is the 12- or 24-hour clock

Its consumer at `0x20C188` switches on the bit and picks a different format
string and a different arithmetic path:

| Bit 30 | Format at | Arithmetic |
| --- | --- | --- |
| 0 | `0x2B56C8` — `%2d:%02d:%02d` | none |
| 1 | `0x2B56E0` | `div $zero, $17, $2` with `$2` = **12** |

Dividing the hour by twelve and selecting a table that carries `A`/`M` markers
settles it without needing a string that says so.

## Bit 3 goes to the kernel

Bit 3 has the narrowest consumer of all. `0x206B50` reads it, returns early if
it matches a cached copy at `0x276FC0`, and otherwise runs one of two branches
that differ in exactly one instruction — the argument passed to `0x254850`:

```
206bd0  jal   0x254850
206bd4  addiu $4, $zero, 0x1     # the other branch passes $zero
```

and `0x254850` is a syscall stub:

```
254850  addiu $3, $zero, 0x4f
254854  syscall
```

So **the OSD hands this configuration bit to EE syscall `0x4F`**, whose
signature `spec/05` already records as `($a0) -> $v0` and which
`docs/analysis/19-ee-config-syscalls.md` places in the initialisation and
configuration band. The two documents meet here: `19` had the slot's shape from
the kernel side, and this is a real caller of it from the other. Both branches
then repeat the same reconfiguration sequence, so the bit selects a mode the
kernel holds rather than something the OSD draws.

## What this pins for the rebuild

- Only block 1 feeds the decoder. Block 0's fifteen bytes reach the caller
  untouched and nothing in this path inspects them.
- The struct layout is the table above, confirmed twice over by two independent
  methods — a bit sweep through the running decoder, and the widths of the
  accessor bank.
- **Struct bits 4–8 are the OSD language**, index 0–7 in the order Japanese,
  English, French, Spanish, German, Italian, Dutch, Portuguese.
- **The language is gated on the top three bits of block 1 byte +0.** Zero means
  an older record whose language is one bit — Japanese or English — held
  elsewhere. This is the field a synthesised block must get right before any
  other, and getting it wrong is silent.
- **The lookup is unchecked.** An index of 8 or more reads past an eight-entry
  table; 9 yields a null table, and an OSD with a null string table draws
  nothing rather than failing.
- **Struct bits 9–19 are a timezone offset in minutes**, multiplied by 60 where
  used, with **bit 29** selecting a further hour.
- **Bit 30 chooses the 24- or 12-hour clock**, the latter dividing the hour by
  twelve.
- **Bit 3 is passed to EE syscall `0x4F`**, which gives `spec/05`'s recorded
  signature for that slot a caller and its band a purpose.
- Fields still unnamed: bit 0, bits 1–2, bits 20–28, and the second word's
  bits 0–1 and its bit-reversed 2–5.
