# Deci2Call: what the sub-functions answer, and why a title's open fails

`docs/spec/05` SYS-3c was written from a dispatcher read alone: slot `0x7C`
runs on a frame of its own and restores every register from it, so a
sub-function that writes nothing is invisible, and the only answer it named
was `-1` for an `fno` out of range. That was enough while nothing on this
image opened a DECI2 socket. A title does, and on the reference its open is
*refused* -- and the refusal is what lets the title go on. This reads the
sub-functions that answer, the table they answer from, and where the state
that refuses the title comes from.

```sh
python3 tools/romdir.py assets/SCPH-50000.bin --extract <outdir>   # outside the repo
python3 tools/romdis.py <outdir>/KERNEL --cpu ee --vma 0x80000000 --range 0x800141fc 0x80014280
python3 tools/romdis.py <outdir>/KERNEL --cpu ee --vma 0x80000000 --range 0x8000ecc8 0x8000ee10
python3 tools/romdis.py <outdir>/KERNEL --cpu ee --vma 0x80000000 --range 0x8000e860 0x8000ec00
python3 tools/romdis.py <outdir>/KERNEL --cpu ee --vma 0x80000000 --range 0x800139c0 0x80013ab0
python3 tools/romdis.py <outdir>/KERNEL --cpu ee --vma 0x80000000 --range 0x80010208 0x800102c0
```

## 1. The dispatch, and where an answer goes

`0x800141fc` is where the syscall entry sends a positive `0x7C`
(`docs/analysis/14`). It saves the caller's registers into a frame at
`0xFFFF8000`, steps EPC past the `syscall`, calls the body `0x8000ECC8` with
`$a0`/`$a1` still the caller's, restores every register from the frame and
`eret`s. The body indexes a sixteen-word table at `0x80016460` by `fno - 1`
after `sltiu` against 16, so `fno` `1`..`0x10` reach a handler and anything
else takes the `-1` path at `0x8000EDE4`.

A handler answers by storing to **the frame's `$v0` doubleword at
`0xFFFF8020`**; the body's common exit reloads `$v0` from there
(`0x8000EDF8`). Handlers that return in `$v0` without that store (`fno`
`5`..`9`, `0x8000ED68`..`0x8000EDC8`) are overwritten by the restore, which is
the "invisible" shape SYS-3c already described.

| `fno` | handler | answers |
| --- | --- | --- |
| 1 | `0x8000ECFC` -> `0x8000EAF8` open | socket, `-3`, `-4` |
| 2 | `0x8000ED20` -> `0x8000EBB0` close | `1`, `-2` |
| 3 | `0x8000ED30` -> `0x800139C0` request a send | `1`, `-2`, `-10`, `-11` |
| 4 | `0x8000ED50`: `0x8000FDE8` link poll, `0x80012540(0)` | nothing: the caller's own `$v0` |
| 5..9 | `0x8000ED68`..`0x8000EDBC` | nothing: the value is lost in the restore |
| 0xA..0xF | `0x8000EDE4` | `-1` (unassigned in the table) |
| 0x10 | `0x8000EDCC` -> `0x80010208` | `-1` -- see §4 |

The negative form `syscall` with `$a0 < 0` (`0x80014258`) negates `fno` and
calls the body with no frame at all, so there `$v0` is the body's return.
Nothing traced uses it.

## 2. The socket table

`0x80023E30`, seventeen entries of twelve bytes (index 0 unused): a protocol
halfword at `+0`, an option word at `+4`, a handler at `+8`.

- **find** (`0x8000E8A0`): scans indices 1..16 for an entry whose protocol is
  non-zero and equal to the argument; returns the index or 0. An empty entry
  never matches, so protocol 0 is never found.
- **valid** (`0x8000E860`): `socket - 1 < 16` and the entry's protocol
  non-zero.
- **open** (`0x8000EAF8`, `fno 1`, block `{protocol halfword, option,
  handler, caller's $gp}`): if find(protocol) > 0, **`-3`**. Otherwise the
  first empty entry from **index 2** upward takes `{protocol, option,
  handler}` and its index is the answer; none free, `-4`. The block's fourth
  word is stored at `0x80023F80` before the call. `0x8000F168(1, protocol)`
  then announces the registration to the DCMP layer, which goes nowhere
  without a link.
- **close** (`0x8000EBB0`, `fno 2`, block `{socket}`): `-2` unless valid;
  else the protocol halfword is cleared and the answer is `1`.
- **request a send** (`0x800139C0`, `fno 3`, block `{socket, destination
  byte}`): `-2` unless valid. Destination `'H'` (`0x48`, the host) needs
  `0x80023E24` non-zero and `'I'` (`0x49`, the IOP) needs `0x80023E28`
  non-zero, else **`-10`**; neither word is written anywhere on an image with
  no debug station, so both answers are constant here. Any other destination
  takes a free entry of the 32-entry request queue at `0x80024318` (sixteen
  bytes each, free when `+8` is zero; none free, `-11`), records the socket
  and destination, kicks the manager (`0x80010378`, and `0x80013AB0` when
  `0x80023E20` is zero) and answers `1`. Serving the queue -- the handler's
  `DECI2_WRITE`/`WRITEDONE` events -- is the manager's job on later polls.

After boot the table reads, in a `--dump` of the reference on PS2e at
3e9 cycles (`ee_ram.bin` at `0x23E30`):

| index | protocol | option | handler |
| --- | --- | --- | --- |
| 1 | `0x0001` DCMP | 0 | `0x8000F360` |
| 2 | `0x0201` | 0 | `0x80013850` |
| 3 | `0x021F` | 0 | `0x80014000` |
| 4 | `0x0230` | 0 | `0x80012178` |
| 5 | `0x0210` ETTYP | `0x00401250` | `0x0026D5A8` |

Entries 1-4 are the kernel's own manager, registered at its init with
handlers inside the kernel. **Entry 5 is not.** `0x0026D5A8` and
`0x00401250` are addresses inside the reference's `OSDSYS`, which opens the
EE TTY protocol (`0x0210`, `ETTYP`) through `fno 1` -- the four `0x7C` calls
from `pc 0x00254B94` in a syscall trace of the reference boot -- and never
closes it before it launches the disc's program.

## 3. What the title sees, on both kernels

`SLPS-25918` opens `ETTYP` itself: block `{0x0210, 0x003C3210, 0x00179AE8,
0x203C9348}`, from the SDK wrapper at `0x184180`. Read at the instruction
after the `syscall` with the PS2e EE debugger, on the same disc:

| | reference | ours before this note |
| --- | --- | --- |
| `fno 1` open of `0x0210` | **`-3`** | `0x203C9348`, the caller's own `$v0` |
| what the title does next | twelve more opens, all `-3`, then gives up its TTY and runs on | `fno 3` request-send, then `fno 4` poll **for ever** |
| `fno 0x10`, thirteen calls at start-up | `-1` | the caller's own `$v0` |

The title's poll loop (`0x179DA0`) waits for a flag at `+0xC` of its socket
record that only its own handler clears on a write-done event; nothing on
this image delivers one, so with a successful open the title spins on the
poll at priority 38 and every other thread sleeps -- byte-identical thread
tables and RPC counts at 12e9 and 30e9 cycles, 217 RPC calls both times.
With the reference's `-3` the title never sends, and its command stream runs
on to the memory-card polling the reference shows.

So the state that keeps the title off its TTY on a console is **left behind
by the OSD**, not by the kernel: the same title launched with the OSD skipped
(an emulator's fast boot) opens the socket, and that is how an emulator's
console shows its `printf` at all.

## 4. `fno 0x10` always answers `-1`

`0x80010208` computes an answer -- `0` when `0x80023E24` is zero, else a
comparison against the link -- and the dispatch stores it into the frame's
`$v0` (`0x8000EDE0`, in the delay slot of the call to `0x80012540`). Then it
**falls through into the out-of-range path at `0x8000EDE4`**, which stores
`-1` into the same slot. The computed value is never seen; the thirteen calls
the title makes at start-up all read `-1` on the reference, and the title
carries on regardless.

## 5. What this settles for the spec

- SYS-3c's "every sub-function is gated on state only `fno 1` establishes"
  was true of the *invisible* ones; `fno 1`, `2` and `3` answer, and so do
  `0xA`..`0x10`. The rule is now stated per sub-function.
- The protocol table's initial contents, and the search starting at index 2,
  are what make a fresh open answer `5` on a bare kernel and `-3` for
  `ETTYP` after the OSD has run. A rebuild whose OSD does not leave that
  socket open changes what every title that opens `ETTYP` does next.
- The manager behind `fno 3`'s queue and `fno 4`'s poll is the link driver
  for a host that a retail console never has. It is described here only as
  far as its answers; what it would do with a served request is not read.
