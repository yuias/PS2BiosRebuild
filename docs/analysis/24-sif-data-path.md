# The SIF Data Path: How a Transfer Says Where It Belongs

`docs/analysis/23-joining-the-two-cpus.md` ended by naming what it had left
out. Six shared registers were enough to make the two CPUs meet, but not to
make them talk: after the handshake the reference's EE builds a transfer, waits
for its SIF1 channel to report itself finished, and waits forever, because
nothing in `tools/ps2sim.py` drained the channel.

```
000842c8  jal   0x80083a80          # "has the channel finished?"
000842d0  beq   $v0, $zero, -3      # spin until it has
```

This document is what that channel turned out to be carrying. The short version
is one sentence, and it is the sentence the whole data path is built on: **the
sender says where the bytes belong, not the receiver**. Neither side can read
the other's memory, so neither side's driver can be told a destination by its
own caller — the destination travels *with* the data, in a header the receiving
channel parses before it stores anything.

```sh
python3 tools/ps2sim.py assets/SCPH-50000.bin              # the framed packets
python3 tools/ps2sim.py assets/SCPH-50000.bin --traffic     # and the tags walked
```

## Where the observations come from

Two sources, and they are worth separating, because only one of them is
reproducible with the tools in this repository.

The **framing of the EE-to-IOP direction is observed here**, by running the
reference image on `tools/ps2sim.py` and reading what its driver actually built.
Every claim in the next two sections names the command that shows it.

The **shape of the IOP-to-EE direction** was contributed from outside: notes
taken while bringing up an emulator against the same SCPH-50000 image
(`PS2e/docs/hw-notes.md`, a sibling project of the same author's, not part of
this repository). Those notes are analyst-side material of exactly the kind
`docs/clean-room-policy.md` rule 4 describes — behaviour and interfaces, not
code — but they are somebody else's run, not ours. They are used here as a
*hypothesis*, implemented in the simulator, and exercised by our own image;
where the reference could confirm them it did, and where it could not, this
document says so rather than pretending otherwise. See "What is still taken on
trust" at the end.

## The EE's outgoing channel: a tag list, not a length

The reference programs EE channel 6 — `0x1000C400`, SIF1, the EE-to-IOP
direction — with `CHCR = 0x184`. Read as the DMAC reads it that is `MOD` =
chain, `TIE` set, `STR` set, and `TTE` **clear**. It writes no `QWC` and no
`MADR`; what it writes is `TADR`, the address of a list of tags.

```sh
python3 tools/ps2sim.py assets/SCPH-50000.bin --traffic
```

```
chain tags walked (BOOT-11b):
   EE source  at 0x00021380: 0x00000003 0x00021580   id 0, 3 quadwords
   EE source  at 0x00021390: 0x00000002 0x00021600   id 0, 2 quadwords
```

Each tag is a quadword whose first word carries a quadword count in its low
sixteen bits and a three-bit id at bits 28-30, and whose second word is an
address. Both of these are id 0, `refe`: *transfer that many quadwords from
that address, and stop*. `TADR` advances by one quadword as each is consumed,
which is why the second walk starts sixteen bytes after the first without the
driver rewriting it.

`TTE` being clear is the detail that matters for everything below: the tag
quadwords are **not** transferred. What reaches the FIFO is only the data they
point at.

## What the data begins with: the IOP's header

Look at what the first tag points at, and the first quadword is not part of the
message at all:

```sh
python3 - <<'EOF'
import sys, pathlib
sys.path.insert(0, "tools")
import ps2sim
console = ps2sim.Console(pathlib.Path("assets/SCPH-50000.bin").read_bytes())
console.run()
ram, base = console.ee.bus.ram, 0x21580
for row in range(3):
    at = base + row * 16
    print(f"+{row * 16:#05x}  " + " ".join(
        f"{int.from_bytes(ram[at + i * 4:at + i * 4 + 4], 'little'):08x}"
        for i in range(4)))
EOF
```

```
+0x000  c0019600 00000008 00000000 00000000    <- the header
+0x010  00000014 00000000 80000002 00000000
+0x020  000935c0 00000001 200935c0 20093640
```

The header's first word is an address with flags above it and its second word
is a count in **words**; the remaining two words are padding to fill the
quadword. Bit 31 is the one this simulator acts on — it stops the receiving
channel's run — and the imported notes read bit 30 as a request to raise an
interrupt when the packet lands, which nothing here is in a position to check.

The address is the giveaway. `0x00019600` is precisely what the IOP published
in `SMCOM` during the handshake:

```
SMCOM 0x00019600   SMFLG 0x00010000   (the IOP's to write)
```

So the EE is not choosing an address out of its own knowledge of IOP memory. It
is sending the data back to the buffer the IOP itself nominated, and it is the
IOP's *channel*, not the IOP's *code*, that reads that address out of the
stream and stores there. Running it confirms the round trip:

```
SIF transfers:
   EE sent          48 bytes at 0x00021580
   EE sent          32 bytes at 0x00021600
   IOP received     32 bytes at 0x00019600

framed packets (BOOT-11), as the receiving channel read them:
   EE -> IOP      8 words to 0x00019600   header 0xc0019600
```

Eight words is thirty-two bytes, and forty-eight bytes left the EE: sixteen of
header and thirty-two of data. The arithmetic closes, and it closes only
because the count is in words while the transfer is in quadwords — the payload
is **padded up to a quadword boundary**. What the packet above actually carries
is a twenty-byte SIFCMD packet (`psize 0x14`, `cid 0x80000002`, and an EE RAM
address of `0x000935C0` for the IOP to reply into) rounded up to thirty-two.

Only the first of the two packets is received. The header's bit 31 stops the
IOP's channel after it, and the IOP re-arms from an interrupt handler this
simulator does not run — so the second packet stays in the FIFO. That is the
boundary of what is modelled, not a disagreement with the framing.

## The other direction: a send block, and a tag made for the EE

The IOP-to-EE direction is the mirror image with one extra step. It began as
the part contributed from outside; the interrupt work described further down
turned it into an observation.

The IOP's channel 9 (`0x1F801520`) also works from a `TADR`, and what sits
there is a sixteen-byte **send block** per packet:

| Word | Meaning |
| --- | --- |
| 0 | address in IOP RAM to read, with the same end/interrupt flags above it |
| 1 | how many words to send |
| 2 | the EE destination tag, low word |
| 3 | the EE destination tag, high word |

The last two words are the extra step. The EE's channel 5 (`0x1000C000`, SIF0)
runs as a **destination** chain: it pops a quadword off the front of the FIFO,
reads a quadword count and an address out of it, and stores what follows there.
That tag has to come from somewhere, and the receiving side cannot invent it —
so the *sending* side carries it, ready-made, in its own send block, and the
channel pushes it into the FIFO ahead of the data.

Which is the same rule as the other direction, stated in the other direction's
vocabulary. In both cases the receiving channel is armed with a mode and
nothing else; every address it uses arrives in front of the bytes it applies
them to.

## Modelling it, and what that made possible

`tools/ps2sim.py` now models all three framings — the EE's source chain, the
IOP-facing packet header, the IOP's send block and the EE destination tag it
carries. Two changes were needed beyond the parsing:

- **A started channel that cannot finish stays busy.** A receiver is armed
  before the sender has pushed anything. Clearing `STR` there and reporting
  zero bytes moved would tell its driver a transfer had happened. Armed
  channels are now retried each turn and only report themselves idle once
  their packet has actually crossed.
- **`TADR` advances.** Both sides re-arm a channel without rewriting it, and a
  model that did not advance it would replay the first packet forever.

With that, the reference's EE gets past the spin quoted at the top of this
document. It does not get much past it — it goes on to wait for an answer that
needs the IOP's interrupt handlers to produce — but the transfer it was waiting
on completes, and the completion is now a gate:

```
BOOT-11a: nothing crossed to the IOP with a header in front of it
BOOT-11a: headers sent the data to ['0x...'], not to the address the IOP
          published in SMCOM (0x00019600)
```

Both are checked by `tools/ps2sim.py --check` and both appear when it is run
with `--no-bridge`, which is the failing direction the gate is tested in.

## Our own image now speaks the same framing

Before this, our two halves moved data in normal mode with a quadword count
each side agreed on out of band — a deviation `docs/implementation.md` recorded
and `tools/imgcheck.py` listed as not-yet-done. There is no longer a reason for
it, and it is gone. The EE builds a one-tag source list over a packet headed
for the address the IOP published; the IOP builds a send block whose EE tag
names the address the EE asked for in its request. The request grew one word to
carry that address, because with the framing right there is nowhere else for it
to live: `MADR` on the receiving end is no longer consulted.

```sh
python3 tools/ps2sim.py build/rom.bin
```

```
framed packets (BOOT-11), as the receiving channel read them:
   EE -> IOP      8 words to 0x00000d40   header 0x80000d40
   IOP -> EE      4 words to 0x000153b0   header 0x70000001
   EE -> IOP      8 words to 0x00000d40   header 0x80000d40
   IOP -> EE      4 words to 0x00017520   header 0x70000001
   EE -> IOP      8 words to 0x00000d40   header 0x80000d40
   IOP -> EE    324 words to 0x00017530   header 0x70000051
```

Three exchanges: ask the size of `ROMVER`, take it, and then the same pair for
`OSDSYS`, whose 324 words are the file that the boot goes on to run. The
`0x70000000` in the returning tags is id 7, `end` — our IOP marks each answer
as the last of its run, since it sends one packet per request.

This closes the last place where the two sides of our image agreed on something
the hardware does not agree on. It is also the change most likely to matter
when the image is first tried in PCSX2, which emulates the channels rather than
the convention two cooperating halves could have invented.

## One interrupt turns the trust into an observation

The reference's IOP would not reach its sender: it finishes its boot list, parks
in the idle loop, and nothing wakes it. Reading its state at that moment says
precisely what it is waiting for, and every register in the picture is one of
the imported notes' claims, now checked here:

```
IOP CP0 SR 0x00000401       # IEc set, and hardware line 2 unmasked
I_MASK     0x0001080d       # vblank, CDVD, DMA, evblank, timer 5
DICR2      0x000c0400       # DMA interrupt enabled for channels 9 and 10
DICR       0x00800000       # ... under the master enable in the other bank
```

`I_MASK` is `0x1080D` exactly as the notes said. So the IOP is not stuck; it is
*armed*, and one source it is armed for — a SIF channel finishing — is one this
simulator was in a position to raise. That took three pieces:

- the interrupt controller: `I_STAT` acknowledged by writing a **zero** to the
  bit being cleared, `I_MASK` as plain storage, and `I_CTRL` whose *read*
  disables interrupts and hands back the previous state, which is how a
  critical section begins and why storing it would let an interrupt land inside
  one;
- delivery: a pending, unmasked, ungated source raises `Cause` IP2, and the
  core takes it between instructions when `IEc` and `IM2` are both set;
- the DMA controller's own flags, which are **write-one-to-clear**. This one
  cost a debugging round and is worth stating plainly: stored verbatim, a flag
  the handler had cleared stuck, the driver kept being told about an interrupt
  it had already served, and it responded by masking the DMA source off
  entirely — `I_MASK` ended the run as `0x10805`, with bit 3 gone. The symptom
  looked nothing like the cause.

With that, the reference answers:

```
SIF transfers:
   EE sent          48 bytes at 0x00021580
   EE sent          32 bytes at 0x00021600
   IOP received     32 bytes at 0x00019600
   IOP received     16 bytes at 0x00019600
   IOP sent         32 bytes at 0x00019870
   EE received      32 bytes at 0x000935c0

framed packets (BOOT-11), as the receiving channel read them:
   EE -> IOP      8 words to 0x00019600   header 0xc0019600
   EE -> IOP      4 words to 0x00019600   header 0xc0019600
   IOP -> EE      8 words to 0x000935c0   header 0x90000002
```

Both of the EE's packets are taken now that the IOP re-arms its channel from
the handler, and **the reply lands at `0x000935C0`** — which is the address
that appeared inside the very first packet's payload, the EE receive buffer the
EE nominated in its init message. BOOT-11c is no longer exercised-only: the
reference builds the send block, and the tag it puts in front of the data is
read here by the EE's own channel.

That tag is `0x90000002`, which also settles what our image had to guess. Id 1,
`cnt` — the data follows the tag, which is exactly the SIF0 case — with bit 31,
the interrupt request, set. Not id 7. Our image sends `end` instead, which the
simulator accepts and which is not wrong, but it is not what the reference
writes, and `docs/implementation.md` now records that as a deviation rather
than leaving it unstated.

## What is still taken on trust

1. **The EE takes no interrupts.** Its INTC and DMAC are unmodelled, so the
   reply it has now received sits in its buffer unprocessed and the reference's
   EE stops one step further on than it used to rather than going on. The
   imported notes describe the missing piece — the IOP's writes to `SMFLG`
   raise the EE's INTC SBUS source, bit 1, and the EE's handler folds the flags
   into an `SREG` array — and none of it is verified here.
2. **The IOP's other sources are still absent.** Vblank and timer 5 are in
   `I_MASK` and nothing raises them, so any IOP thread that sleeps on a delay
   sleeps for the rest of the run. Only the SIF path is awake.
3. **Bit 30 of the outgoing header** is read as an interrupt request on the
   notes' authority. This simulator acts on bit 31 only.
