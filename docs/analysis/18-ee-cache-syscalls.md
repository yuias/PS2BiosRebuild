# EE Syscalls: Cache and CP0 Control

`docs/spec/04-ee-kernel.md` EE-8e records that slots `0x60`, `0x61` and `0x62`
are published through KSEG1 while the other 122 use KSEG0, and calls the choice
"part of each slot's contract". Reading them shows it is more than a
convention: it is forced by what they do.

This covers the whole `0x60`–`0x6A` band, which turns out to be one coherent
group — cache and CP0 control — and finds a third kernel table alongside the
two of `docs/analysis/16-ee-interrupt-syscalls.md`.

```sh
python3 tools/romdir.py assets/SCPH-50000.bin --extract <outdir>   # outside the repo
python3 tools/romdis.py <outdir>/KERNEL --cpu ee --vma 0x80000000 --range 0x80002c00 0x80002c20
```

## Why three slots must be uncached

**Slot `0x60`** (`0xA0002C00`) rewrites the cache mode:

```
80002c00  mfc0  $t0, $16          # Config
80002c04  andi  $a0, $a0, 0x3
80002c08  srl   $t0, $t0, 0x3     # drop the low three bits
80002c0c  sll   $t0, $t0, 0x3
80002c10  and   $t0, $t0, $a0
80002c14  mtc0  $t0, $16          # write it back
80002c18  sync.p
```

The low three bits of the R5900's `Config` are the cache mode. **Code that
changes them cannot itself be running cached** — the change would take effect
underneath the fetches bringing in its own next instructions.

**Slots `0x61` and `0x62`** (`0xA00028C0`, `0xA0002980`) are a matched pair
that walk the cache with the `cache` instruction:

```
800028c0  andi  $a0, $a0, 0x3
800028c4  mfc0  $t0, $16
800028c8  srl   $t0, $t0, 0x10
800028cc  andi  $t0, $t0, 0x1     # a Config enable bit
800028d0  bnez  $t0, ...          # 0x62 has beqz here -- the other polarity
800028e0  sync
800028e4  cache 0x16, 0x0($t0)
800028e8  cache 0x16, 0x1($t0)
800028ec  sync
800028f0  addiu $t0, $t0, 0x40    # one line at a time
800028f4  slti  $t1, $t0, 0x1000  # over the whole cache
```

Same reasoning: a loop invalidating every line of the cache must not be fetched
through it.

So EE-8e is not a stylistic detail a rebuild may normalise. Publishing these
three at their KSEG0 addresses would leave them running through the very cache
they are reconfiguring or invalidating — and the failure would be intermittent
and timing-dependent, which is the worst kind.

Note also that `0x61` and `0x62` differ only in the polarity of one branch
(`bnez` against `beqz`) on the same `Config` bit. They are the two halves of a
guarded pair, not two unrelated calls.

## The rest of the band

The other members are the cached companions of the same subject:

| Slots | Handler | What it does |
| --- | --- | --- |
| `0x63`, `0x67` | `0x80002C40` | read a CP0 register by number |
| `0x64`, `0x68` | `0x80002A40` | dispatch on `$a0` ∈ {0,1,2} to three routines |
| `0x65`, `0x69` | `0x80002B40` | a cache operation over an address **range** |
| `0x66`, `0x6A` | `0x80000A80` | `Status`/interrupt helpers, via the jump table at `0x80000AA0` |

Slot `0x65`'s range form is worth quoting, because its argument handling is a
requirement in itself:

```
80002b40  lui   $at, 0xffff
80002b44  ori   $at, $at, 0xffc0   # ~0x3F
80002b48  and   $a0, $a0, $at      # round the start down to a line
80002b4c  and   $a1, $a1, $at      # and the end
80002b5c  cache 0x10, 0x0($v0)
```

Both endpoints are masked to a 64-byte line boundary before the loop. A caller
passing unaligned addresses gets the enclosing lines operated on, and a rebuild
that instead rejected or rounded them the other way would leave a line
untouched at one end.

## A third table: CP0 reads

Slot `0x63` is a dispatcher, not a function:

```
80002c40  sll   $a0, $a0, 0x2
80002c44  lui   $t0, 0x8001
80002c48  addu  $t0, $t0, $a0
80002c4c  lwu   $t0, 0x54a8($t0)   # table at 0x800154A8
80002c50  jr    $t0
```

and the table's entries point at a run of two-instruction leaves immediately
after it:

```sh
python3 - <<'PY'
import struct
d = open('<outdir>/KERNEL', 'rb').read()
print([hex(struct.unpack_from('<I', d, 0x154a8 + i * 4)[0]) for i in range(8)])
PY
# ['0x80002c58', '0x80002c60', '0x80002c68', '0x80002c70', ...]
```

```
80002c58  jr    $ra
80002c5c  mfc0  $v0, $0
80002c60  jr    $ra
80002c64  mfc0  $v0, $1
...
```

Each leaf reads one CP0 register and returns it. The dispatcher exists because
`mfc0`'s register number is encoded in the instruction and cannot be supplied at
run time — so a table of one-per-register stubs is the only way to offer
"read CP0 register *n*" as a call.

That makes **three** tables the kernel publishes, all of which a rebuild must
provide:

| Address | Purpose | Entries | Established in |
| --- | --- | --- | --- |
| `0x80014F40` | syscall dispatch | 125 | `14` |
| `0x80015340` | exception dispatch | 14 | `14` |
| `0x80015380` | interrupt dispatch | 8 | `16` |
| `0x800154A8` | CP0 register reads | 8 | this document |

## What this pins for the rebuild

- Slots `0x60`–`0x62` must be published uncached because they reconfigure and
  invalidate the cache; this is a hardware constraint, not a convention, and
  getting it wrong fails intermittently rather than cleanly.
- `0x61` and `0x62` are one guarded pair distinguished by branch polarity on a
  `Config` bit.
- Slot `0x65` rounds **both** range endpoints down to a 64-byte line before
  operating.
- A fourth table at `0x800154A8` provides per-CP0-register read stubs, because
  the register number is an instruction field and cannot be a parameter.

Next: the scattered far-end slots — `0x01`, `0x4A`–`0x4F`, `0x6E`, `0x6F` —
which are the last unexamined group (`docs/project-state.md` §5).
