# Why SIF0 delivered nothing on PCSX2

`docs/analysis/25-first-run-on-pcsx2.md` left the project with one open
question and called it the blocker: the EE armed its receiving channel, the IOP
built its reply and raised the flag, and the EE's buffer was untouched. Every
obvious answer had been ruled out by measurement — the destination, the channel
enables, the shape of the destination tag — and the next thing to establish was
whether the IOP's channel put anything into the FIFO at all.

It did. The fault was on the other side of the bus, one bit wide, and invisible
to every instrument this project had written for itself.

## The instruments

Two, neither of them new:

- **The EE's serial console**, which `25` established is the only thing that
  reaches inside the running image. With the kernel now in C++ it costs four
  lines to print a channel's four registers.
- **`SMCOM`**, which is the one register both processors can read and which the
  EE has finished with by the time a transfer starts — the handshake value is
  already latched. The IOP writing a value there and the EE printing it is a
  diagnostic channel that needs no working data path, which is the property
  that matters when the data path is what is broken.

## What the registers said

The EE's two channels, read immediately after the exchange:

```
# sif0 CHCR=90000084 MADR=00000010 QWC=00000000 TADR=00000000
# sif1 CHCR=00000084 MADR=00000000 QWC=00000000 TADR=80017840
# ctrl=f0000102 smcom=00000da0 first word=00000000
```

Three readings, in the order they change the picture:

**The IOP's reply arrived, correctly framed.** `sif0`'s `CHCR` reads back with
`STR` clear and its upper halfword holding `0x9000` — the top of the destination
tag the IOP built, `0x90000000 | qwc`. The EE's channel popped that tag and
acted on it. `MADR` of `0x10` says it then stored exactly one quadword, which is
the right amount for a sixteen-byte `ROMVER`.

**It stored it at physical zero.** `MADR` ends at `0x10`, so it began at `0`.
The tag's address field was zero, and that address comes from the request the
EE sent.

**The request never left the EE.** `sif1` — the outgoing channel — reads back
`MADR` of `0`, and its `TADR` still holds the tag address it was given.
`docs/analysis/24-sif-data-path.md` records that `TADR` advances as tags are
consumed. This one consumed nothing, and yet `STR` is clear: the channel
reported itself finished without doing anything.

The `SMCOM` diagnostic settled it. Making the IOP write the destination it had
read out of the request, just before raising the reply flag:

```
# ctrl=f0000102 smcom=00000000 first word=00000000
```

Zero. The IOP's request buffer was untouched, so it replied to address zero
because that is what it had been told, and the whole failure is upstream of it.

## The cause: bit 31 of `MADR` and `TADR`

The EE's DMAC takes **physical** addresses, and it uses the top bit of `MADR`
and `TADR` for something else: **bit 31 selects the scratchpad**. An address
handed to it with that bit set names the sixteen kilobytes at `0x70000000`, not
main memory.

`KERNEL` is linked in KSEG0. Every pointer this kernel takes therefore arrives
as `0x8001xxxx`, with bit 31 set. Writing that into `TADR` asked the controller
to read a source tag out of the scratchpad, where it found whatever the reset
path had left; the channel started, made nothing of it, and stopped. The
destination the EE published in its request had the same bit set and reached the
IOP as an address the IOP passed straight back into a tag.

The fix is one function:

```cpp
[[nodiscard]] uint32_t physical(const void *pointer) {
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pointer))
           & 0x1FFFFFFF;
}
```

applied to the address published in `MSCOM`, to the destination carried in a
request, and to `TADR`.

## Why the simulators could not see it

`tools/ps2sim.py` and `tools/eesim.py` resolve an address by masking it:

```python
physical = address & 0x1FFFFFFF
```

which is the correct way to model KSEG0 and KSEG1 for a *CPU* access, and which
makes `0x80017840` and `0x00017840` the same word. That is exactly the
distinction the DMAC draws. The simulators were not wrong about the CPU; they
simply had no opinion about the controller, and a bug that lives only in the
difference between the two could not appear in them.

This is the demonstration `docs/project-state.md` §6 was arguing for in the
abstract: a gate written by the same hands as the code shares its blind spots,
and the working target is what finds them.

## The result

The image boots end to end on PCSX2:

```
# PS2BiosRebuild EE kernel: entered at 0x80001000.
# The IOP answered; the SIF handshake is complete.
# ROMVER, fetched from the archive across the SIF: 0100XP20260810
# OSDSYS: loaded from the archive and running. Argument: BootBrowser
```

Both processors reset, the IOP loads and links its modules, the two meet, a
file crosses the bus in each direction, and the program the archive holds for
the end of the boot is placed and entered — on the target the image is built
for, rather than on an instrument written alongside it.

## What this pins for the rebuild

- **Every address given to the EE's DMAC must be physical**, and not only
  because the controller does not walk the TLB: bit 31 of `MADR` and `TADR`
  selects the scratchpad, so a KSEG0 pointer is not merely untranslated but
  actively means something else.
- That applies to `TADR`, to the address published in `MSCOM`, and to any
  address a request carries for the other processor to put in a tag.
- The earlier reading — that EE-to-IOP transfers worked on the target and
  IOP-to-EE ones did not — was the wrong way round. The IOP's reply was framed
  correctly the whole time; it had nowhere to put it because the request that
  named the destination had never crossed.
- `tools/ps2sim.py` masks addresses on their way into memory and so cannot
  distinguish a physical address from a KSEG0 one. Modelling the DMAC's bit 31
  would turn this into a gate, and until it does the class of fault stays
  invisible to the simulators.
