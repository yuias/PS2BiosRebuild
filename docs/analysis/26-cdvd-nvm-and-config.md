# CDVD: the NVM word interface and the OSD configuration blocks

`docs/analysis/12-ee-facing-services.md` surveyed `CDVDMAN` and stopped at its
shape — 62 exports, a 90 KB `.bss`, an EE-facing half in `CDVDFSV`. This
document reads the part of it that stores settings: the mechanism the OSD uses
to decide whether the console has been configured.

The question came from outside. A sibling project bringing up an emulator
against the same image reports that its OSD stops on the first-boot screen with
a zeroed NVRAM, and that supplying a hand-made "already configured" block made
the OSD draw nothing at all — a validity test failing rather than passing. What
follows is the transport that test runs over, and the check it applies.

**The useful surprise is that none of this needs `OSDSYS`.** `20` established
that `OSDSYS` is 99% one compressed blob, so anything inside it costs a
decompressor first. The configuration *transport*, including its checksum,
lives in `CDVDMAN`, which is an ordinary uncompressed IRX and reads with the
tools already here.

## Finding the four entry points

`CDVDMAN` carries diagnostic format strings that name its own routines, so the
entry points can be located without reading the whole module:

```sh
python3 tools/romdir.py assets/SCPH-50000.bin --extract <outdir>   # outside the repo
strings -n 4 <outdir>/CDVDMAN | grep -iE "nvm|config"
```

```
ReadNVM call addr= 0x%04x data= 0x%04x stat= 0x%02x
WriteNVM call addr= 0x%04x data= 0x%04x stat= 0x%02x
ReadConfig fail Command busy
ReadConfig fail status: 0x%02x
WriteConfig fail Command busy
WriteConfig fail status: 0x%02x
```

Dumping the loadable image puts file offsets and module addresses on the same
scale, which every command below relies on:

```sh
python3 tools/irxinfo.py <outdir>/CDVDMAN --dump-load <outdir>/CDVDMAN.load
python3 tools/romdis.py  <outdir>/CDVDMAN.load --cpu iop --vma 0 --range <a> <b>
```

Each string's address then appears as the immediate of the `addiu` that forms
its pointer, which names the enclosing routine, and the export table
(`--exports`) turns the routine into an ordinal:

| String | at | referenced from | routine at | ordinal |
| --- | --- | --- | --- | --- |
| `ReadNVM call` | `0x5630` | `0x41ac` | `0x411c` | **26** |
| `WriteNVM call` | `0x5668` | `0x4268` | `0x41e0` | **27** |
| `ReadConfig fail …` | `0x56b4`, `0x56d4` | `0x48bc`, `0x48f8` | `0x4864` | **33** |
| `WriteConfig fail …` | `0x56f4`, `0x5714` | `0x4a94`, `0x4ad0` | `0x4a3c` | **34** |

Two more belong to the same group, identified below by the commands they send:
ordinal **31** at `0x467c` and ordinal **32** at `0x4710`.

## Everything goes through one command sender

All six call a single routine at `0x2b70`, which is the module's S-command
transport:

```sh
python3 tools/romdis.py <outdir>/CDVDMAN.load --cpu iop --vma 0 --range 0x2b70 0x2dbc
```

Its signature is `send(command, out_buffer, out_length, in_buffer, in_length)`,
the fifth argument on the stack at `0x10`, and it is a mutex-protected
sequence over three byte-wide registers:

| Register | Use |
| --- | --- |
| `0xBF402016` | write: start the command whose number is written here |
| `0xBF402017` | read: status; write: one parameter byte |
| `0xBF402018` | read: one result byte |

Two bits of the status register carry the whole protocol:

| Bit | Meaning as the code uses it |
| --- | --- |
| `0x80` | busy — a command is in progress |
| `0x40` | the result FIFO is empty |

The order is fixed and worth stating exactly, because an emulator that models
the registers as plain storage will hang on the third step:

1. Take the semaphore (`thsemap` 9). A return of `-419` aborts with 0.
2. Read the status. **If `0x80` is set, give up** — release and return 0. This
   is the "Command busy" the callers report; the sender never waits for a busy
   channel, it declines.
3. Drain: while `0x40` is *clear*, read `0xBF402018` and discard. Stale results
   from a previous command are thrown away before a new one is sent.
4. Write the parameter bytes, one at a time, to `0xBF402017`.
5. Write the command number to `0xBF402016`.
6. Spin while `0x80` is set.
7. While `0x40` is clear, read result bytes from `0xBF402018`, counting them.
   The count is compared against the expected length and the *smaller* of the
   two is copied to the caller — a short reply is truncated, not an error.
8. Release the semaphore (`thsemap` 6) and return 1.

So the return value is "the command was sent", not "the command succeeded".
Every caller treats 0 as the busy case and looks elsewhere for the outcome.

Steps 3 and 7 both terminate on `0x40` becoming set, which means an emulator
must raise that bit when the FIFO empties or step 7 never returns.

## NVM is addressed as 16-bit words, big-endian on the wire

Ordinals 26 and 27 are one word each:

```sh
python3 tools/romdis.py <outdir>/CDVDMAN.load --cpu iop --vma 0 --range 0x411c 0x429c
```

| | ordinal 26 | ordinal 27 |
| --- | --- | --- |
| arguments | `(address, u16 *data, u8 *status)` | `(address, data, u8 *status)` |
| S-command | `0x0A` | `0x0B` |
| parameters | 2 bytes | 4 bytes |
| result | 3 bytes | 1 byte |

Both byte-swap what they send: the parameter bytes are `[addr>>8, addr&0xff]`
and, for the write, `[addr>>8, addr&0xff, data>>8, data&0xff]`. The read's
three result bytes come back as `[status, data>>8, data&0xff]` and are
unpacked the same way. The address is a **word index**, not a byte offset —
one call moves exactly one halfword.

## The configuration area is four commands and 15-byte blocks

The remaining four ordinals are a session: open, then read or write a run of
blocks, then close.

```sh
python3 tools/romdis.py <outdir>/CDVDMAN.load --cpu iop --vma 0 --range 0x4648 0x4a3c
```

| Ordinal | at | S-command | parameters | result |
| --- | --- | --- | --- | --- |
| 31 — open | `0x467c` | `0x40` | 3 bytes | 1 byte |
| 33 — read | `0x4864` | `0x41` per block | none | 16 bytes |
| 34 — write | `0x4a3c` | `0x42` per block | 16 bytes | 1 byte |
| 32 — close | `0x4710` | `0x43` | none | 1 byte |

**Open** takes `(a, b, count, u32 *status)` and sends the three bytes
`[b, a, count]` — note the first two are transmitted in the opposite order to
the argument list. It stores `count` in a word of `.bss` at `0x81ac` and zeroes
the caller's status word before sending. It also delays 16000 through `thbase`
33 *before* the command, so the device is given time it apparently needs.

**Close** sends no parameters and clears the same `.bss` word, so the block
count is the session's only state and a read outside a session does nothing.

**Read** and **write** are loops over that count, and both advance the caller's
buffer by **15** bytes per iteration (`addiu $17, $17, 0xf`). Each stops early
on either failure and returns the number of blocks it completed, which is how a
caller distinguishes "no session" (0) from a mid-run fault.

## The checksum, confirmed from both directions

The per-block read helper is at `0x474c` and the per-block write helper at
`0x4944`. They are the pair that answers the original question.

```sh
python3 tools/romdis.py <outdir>/CDVDMAN.load --cpu iop --vma 0 --range 0x474c 0x4864
python3 tools/romdis.py <outdir>/CDVDMAN.load --cpu iop --vma 0 --range 0x4944 0x4a3c
```

**A block is 16 bytes on the wire and 15 bytes to the caller.** The sixteenth
byte is a checksum, and it is the plainest one possible: the **sum of the
fifteen data bytes, modulo 256**.

The read helper requests 16 bytes, adds bytes 0 through 14 with fourteen
straight-line `addu`s, masks the sum to 8 bits, `xor`s it against byte 15 and
reduces the result with `sltu $3, $zero, $3` — so it stores **1 for a mismatch
and 0 for a match** through its second argument. Then it copies bytes 0..14 to
the caller and drops byte 15. The checksum is never seen above `CDVDMAN`.

The write helper is the same arithmetic in reverse: it copies the caller's 15
bytes into a 16-byte buffer, sums them with the identical unrolled chain,
stores the sum as byte 15, and sends all 16.

That the two are independent stretches of code computing the same sum is the
confirmation. Nothing here is inferred from one reading.

One asymmetry to note, since the two routines take the same-looking status
pointer: on **read** the word receives the *checksum verdict* computed locally,
while on **write** it receives the device's *reply byte*. The callers' shared
`fail status: 0x%02x` message therefore reports two different things.

## What `CDVDMAN` does not do

It never looks inside the fifteen bytes. There is no field decoding, no version
check, no "is this configured" test anywhere in this group — the module moves
opaque blocks and validates only the sum. **Everything about what the bytes
mean, and the rule that decides a block is unconfigured, is above this layer**,
which for the boot path means `OSDSYS`, and therefore inside its compressed
payload.

The EE does reach these ordinals. `CDVDFSV`, the EE-facing half, binds the
whole quartet:

```sh
python3 tools/irxinfo.py <outdir>/CDVDFSV --imports
# cdvdman ordinals ... 31, 32, 33, 34 ...
```

so the path is `OSDSYS` on the EE → `CDVDFSV`'s RPC across the SIF →
`CDVDMAN` ordinals 31/33/32 on the IOP → S-commands `0x40`/`0x41`/`0x43`.

## What this pins for the rebuild

- The CDVD S-command transport is three byte registers at `0xBF402016`–`18`,
  with status bit `0x80` busy and bit `0x40` result-FIFO-empty. A sender that
  finds `0x80` set declines rather than waiting.
- NVM is word-addressed: S-commands `0x0A`/`0x0B`, one halfword per call,
  big-endian in both parameters and results.
- The configuration area is a session: `0x40` open with a block count, `0x41`
  read and `0x42` write one block at a time, `0x43` close.
- **A configuration block is 15 data bytes plus a one-byte sum of them, and
  `CDVDMAN` verifies it on read and generates it on write.** A hand-made block
  whose sixteenth byte is not that sum is rejected before anything above sees
  it.
- The session's block count is the driver's only state, and reads outside a
  session return zero blocks rather than failing.
- The meaning of the fifteen bytes is not in `CDVDMAN`. It is in `OSDSYS`, and
  reaching it needs the payload expanded first.
