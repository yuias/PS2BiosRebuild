# Expanding the `OSDSYS` payload, and the config it reads

`docs/analysis/20-osdsys.md` stopped at the container: `OSDSYS` is 99% one
compressed blob behind a 3 KB decompressor, and reproducing the payload is out
of scope. `docs/analysis/26-cdvd-nvm-and-config.md` then took the OSD's
configuration as far as `CDVDMAN` goes — a 15-byte block plus a one-byte sum —
and stopped where the driver stops: it never interprets the bytes.

What is left is on the other side of the compression. This document gets there,
and it does so by **executing** the decompressor rather than decoding its
format.

## The expander runs under `tools/eesim.py`

The four `.text.Expand*` sections total 484 bytes. Reading them closely enough
to reimplement the format is real work; running them is not, because the
simulator that already boots the reference EE can run them as they stand.

Checking that first is what makes it cheap. The group uses no `$gp`, no
multimedia instructions and no quadwords:

```sh
python3 tools/irxinfo.py --help >/dev/null   # (the check below is a one-liner)
python3 - <<'PY'
import struct
d = open('<outdir>/OSDSYS.seg', 'rb').read()      # the PT_LOAD, extracted below
ops = {}
for i in range(0xaf8, 0xd04, 4):
    w, = struct.unpack_from('<I', d, i)
    ops[w >> 26] = ops.get(w >> 26, 0) + 1
print(sorted(hex(k) for k in ops))
PY
# ['0x0', '0x23', '0x24', '0x28', '0x2b', '0x3', '0x37', '0x3f', '0x4', '0x5',
#  '0x9', '0xc', '0xf']  -- MIPS-III integer only
```

So `eesim` grew a mode that loads an `ET_EXEC` into RAM and calls one address
in it, with no ROM and no kernel underneath — the executables of `spec/02` are
ordinary programs, and a self-contained routine inside one needs nothing else:

```sh
python3 tools/romdir.py assets/SCPH-50000.bin --extract <outdir>   # outside the repo
python3 tools/eesim.py <outdir>/OSDSYS --call 0x100af8 0x100d80 0x200000 \
        --dump 0x200000 result <outdir>/OSDSYS.expanded
# call 0x00100af8(0x100d80, 0x200000): 10361576 instructions, returned 761812
```

The arguments come from the caller at `0x1000c0`, which loads `$a0` with
`0x100d80` — the start of `.data` — and `$a1` with `0x200000`. `Expand` itself
is `ExpandInit(source)` then `ExpandMain(destination)`, returning a word the
expander leaves at `0x152348`.

**That return value is `761812`, and the first word of `.data` is `0x000B9FD4`
— the same number.** `20` inferred from the ratio that the header word was an
expanded size; it now *is* one, measured. (`20` also states that figure in
decimal as 761300, which is an arithmetic slip for the same hex value.)

The result is the OSD proper, and it identifies itself:

```sh
strings -n 8 <outdir>/OSDSYS.expanded | grep -E "BootBrowser|sceCdInit"
```

```
BootBrowser
sceCdInit: cdvdfsv Ver %02d%02d cdvdman Ver %02d%02d
```

along with the menu text in seven languages. It is a second program of the same
shape as its container — entry stub, BSS clear, then the body — loaded at
`0x200000`.

Everything below is read from that expanded image at that address. It is
derived from the reference and, like any other extract, stays outside the
repository.

## Reaching the config from the EE means one RPC service

The EE cannot call `CDVDMAN` directly. `CDVDFSV` is its half, and it exposes a
numbered RPC service:

```sh
python3 tools/irxinfo.py <outdir>/CDVDFSV --dump-load <outdir>/CDVDFSV.load
python3 tools/romdis.py <outdir>/CDVDFSV.load --cpu iop --vma 0 --range 0x41b8 0x41f8
```

Service **`0x80000593`** dispatches on a function number of 1 to 25 through a
jump table at `0x50e8`. Six of its cases are the group `26` described, and each
case calls a wrapper that calls exactly one `cdvdman` ordinal — which is how
the numbering is pinned rather than guessed:

| RPC fno | wrapper | calls | `26` calls it |
| --- | --- | --- | --- |
| 8 | `0x3944` | `cdvdman` 26 | ReadNVM |
| 9 | `0x39b0` | `cdvdman` 27 | WriteNVM |
| 14 | `0x3a88` | `cdvdman` 31 | open |
| 15 | `0x3b20` | `cdvdman` 32 | close |
| 16 | `0x3b94` | `cdvdman` 33 | read |
| 17 | `0x3c0c` | `cdvdman` 34 | write |

Its `13` sits between the NVM pair and the config group and calls neither, so
the group is 14–17 and nothing else.

The same numbers appear on the EE side. `OSDSYS` binds the service at
`0x256a38` with its client structure at `0x404D20`, and seven wrappers pass
that client with a fixed function number:

```sh
python3 tools/romdis.py <outdir>/OSDSYS.expanded --cpu ee --vma 0x00200000 \
        --range 0x256a10 0x256a50
```

| fno | wrapper in `OSDSYS` |
| --- | --- |
| 8, 9 | `0x258370`, `0x258470` |
| 13 | `0x258568` |
| 14, 15 | `0x258628`, `0x258748` |
| 16, 17 | `0x258808`, `0x2588f0` |

## The OSD reads exactly two blocks

Only two places in `OSDSYS` use the config wrappers: a reader at `0x203a38` and
a writer at `0x20d3d8`.

```sh
python3 tools/romdis.py <outdir>/OSDSYS.expanded --cpu ee --vma 0x00200000 \
        --range 0x203a38 0x203ac0
```

The reader is `open(1, 0, 2)`, then read, then close, each retried:

```
203a48  addiu $4, $zero, 0x1      # first argument 1
203a50  move  $5, $zero           # second argument 0
203a54  addiu $6, $zero, 0x2      # block count 2
203a58  jal   0x258628            # fno 14, open
...
203a60  lw    $3, 0x0($sp)        # the status the open wrote
203a64  andi  $3, $3, 0x81        # retry while either bit is set
```

**The block count is 2, so the OSD's configuration is 30 bytes** — two 15-byte
blocks, each of which arrived with its own sum byte and was verified and
stripped by `CDVDMAN` (`26`). `26` said the open's three parameters go out as
`[second, first, count]`; here that is the wire triple `[0, 1, 2]`.

The status test is its own small fact: the caller retries the whole call while
either bit `0x01` or bit `0x80` of the returned status is set, so a model that
leaves either standing makes the OSD spin here rather than fail.

Two 15-byte copy loops sit beside the reader, at `0x203a08` and `0x2039d8`
(`slti $2, $6, 0xf` in both), which is the block size confirmed independently
of `CDVDMAN`.

## What the OSD actually looks at

The consumer at `0x203ba8` reads the two blocks into a static 30-byte buffer at
`0x2BA780`, then splits them:

```sh
python3 tools/romdis.py <outdir>/OSDSYS.expanded --cpu ee --vma 0x00200000 \
        --range 0x203ba8 0x203c08
```

- **block 0** — buffer bytes 0–14 — is copied out whole, uninterpreted, to the
  caller's first argument.
- **block 1** — buffer bytes 15–29 — goes to a decoder at `0x203698` which
  packs bit fields out of its first two bytes into one word:

| Source | Into |
| --- | --- |
| `buffer[0x0F] & 0x01` | bit 0 |
| `buffer[0x0F] & 0x06` | bits 1–2 |
| `buffer[0x0F] & 0x08` | bit 3 |
| `buffer[0x0F] >> 5`, if non-zero: `buffer[0x10] & 0x1F` | bits 4–8 |

The last one is the shape worth noting: **a field in the second byte is only
taken when the top three bits of the first are non-zero**, and otherwise the
word keeps a default. So block 1's first byte is not merely a set of flags; its
upper bits gate whether the rest of the record is believed at all. That is the
kind of test a hand-made block fails silently.

## What this pins for the rebuild

- The `.text.Expand*` group is plain MIPS-III and runs under `tools/eesim.py`
  as it stands. The compression format never had to be decoded to get at what
  it hides — which also means a rebuild still owes none of it (`20`).
- The word at the head of `OSDSYS`'s `.data` is the expanded size, `761812`
  bytes, now measured rather than inferred.
- The EE reaches the configuration through `CDVDFSV` RPC service `0x80000593`,
  functions 14 (open), 15 (close), 16 (read) and 17 (write); 8 and 9 are the
  NVM word pair.
- **The OSD's configuration is two blocks — 30 bytes** — read as `open(1, 0, 2)`.
- The caller retries while the returned status has bit `0x01` or `0x80` set.
- Block 0 is passed through untouched. Block 1's first two bytes are decoded
  into bit fields, and the upper bits of its first byte gate whether the second
  byte is read at all.
- What the individual fields *mean* is still open: it needs the callers of the
  decoded word followed into the menu code, which is a larger piece of work
  than this document.
