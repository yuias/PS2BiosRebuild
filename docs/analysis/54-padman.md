# PADMAN: How the IOP Reads One Controller and Hands It to the EE

Reference module `PADMAN` (unprefixed, `.iopmod` version **1.14**, export tag
`padman` v1.02, 16 entries; entry `0x2d40`, gp `0xed60`, text `0x6b20`, data
`0x250`, bss `0x1140`). Scope: exactly what a rebuild needs so that an EE
program can read the digital buttons of the controller on **port 0, slot 0**.
Vibration/actuators, pressure mode, analogue-stick modes (beyond where the
bytes sit), multitap, other ports/slots, memory cards, and `XPADMAN`/`TPADMAN`
beyond a one-line comparison are out of scope and were not read.

```sh
python3 tools/romdir.py assets/SCPH-50000.bin --extract <outdir>          # outside the repo
python3 tools/irxinfo.py <outdir>/PADMAN --exports --imports
python3 tools/irxinfo.py <outdir>/PADMAN --dump-load <outdir>/PADMAN.text
python3 tools/romdis.py <outdir>/PADMAN.text --cpu iop --vma 0            # whole module, 0x0-0x6b20
readelf -S <outdir>/PADMAN
xxd -s 0x6b20 -l 0x250 <outdir>/PADMAN.text                               # .rodata/.data as literals
# cross-checks used below
python3 tools/romdis.py <outdir>/SIFCMD.text   --cpu iop --vma 0 --range 0x133c 0x1580
python3 tools/romdis.py <outdir>/THREADMAN.text --cpu iop --vma 0 --range 0x1f00 0x1fb0
python3 tools/irxinfo.py <outdir>/XPADMAN --imports
```

Addresses are `PADMAN.text` offsets (module vaddr, base 0). `.text` is
`0x0`-`0x6b20`, `.rodata` `0x6b20`-`0x6d60` (strings and three jump tables),
`.data` `0x6d60`-`0x6d70` (the export table's name/version header),
`.bss` `0x6d70`-`0x7eb0`. Import stubs are `jr $ra`/`addiu $zero,$zero,ORD`
pairs, so every call target below was identified by reading the ordinal in
the delay slot (`0x6928`-`0x6b20`). Names in brackets are the ordinal names
`docs/analysis/37`, `38`, `40`, `34` and `47` already established for the
imported libraries; every name for something inside `PADMAN` is coined here.
Where a sentence is an inference rather than a reading it says so.

## 0. Cross-references established elsewhere, not re-derived

- `docs/analysis/47` §3: `vblank` 8/9 (`RegisterVblankHandler`/`Release...`),
  callbacks run on the interrupt stack, priority ascending. This reading
  confirms `PADMAN`'s registration is `(list 0, priority 0x10, handler 0x568,
  arg 0x74b8)` and its callback always answers 1. One nuance for 47 §3.4:
  the bracket around the registration is `CpuSuspendIntr` (`intrman` 17) …
  **`CpuEnableIntr` (`intrman` 9)**, not `CpuResumeIntr` (18) — the entry
  leaves interrupts enabled unconditionally. Elsewhere (§3) `PADMAN` does use
  the 17/18 pair.
- `docs/analysis/40` §2/§4: `sio2man` 11 = `sio2_stat70_get`, 23 =
  `sio2_pad_transfer_init` (handshake only), 25 = `sio2_transfer(td)` with the
  `td` layout in §4; `stat6c` is read back into `td+0`.
- `docs/analysis/34` §2/§3: `sifman` 29 is **`sceSifCheckInit`** (the "did
  `sceSifInit` run" latch), 5 is `sceSifInit`, 7 is `sceSifSetDma`; `sifcmd`
  14/17/19/22 = `InitRpc`/`RegisterRpc`/`SetRpcQueue`/`RpcLoop`;
  `sceSifExecRequest` calls `func(fno, buf, size)` and sends back the pointer
  it returns, sized by the client's receive size.
- `docs/analysis/38`: `ReferThreadStatus(0, info)` means "self"; `info+0x4` is
  `option` (copied verbatim from the `CreateThread` parameter block),
  `info+0x8` is `status`, `0x10` = dormant. `ClearEventFlag`'s argument is a
  keep-mask.
- `docs/analysis/37`: `intrman` 9 = `CpuEnableIntr`, 23 = `QueryIntrContext`.

## 1. The entry, traced end to end

`_module_start` (`0x2d40`) never reads `$a0`/`$a1`: both are overwritten
before any use (`0x2d44`), so `argc`/`argv` are ignored. In order:

1. Zero the two words of the **callback record** at `0x74b8` (`+0` "ending"
   flag, `+8` = `0x74c0` "polling enabled" flag; see §1.1).
2. `RegisterLibraryEntries(0x68d0)` — nonzero answer → return 1
   (non-resident), nothing else done.
3. `0x46a0`: zero the key byte of each of 16 slots of the **command-set
   table** (`0x7860`, 16 × `0x48` bytes) and its count (`0x7ce0`). Then ten
   registrations through `0x46d0` (refuses a duplicate key or a 17th entry),
   in this order: `0x5254` key 0, `0x53b0` key 1, `0x5830` key 2, `0x5af0`
   key 3, `0x5500` key 4, `0x59a0` key 5, `0x5c60` key 6, `0x56d8` key 7,
   `0x5df0` key 0xe, `0x6150` key 0xf. The key is compared against the
   **high nibble of the controller's ID byte** (`0x489c`, `0x48f8`, …:
   `andi 0xff; srl 4`). Each entry is `{key, tx-builder, ctrl1(), ctrl2(),
   regdata(), txlen(), rxlen(), config-enter-builder, 9 more builders}` —
   §4 reads the two entries the digital path uses.
4. `0x3f68`: clear the "pending" word of both **transfer-request records**
   (`0x7510`, `0x756c`; stride `0x5c`), and point the SIO2 transfer
   descriptor's `in`/`out` buffers at `0x7660`/`0x7760` (`td+0x74`/`+0x78`).
5. Zero `+0x20`/`+0x24`/`+0x28` of both **port records** (`0x7450`, stride
   `0x34`): open-slot mask, port state, pending request.
6. `CreateEventFlag({attr 2, option 0, init 0})` → `0x74bc` (the **global
   flag**); 0 → return 1.
7. `CreateThread({attr 0x02000000, entry 0x624, stack 0x800, prio 0x20})` →
   `0x74c4`, `StartThread(id, 0)`. This is the **frame scheduler** (§3.1).
8. Same for entry `0x5e0` → `0x74c8`: the **SIO2 runner** (§4.2). Either
   failure → return 1.
9. `CpuSuspendIntr(&s)`; `RegisterVblankHandler(0, 0x10, 0x568, 0x74b8)`
   (start-of-vblank list, priority `0x10`, arg = the callback record);
   `CpuEnableIntr()` (ordinal 9; `s` is passed but ordinal 9 takes nothing).
   The registration's return value is ignored.
10. `0x6808`: create and start the two **RPC threads** (§2), prio `0x15`,
    stack `0x800`, entries `0x66b0` (id → `0x7cf0`) and `0x6774` (id →
    `0x7dd0`). Either failure → return 1.
11. `printf` of a one-line banner carrying a date; return 0 (resident).

So at the end of the entry there are four threads: frame scheduler and SIO2
runner blocked on the global flag, two RPC threads asleep in `RpcLoop`. The
vblank callback is armed but does nothing until a port is opened.

### 1.1 The callback record and the vblank callback (`0x568`)

The record at `0x74b8` — the one 47 §8 left unread — is:

| Off | Field | Written by |
|---|---|---|
| `+0x0` | ending flag (1 = module end requested) | entry (0), `0x2f18` (1) |
| `+0x4` | global event flag id | entry |
| `+0x8` | polling enabled (1 after the first successful port open) | entry (0), `0x3238` (1); **never cleared** |
| `+0xc` | frame-scheduler thread id | entry |
| `+0x10` | SIO2-runner thread id | entry |

Callback (`0x568`, arg = record): if `+0x8 == 1` → `iSetEventFlag(+0x4, 0x1)`.
Then if `+0x0 == 1` → helper `0x78` → if it answers 1, `iSetEventFlag(+0x4,
0x8)`. Always returns 1 (stays registered). The helper `0x78` calls
`ReferThreadStatus` (`thbase` 22, the non-`i` form) on `+0xc` and `+0x10`,
counts those whose `status & 0x10` is clear (not dormant), and answers
`count < 1`. **Confirmed from `THREADMAN.text 0x1f00`**: the non-`i`
`ReferThreadStatus` begins with `QueryIntrContext()` and answers `-100` from
interrupt context, so inside this callback both calls fail, the count stays
0, and the helper answers "both dormant" on the first vblank after an end
request whether or not the threads have exited. (Consequence inferred:
`0x2f18`'s teardown proceeds without waiting; `XPADMAN`/`TPADMAN` import
`thbase` 23 `iReferThreadStatus` where v1.14 does not.) This only affects the
module-end path (§2, fno `0x8000010e`), which is outside the digital read.

### 1.2 The wait helper (`0x0`)

Every wait in the module goes through `0x0(ef, bits, ignored, ignored)`:
`WaitEventFlag(ef, bits | 0x1000, WEF_OR, &res)`; if `res & 0x1000` →
`ReferThreadStatus(0, …)` (result unused), `SetEventFlag(ef, 0x1000)` (so the
next thread sharing the flag also sees it), `ExitThread()`. Otherwise
`ClearEventFlag(ef, ~bits)`. The third argument (`0x10` at every call site)
is overwritten with 1 before the call, so the mode is always OR and the
clear is always manual. Bit `0x1000` is therefore "terminate" on every flag.

## 2. The SIF RPC surface

Each RPC thread (`0x66b0`, `0x6774`) does, in order: `if (!sceSifCheckInit())
{ printf; sceSifInit(); }` — this is what `sifman` 29 is for, nothing more —
then `sceSifInitRpc(0)`, `GetThreadId()`, `sceSifSetRpcQueue(qd, tid)`,
`sceSifRegisterRpc(sd, sid, func, buf, 0, 0, qd)`, `sceSifRpcLoop(qd)`.

| Thread | `qd` | `sd` | `buf` | `sid` | dispatcher | `RpcLoop` arg |
|---|---|---|---|---|---|---|
| `0x66b0` | `0x7cf4` | `0x7d0c` | `0x7d50` (`0x80` bytes to the next object) | **`0x8000010f`** | `0x655c` | `0x7cf4` |
| `0x6774` | `0x7dd4` | `0x7dec` | `0x7e30` (`0x80` bytes to end of `.bss`) | **`0x8000011f`** | `0x6744` | **`0x7cf4`** |

Two things to say plainly.

**The service ids are `0x8000010f` and `0x8000011f`, not `0x80000100`/
`0x80000101`.** `docs/analysis/43` §10/§11 is not wrong about the title: those
two literals are the sids `XPADMAN` v3.06 registers (`XPADMAN.text 0x7828`,
`0x78ec`: `lui $5,0x8000; ori $5,$5,0x100`/`0x101`), and the disc's
`padman.irx` is of that family (`docs/analysis/49` notes the disc's copy
imports `timrman`, which no rom0 `PADMAN` does). In rom0 `PADMAN` v1.14 the
values `0x80000100`-`0x8000010e` are **function codes inside the request**, a
different namespace. An EE program
written against the SDK that talks to the X-family (bind `0x80000100`) will
not find a server on rom0 `PADMAN`; the OSD, if it uses rom0 `PADMAN`, must
bind `0x8000010f`. (Which client binds `0x8000010f` was not traced here —
open question, §5.)

**The second service is registered but never dispatched.** The second thread
calls `RpcLoop` on the **first** queue (`0x67f4`: `addiu $4,$16,-0xe0`, bytes
`20 ff 04 26`, `0x7dd4-0xe0 = 0x7cf4`). `SIFCMD.text 0x1520` (`RpcLoop`) only
ever calls `0x133c` (`GetNextRequest`), which pops `qd+0xc` of the queue it is
given and sleeps when empty; a request for `0x8000011f` is queued on `0x7dd4`
and wakes the second thread, which then polls `0x7cf4` (possibly popping a
first-service request — same handler, harmless) and sleeps again. The
`0x6744` handler (prints, returns the buffer) is unreachable. Confirmed by
static reading of both binaries, not simulated. A client calling
`0x8000011f` would wait for an END packet that never comes.

### 2.1 The dispatcher `0x655c` and the request/reply contract

`func(fno, buf, size)` is called by `SIFCMD`; **`0x655c` ignores `fno` and
reads the function code from `buf[0]`** (`move $16,$5; lw $2,0($16)`). It
computes `buf[0] - 0x80000100`; `< 15` → the 15-entry table at `.rodata
0x6cd0`; otherwise prints a diagnostic and returns `buf` unchanged. Every
case returns `buf`, so **the reply is the request buffer itself**, sent back
with the client's receive size (34 §3). Word offsets below are into that one
buffer; "in" words are what the client filled, "out" is what the handler
stores before returning. Word `n` is byte `4n`: the 6-byte payloads of
`0x80000106`/`0x80000107` and the 12-byte payload of `0x8000010a` all start
at byte `0xc`. Word `0` is never rewritten.

| `buf[0]` | Thunk → body | In (words) | Out | What it is |
|---|---|---|---|---|
| `0x80000100` | `0x626c` → export 4 `0x2fa8` | `[1]` port, `[2]` slot, `[4]` **EE address of the state area** | `[3]` = 1 ok / 0 fail | **open port**, §2.2 |
| `0x80000101` | `0x6688` (default case) | — | none | **no handler in this build**: prints the diagnostic, returns `buf` |
| `0x80000102` | `0x62e0` → export 8 `0x34cc` | `[1]` port, `[2]` slot, `[3]` index, `[4]` kind | `[5]` byte or -1 | actuator info tables at slot `+0x6`/`+0x8..` (out of scope) |
| `0x80000103` | `0x6320` → export 9 `0x35ec` | same shape | `[5]` | combination tables at slot `+0x7`/`+0x18..` (out of scope) |
| `0x80000104` | `0x6360` → export 10 `0x36f8` | `[1]` port, `[2]` slot, `[3]` kind, `[4]` index | `[5]` | mode info: kind 1 → ID high nibble (works for any pad), 2/3/4 → current mode id / index / table at `+0x28` (require slot `+1 >= 2`) |
| `0x80000105` | `0x63a0` → export 11 `0x392c` | `[1]` port, `[2]` slot, `[3]` mode, `[4]` lock | `[5]` 1/0 | request main-mode change: `+0xb8/+0xb9`, port request 4 (state 4 thread) |
| `0x80000106` | `0x63e0` → export 12 `0x3838` | `[1]` port, `[2]` slot, `[3..4]` 6 bytes | `[5]` 1/0 | set actuator values → `+0xba`, `+0xc2 = 6` |
| `0x80000107` | `0x6418` → export 13 `0x3a14` | `[1]`, `[2]`, `[3..4]` 6 bytes | `[5]` 1/0 | set actuator map → `+0xc4`, port request 5 |
| `0x80000108` | `0x6450` → export 14 `0x3d34` | `[1]` port, `[2]` slot | `[3]` u32 | button-mask word from `+0x34..+0x37` |
| `0x80000109` | `0x6488` → export 15 `0x3c10` | `[1]`, `[2]`, `[3]` mask | `[4]` 1/0 | set button-info mask → `+0x30..+0x33`, port request 6 |
| `0x8000010a` | `0x64c4` → `0x3b08` (unexported) | `[1]`, `[2]`, `[3..5]` 12 bytes | `[7]` 1/0 | 12 bytes → `+0x38`, port request 7 |
| `0x8000010b` | `0x64fc` → `0x3df4` | — | `[3]` = 2 | number of ports |
| `0x8000010c` | `0x6528` → `0x3dfc` | `[1]` port | `[3]` = 1 | number of slots on a port (no multitap in this build) |
| `0x8000010d` | `0x62a8` → `0x3274` | `[1]` port, `[2]` slot | `[3]` 1/0 | close port |
| `0x8000010e` | `0x6240` → `0x2f18` | — | `[3]` = 1 | module end (§1.1) |

Both request buffers are `0x80` bytes by layout only (`0x7d50`→`0x7dd0`,
`0x7e30`→`0x7eb0`); `sceSifRegisterRpc` takes no size, so a client that
sends more than `0x80` bytes overruns the second thread's id or the end of
`.bss`.

### 2.2 Open (`0x2fa8`), the only call the digital read needs

`open(port, slot, ee_addr)`: `port < 0` or `port >= 2` → 0. Otherwise, with
`P` = port record `0x7450 + port*0x34` and `S` = slot record `0x6d70 +
port*0x370 + slot*0xdc`:

1. `P+0x20` bit `slot` already set → print, return 0.
2. `S+0xcc = 5` (state), `S+0xce = 0` (request state), `S+0xd0 = 0` (frame
   counter), **`S+0xd4 = ee_addr`** (no alignment or null check), `S+0xb4 = 0`
   (data-valid), `S+0xc2 = 0`, `S+0x52 = 0`.
3. If `P+0x20` was already nonzero (another slot open): set the bit, return
   1 — no threads.
4. Else: `P+0x20 = 1<<slot`, `P+0x24 = 0`, `P+0x28 = 0`;
   `CreateEventFlag({2, ?, 0})` → `P+0x1c` (the **port flag**); six
   `CreateThread({attr 0x02000000, option = port, stack 0x400, prio 0x20})`
   for entries `0x1e7c`, `0x2194`, `0x2620`, `0x27c4`, `0x2968`, `0x2b80`
   stored at `P+0x0`, `+0x4`, `+0xc`, `+0x10`, `+0x14`, `+0x18` — the
   **port state threads**, one per port state 1,2,4,5,6,7. Each learns its
   port from `ReferThreadStatus(0, info)->option`. Any failure → 0.
5. If `P+0x24 < 2`: `0x3fac(port, 0)` sets the transfer-request "pending"
   word (`0x7510 + port*0x5c`) to 1 — **only for slot 0**, any other slot
   answers 0 and is ignored; `StartThread(P+0x4, 0)` (the discovery thread,
   state 2); `P+0x24 = 2`; **`0x74c0 = 1`** (vblank polling on, for good).
   Return 1.

Nothing here touches SIO2; the first transfer happens on the next vblank.

## 3. Direction of the pad-data read: pushed, not polled

**The EE never asks for button state over RPC.** `PADMAN` pushes a `0x40`-byte
**state record** into EE RAM with `sceSifSetDma` (`sifman` 7) once per
vertical blank while slot 0 of the port is the only open slot. The address
is the one the open request carried in `buf[4]`, stored at `S+0xd4`. The
record alternates between the two `0x40`-byte halves of a `0x80`-byte EE
area, so the EE side owns a double buffer and (inferred, not read here — the
EE client is out of scope) picks the half with the larger frame counter.

### 3.1 The frame scheduler thread (`0x624`), one iteration per vblank

1. `0x0(global, 0x1)` — wait for the callback's per-vblank bit.
2. `v = sio2_stat70_get()` (`sio2man` 11); `P0+0x30 = (v>>4)&1`,
   `P1+0x30 = (v>>5)&1`. (What those bits mean is an open question, §5;
   they select register variants in §4 and a one-byte shift in §4.3.)
3. For each port (0, 1), with `Q = sp+0x10 + 4*port` ("queued this frame"):
   `Q = 0`; skip if `P+0x20 == 0`. Then service the pending request
   `P+0x28`: 0 → nothing; 3 → `P+0x24 = 3`, `P+0x28 = 0`, `S0+0xce = 2`,
   `SetEventFlag(P+0x1c, 0x1000)` (kill the six threads); else if `P+0x24
   == 1` → `StartThread(P+0x2c, 0)`, `P+0x24 = P+0x28`, `P+0x28 = 0`,
   `S0+0xce = 2`; else → `P+0x28 = 0`, `S0+0xce = 1` (rejected: not idle).
   Then switch on `P+0x24` (table `0x6bf0`): state 1 → bit `0x1`; 2 → `S0+
   0xb4 = 0`, bit `0x2`; 3 → `S0+0xb4 = 0`, and if all six threads are
   dormant (`0x13c`, thread status via the non-`i` call, which is fine from
   thread context) → `P+0x24 = 0`, `S0+0xce = 0`, `P+0x20 = 0`,
   `SetEventFlag(P+0x1c, 0x400)`, next port; 4/5/6/7 → `S0+0xb4 = 0`, bit
   `0x8/0x10/0x20/0x40`. For states other than 3: `SetEventFlag(P+0x1c, bit)`,
   `WaitEventFlag(P+0x1c, 0x80, OR|CLEAR)` — the state thread has now built
   its transfer request — `Q = 1`.
4. If either `Q` is 1: `SetEventFlag(global, 0x2)`, `0x0(global, 0x4)` — the
   SIO2 runner performs the batch (§4.2) and answers.
5. For each port with `Q == 1`: `SetEventFlag(P+0x1c, 0x100)`,
   `WaitEventFlag(P+0x1c, 0x200, OR|CLEAR)` — the state thread has consumed
   its reply. Then **if `P+0x20 == 1`: `0x2e0(port, 0)`, the push.**
6. Loop to 1.

So per vblank there is at most one SIO2 batch carrying one command per open
port, and one DMA push per port whose open mask is exactly `1`.

### 3.2 The push (`0x2e0(port, slot)`)

If `S+0xd4 == 0`: print several diagnostics, **and clear `P+0x20` to 0** (delay
slot at `0x3e0`) — a null address silently closes the port. Otherwise fill
the **state record** at `0x74d0` (`.bss`, `0x40` bytes; `+0x31..+0x3f` are
never written by this code and stay whatever they were — zero from load):

| Off | Size | Value | Source |
|---|---|---|---|
| `+0x00` | u32 | frame counter **before** increment | `S+0xd0`, then `S+0xd0++` |
| `+0x04` | u8 | slot state | `S+0xcc` low byte: 0 no reply, 2 stable/non-configurable, 5 in progress, 6 stable/configured, 7 error (§4.3) |
| `+0x05` | u8 | request state | `S+0xce` low byte: 0 idle, 1 rejected, 2 in flight |
| `+0x06` | u16 | data valid this frame | `S+0xb4`: 1 only after a good `0x42` reply (§4.3) |
| `+0x08` | 32 B | the **read-out block** from export 6 (`0x33b0`), see below | |
| `+0x28` | u32 | export 6's return: `0x20` if valid, else 0 | |
| `+0x2c` | u8 | port state | `P+0x24` |
| `+0x2d` | u8 | 1 = pad did not enter config mode, 2 = it did | `S+0x1` |
| `+0x2e` | u8 | model byte from the config query, bit 1 = pressure-capable | `S+0x3` |
| `+0x2f` | u8 | stat70 bit `4+port` sampled this vblank | `P+0x30` |
| `+0x30` | u8 | consecutive-error counter, low byte | `S+0xd8` |

Read-out block (export 6, `0x33b0(port, slot, dst)`): if `S+0xb4 != 1` →
`dst[0] = 0xff`, `dst[1..0x1f] = 0`, return 0. Else `dst[0] = 0`, `dst[1] =
S+0x2` (ID byte, `0x41` for a digital pad), `dst[i] = rx[i+1]` for `i =
2..0x1e` where `rx` is the published reply copy at `S+0x94`, `dst[0x1f] =
rx[2]` (the `0x5a` byte); if the ID is `0x12` (a mouse) `dst[2..3] = 0xff`,
`dst[4..5] = 0`. Return `0x20`. Hence, in the pushed record:

- **`+0x0a` = reply byte 3, `+0x0b` = reply byte 4: the digital button
  halfword, exactly as the controller sent it.** `PADMAN` does not decode or
  invert it, and the wire's bits are **active low** -- measured, not inferred:
  §6.2 holds two buttons down and watches the two bytes go from `ff ff` to
  `f7 bf`.
- `+0x0c..+0x0f` = reply bytes 5..8 (the sticks, when the pad is in a 9-byte
  mode; zero for a 5-byte digital reply since the buffer is cleared first).

Then `SifDmaTransfer {src 0x74d0, dest S+0xd4 + (old_frame & 1 ? 0x40 : 0),
size 0x40, attr 0}`; `CpuSuspendIntr`; `sceSifSetDma(&t, 1)` (return
ignored); `CpuResumeIntr`. Even frames land at `+0`, odd at `+0x40`.
Alignment is only diagnosed (`src&3`, `size&3` → print), never enforced.

`0x2e0` is also called immediately by close and by exports 11/13/15 and
`0x3b08`, with the request-state byte already set to 2, so the EE sees "in
flight" without waiting for a vblank.

### 3.3 Where "connected" lives

There is no single connected flag. The EE has to combine: `+0x06 == 1` (a
valid reply this frame), `+0x04` (6 or 2 = stable; 0 = nothing answered on
the last discovery probe; 7 = the last poll failed; 5 = handshake in
progress), and `+0x2c` (port state 1 = steady polling, 2 = discovery). For
"is a digital pad present and readable now": `+0x06 == 1 && +0x09 == 0x41`.

## 4. The SIO2 wire side, for port 0 slot 0

### 4.1 Records the state threads fill

Transfer-request record `R = 0x7510 + port*0x5c`:

| Off | Field | Setter |
|---|---|---|
| `+0x0` | pending (1 = include this port in the batch) | `0x3fac` at open, **never cleared** (`0x3ff8` has no caller) |
| `+0x4` | tx length | `0x44e8` |
| `+0x8` | rx length | `0x45c0` |
| `+0xc` | tx bytes (32) | `0x4530(port, slot, len_or_0, src)` |
| `+0x2c` | rx bytes (32) | filled by the runner; read by `0x4608` |
| `+0x4c` | `port_ctrl1` value | `0x43d0` |
| `+0x50` | `port_ctrl2` value | `0x4418` |
| `+0x54` | `regdata` word (bits 0-1 replaced by the port's index in the batch) | `0x4460` |
| `+0x58` | error flag after the batch (1 = failed) | runner; read by `0x44a8` |

All setters answer 0 and do nothing for `slot != 0`.

### 4.2 The SIO2 runner (`0x5e0` → `0x4040`)

`0x0(global, 0x2)`; `0x4040`; `SetEventFlag(global, 0x4)`; loop. `0x4040`
builds the descriptor `td` at `0x75c8` (layout per 40 §4: `stat6c +0`,
`port_ctrl1[4] +4`, `port_ctrl2[4] +0x14`, `stat70 +0x24`, `regdata[16]
+0x28`, `stat74 +0x68`, `in_size +0x6c`, `out_size +0x70`, `in +0x74`,
`out +0x78`, `in_dma +0x7c`, `out_dma +0x88`):

1. `in_size = out_size = 0`, `in_dma.addr = out_dma.addr = 0`.
2. If `R0+0` is 1: `port_ctrl1[0] = R0+0x4c`, `port_ctrl2[0] = R0+0x50`,
   `regdata[0] = (R0+0x54) & ~3`, append `R0` tx bytes to `in[]`, `out_size
   += R0` rx length. If `R1+0` is 1: the same into index `n` (0 or 1 depending
   on whether port 0 was queued), `port_ctrl1[1]`, `port_ctrl2[1]`,
   `regdata[n] = (R1+0x54 & ~3) | 1`. `regdata[n_total] = 0` terminates.
   Untouched `port_ctrlN[2..3]` keep their previous values (zero from load).
3. `sio2_pad_transfer_init()` (`sio2man` 23); `sio2_transfer(td)` (`sio2man`
   25). Then `out_size = 0` and, per queued port `n`: the port fails if
   **`stat6c` bit 13** is set (then `R+0x58 = 1`, nothing copied) or if
   **`stat6c` bit `16+n`** is set (`0x3e10`); otherwise `R+0x58 = 0` and its
   rx length of bytes are copied from `out[out_size…]` into `R+0x2c`.

So the ordering of the three `sio2man` ordinals per vblank is **11 (in the
scheduler, before any request is built) → 23 → 25 (in the runner)**, and
the runner blocks in 25 until `SIO2MAN`'s thread has seen the IRQ-17
completion (40 §3-§4).

### 4.3 What goes on the wire, in order, from open to steady state

All commands are built by the state threads for slot 0 only. `id` is
`S+0x2`; `K(id)` is the command-set entry whose key is `id >> 4`. Lengths for
the generic path come from `0x4ab4`/`0x4ad4`: `id == 0` is treated as
`0x41`; length `= ((id & 0xf) << 1) + 3`, so `0x41` → 5 and `0x73`/`0xf3` → 9;
`regdata` from `0x4af4` is `(txlen << 8) | 0x40 | (rxlen << 18)` (bits 0-1
are the port, filled by the runner).

**Discovery thread (`0x2194`, state 2, port-flag bit `0x2`).** Resets the slot
record (`+1 = 1`, `+2..+7 = 0`, `+0x30..+0x37 = 0`, `+0xb4 = 0`, `+0xc2 = 0`,
`+0xd8 = 0`). Then:

a. **ID probe** (`0x910`), repeated every vblank until it succeeds, with
   `S+0xcc = 0` while it fails: `K(0)` (entry `0x5254`): tx `01 42 00 00 00`
   (5), rx 5, `port_ctrl1 = 0xffc00505` if `P+0x30 == 0` else `0xff060505`,
   `port_ctrl2 = 0x0002000a` if `P+0x30 == 0` else `0x0002012c`, `regdata =
   0x00140540`. Success = no transfer error **and** `rx[1] != 0` **and** a
   command set exists for `rx[1] >> 4`. Then `S+0x2 = rx[1]`. (`rx[2]` is not
   checked here.)
b. `S+0xcc = 5`. **Enter config mode** (`0xac8`), up to 10 tries: `K(id)`'s
   config-enter builder — for key 4 (`0x54dc`) tx `01 43 00 01 00`, rx 5,
   `port_ctrl1/2` as `K(4)` gives: `0xffc00505`/`0xff060505` and
   **`0x00020014`**/`0x0002012c`, `regdata 0x00140540`. Success = **no SIO2
   transfer error, reply bytes not examined**. On success `S+0x1 = 2`. After
   10 failures: `S+0x2 = id` restored, `P+0x24 = 1`, thread exits — the pad
   is treated as non-configurable and polled as-is (its state byte will read
   2, not 6).
c. In config mode the pad's ID is `0xf3`, so every following command uses
   `K(0xf)` (`0x6150`): 9-byte frames, rx 9, `regdata 0x00240940`,
   `port_ctrl1 0xffc00505`/`0xff060505`, `port_ctrl2 0x00020014`/`0x0002012c`:
   `01 45 00 5a 5a 5a 5a 5a 5a` (`0x10b4`, up to 10 tries; **requires `rx[1]
   == 0xf3`**, stores `rx[3]`→`S+0x3` model, `rx[4]`→`+0x4`, `rx[5]`→`+0x5`,
   `rx[6]`→`+0x6`, `rx[7]`→`+0x7`, the last three capped at 4; after 10
   failures `S+0x2 = 0`, `P+0x24 = 1`, exit — which, because the steady poll
   then sees an ID mismatch, restarts discovery on the next vblank);
   `01 46 00 xx 5a…` per `+0x6`, `01 47 00 xx 5a…` per `+0x7`, `01 4c 00 xx
   5a…` per `+0x4` (out of scope); `01 41 00 5a 5a 5a 5a 5a 5a` (`0x1928` →
   table `+0x34` → `0x6048`: query the button mask; when `rx[8] == 0x5a` it
   stores `rx[3..6]` at `S+0x34..+0x37`, the word fno `0x80000108` returns)
   only if `S+0x1 == 2 && (S+0x3 & 2)`; then **exit config** `01 43 00 00 5a 5a 5a 5a 5a` (`0xc34`); then the
   ID probe (a.) again to re-read the ID. Finally `S+0xcc = 5`, `P+0x24 = 1`,
   exit.

**Steady poll thread (`0x1e7c`, state 1, port-flag bit `0x1`).** Every vblank,
with `id = S+0x2` (`0x41`): `port_ctrl1 = K(id).ctrl1(P+0x30, S+0x3)`,
`port_ctrl2 = K(id).ctrl2(P+0x30)`, `regdata = 0x4af4(id)`, tx/rx lengths from
`0x4ab4`/`0x4ad4`, both 32-byte slot buffers (`S+0x54` tx, `S+0x74` rx)
zeroed, tx built by `K(id)`'s poll builder — for key 4 (`0x5460`):

```
tx: 01 42 00 00 00           port_ctrl1 0xffc00505   port_ctrl2 0x00020014
rx: ff 41 5a BL BH           regdata    0x00140540   (P+0x30 == 0 case)
```

(`P+0x30 == 1` selects `0xff060505` / `0x0002012c` instead.) Bytes `tx[3..]`
are the actuator bytes when `S+0xc2 > 0`, else zero. After the batch:

- transfer error (`R+0x58`): `S+0xd8++`, `S+0xb4 = 0`, `S+0xcc = 7`; after 10
  consecutive errors → `StartThread(discovery)`, `P+0x24 = 2`.
- no error: copy rx into `S+0x74`; if `P+0x30 == 1` apply `0x50c` (shift the
  32 bytes right by one and put `0xff` in front — observed, meaning not
  known); then **`rx[2] != 0x5a` or `rx[1] != id`** → `S+0xb4 = 0`, `S+0xcc =
  5`, restart discovery; else copy the 32 bytes to `S+0x94` (the published
  copy export 6 reads), `S+0xb4 = 1`, `S+0xcc = 2` if `S+0x1 == 1` else 6.

Per-frame validity for an emulator's SIO2 model, then: `stat6c` bit 13 clear
(a device answered), bit `16+n` clear, `rx[0..2] == ff <id> 5a`. The minimal
faithful digital-pad model must: answer `01 42 00 00 00` with `ff 41 5a BL
BH`; either fail the `01 43 00 01 00` frame at the SIO2 level (non-configurable
path, state byte 2) or accept it **and then report ID `0xf3`** to the 9-byte
`0x45`/`0x46`/`0x47`/`0x4c`/`0x43` frames until the exit-config frame (state
byte 6). A model that accepts `0x43` but keeps answering with ID `0x41` never
leaves discovery.

### 4.4 `XPADMAN` / `TPADMAN`, one line

Both are `padman` v1.02 with 19 exports (v3.06 / v2.03), import `sio2man`
50 and the multitap ordinals 55/56/58, `thbase` 23, `sifman` 8 and
`sysmem` 14 in addition, and `XPADMAN` registers sids `0x80000100`/`0x80000101`;
nothing else in them was read.

## 5. What a rebuild must implement, and what is open

For "an EE program reads port 0 slot 0 digital buttons":

1. A `padman` v1.02 export table of 16 entries in the order of §2's table
   (0 entry, 1-3 reserved, 4 open, 5 copy-32 (`0x334c`, returns the raw
   `S+0x54` tx block — not used by the RPC path), 6 read-out block, 7
   `S+0x1`, 8-15 as listed). Only entry 4 and 6 are on the digital path.
2. One RPC service, sid **`0x8000010f`**, function code taken from **request
   word 0**, reply = the request buffer; at minimum fno `0x80000100` (open:
   words 1, 2, 4 in; word 3 out), `0x8000010b`/`0x8000010c` (2 / 1),
   `0x8000010d` (close). Registering `0x8000011f` is optional; the reference
   never serves it.
3. The vblank callback contract of 47 §7 plus: `iSetEventFlag(global, 1)`
   only after the first open; callback answers 1.
4. Per vblank: `sio2_stat70_get`, one `sio2_pad_transfer_init` +
   `sio2_transfer` batch with the §4.2 descriptor, and for the open port a
   `0x40`-byte `sceSifSetDma` of the §3.2 record to `ee_addr + (frame&1)*0x40`,
   from thread context under `CpuSuspendIntr`/`CpuResumeIntr`.
5. The discovery sequence of §4.3 (ID probe → config enter → `0x45` … →
   config exit → ID probe → steady), with its two exits (transfer error on
   `0x43` → state byte 2; `0xf3` seen → state byte 6), and the steady-state
   checks (`rx[2] == 0x5a`, `rx[1] == id`, ten-error restart).
6. Kernel facilities it leans on: `ReferThreadStatus(0)` returning the
   `CreateThread` `option` (the port threads have no other way to learn
   their port); `WaitEventFlag` OR-mode with manual keep-mask clears;
   `CpuEnableIntr` as ordinal 9; `sceSifCheckInit` as `sifman` 29 answering
   nonzero after boot, else the RPC threads call `sceSifInit` again.

Open questions:

- **Who binds `0x8000010f`.** No EE-side client was read in this pass; the
  OSD's own pad reading (if it uses rom0 `PADMAN` rather than `XPADMAN`) is
  where to look.
- **`stat70` bits 4/5** (`0x1F808270`): they pick `0xff060505`/`0x0002012c`
  over `0xffc00505`/`0x00020014` and trigger the one-byte right shift of the
  reply. Whether an emulator should ever raise them is unknown; a model that
  keeps them clear exercises only the first variant.
- **`port_ctrl1`/`port_ctrl2` field meanings** (`0xffc00505`, `0x00020014`,
  `0x0002000a` for the ID-0 probe) — carried as opaque words here.
- **The EE-side selection rule** between the two `0x40` halves (larger frame
  counter, inferred) and whether the EE expects anything in `+0x31..+0x3f`.
- **`0x80000101`'s intended meaning** — reserved in this build; `XPADMAN`
  uses that value as a sid, so the two namespaces may have been merged later.
- The module-end path's race (§1.1) and the never-cleared pending word
  (§4.1) matter only for close/end and port 1; noted, not resolved.

## 6. Two dynamic witnesses

Everything above is static. Two runs back it up, and one of them corrects a
temptation the other creates.

### 6.1 The reference's own bring-up, observed

The sibling `PS2e` project logs each SIO2 sub-transfer with its port, its
length and its first command bytes:

```sh
ps2e --bios <reference> --cycles 3e9 --log 'warn,ps2_core::iop::sio2=trace'
```

Filtered to sub-transfers whose first byte is `0x01`, the reference BIOS's
first seconds give, in order: `01 42 00 00` (length 5) three times, then
`01 43 00 01` (5), `01 45 00 5a` (9), `01 46 00 00` (9), `01 46 00 01` (9),
`01 47 00 00` (9), `01 4c 00 00` (9), `01 4c 00 01` (9), `01 4d 00 ff` (9),
`01 41 00 5a` (9), `01 43 00 00` (9), and then `01 42 00 00` (5) for ever,
alternating port 0 and port 1.

That is §4.3's shape exactly -- ID probe, enter config, interrogate, leave
config, steady poll -- **but it is the shell's module, not this one**. The
reference OSD reaches a pad by rebooting the IOP against the `OSDCNF` archive,
whose boot list appends `XSIO2MAN` and `XPADMAN` (`docs/analysis/10`); there is
no `rom0:PADMAN` string anywhere in the image. So the trace is a witness for
the *protocol* and for the order of its stages, not for v1.14's own frames.
The two differ where §4.4 says they do; `01 4d`, for instance, is the actuator
map, which v1.14 sends from a different state.

### 6.2 Our own `SIO2MAN`, driven for the first time

`src/iop/sio2man.cpp` had never carried a transfer: `IOP-6c`'s gate is that it
reaches its parked state. A throwaway boot-list module filled a `TransferData`
by hand -- `regdata[0] = 5 << 8`, `in = {01 42 00 00 00}`, `in_size` and
`out_size` 5, no DMA arguments -- and called ordinals 23 then 25. On PS2e:

| | idle | with two buttons held |
|---|---|---|
| `stat6c` (RECV1) | `0x00001100` | `0x00001100` |
| `stat70` (RECV2) | `0x0000000f` | `0x0000000f` |
| reply | `ff 41 5a ff ff` | `ff 41 5a f7 bf` |

The buttons held were the ones `--press start` and `--press cross` name. `f7`
clears bit 3 of the low byte and `bf` clears bit 6 of the high byte, which is
bit 14 of the halfword: the two bits those names carry, and both **clear when
pressed**. `0x41` is the digital pad's ID byte, matching `S+0x2` in §4.3, and
`0x5a` is the byte the steady poll checks at `rx[2]`.

Two things this settles and one it does not.

- The FIFO byte path through `sio2_data_out`/`sio2_data_in` completes a
  transfer with **no DMA at all**. That matters: v1.14 imports no `dmacman`,
  so the FIFO path is the only one available to it.
- `SIO2MAN`'s own handshake -- ordinal 23's claim, the service thread's
  prepare/push/start, IRQ 17, the harvest -- works unmodified.
- **It does not license dropping the reference's register words.** The probe
  left `port_ctrl1`/`port_ctrl2` at zero and used a `regdata` of its own, and
  it worked only because the emulator decodes bits 0-1 and 8-17 of `regdata`
  and nothing of the two control words. A rebuild carries §4.3's exact
  `0x00140540`, `0xffc00505` and `0x00020014`, because hardware is not the
  emulator and this analysis has not read what those fields mean (§5).

## Appendix: record layouts referenced above

Port record `P = 0x7450 + port*0x34` (2 ports): `+0..+0x18` six thread ids
(indices 0,1,3,4,5,6 ↔ states 1,2,4,5,6,7; `+0x8` unused), `+0x1c` port
event flag, `+0x20` open-slot mask, `+0x24` state, `+0x28` pending request,
`+0x2c` thread to start for it, `+0x30` stat70 bit.

Slot record `S = 0x6d70 + port*0x370 + slot*0xdc` (4 per port, only slot 0
ever driven): `+0x1` config capability (1/2), `+0x2` ID byte, `+0x3` model,
`+0x4..+0x7` counts from the `0x45` reply, `+0x8..+0x27` and `+0x28..`
actuator/mode tables, `+0x30..+0x33` button-info mask, `+0x34..+0x37`
button-mask reply, `+0x38..+0x43` 12 request bytes, `+0x52` u16 flag set by
state 6, `+0x54` tx (32), `+0x74` rx (32), `+0x94` published rx (32),
`+0xb4` data valid (u32), `+0xb8/+0xb9` requested mode/lock, `+0xba` 6
actuator values, `+0xc2` i16 actuator byte count, `+0xc4` 6-byte actuator
map, `+0xcc` u16 state, `+0xce` u16 request state, `+0xd0` frame counter,
`+0xd4` EE address, `+0xd8` error counter.

Port event-flag bits: `0x1/0x2/0x8/0x10/0x20/0x40` "run state 1/2/4/5/6/7",
`0x80` "request built", `0x100` "reply ready", `0x200` "reply consumed",
`0x400` "closed", `0x1000` terminate. Global flag: `0x1` vblank, `0x2` run
batch, `0x4` batch done, `0x8` end-threads dormant, `0x1000` terminate.
