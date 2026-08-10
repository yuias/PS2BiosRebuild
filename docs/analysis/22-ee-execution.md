# Executing the EE: the Reference Kernel on a Simulated R5900

Every EE document so far ends with the same caveat. `docs/spec/04-ee-kernel.md`
put it plainly: "everything dynamic … needs an R5900 to execute it", and
`docs/spec/05-ee-syscall-abi.md` repeated it. `tools/iopsim.py` had removed
that caveat for the IOP a long time ago; this removes it for the EE.

`tools/eesim.py` boots the reference image's EE from the reset vector, reaches
the kernel, and then calls its syscalls.

```sh
python3 tools/eesim.py assets/SCPH-50000.bin            # report the boot
python3 tools/eesim.py assets/SCPH-50000.bin --check    # judge it
python3 tools/eesim.py assets/SCPH-50000.bin --syscall 0x14 3
```

## What had to be modelled, and what did not

The useful discovery is how little is needed. There is no GS, no VU, no
DMA controller, no cache and no timing. What the boot actually depends on is:

| Thing | Why it is there |
| --- | --- |
| The R5900 integer core | 128-bit registers, `lq`/`sq`, the 64-bit set, the second multiplier pipeline (`div1`), `eret` |
| RAM, scratchpad, ROM | the reset path runs on the scratchpad before RAM exists (`spec/04` EE-1b) |
| A timer that advances | the reset path waits for `0x10000000` to change; a constant register spins forever |
| `0x1000F010` and `0x1000E010` as **write-to-toggle** | otherwise the enable/disable syscalls of EE-8g appear to work when they do not |
| `0x1000F180` as a byte sink | the ROM and the kernel print to it; see below |
| One identification register | `0x1000F520`, described next |

Everything else answers zero and is recorded, which is enough.

### The kernel talks, and that is the best evidence in the project

`0x1000F180` is the EE's serial transmit register, and the ROM writes to it a
byte at a time after polling `0x1000F130` for readiness. Collecting those bytes
gives the boot's own account of itself:

```
# TLB spad=0 kernel=1:12 default=13:30 extended=31:38
# Initialize Start.
# Initialize GS ...        # Initialize INTC ...      # Initialize TIMER ...
# Initialize DMAC ...      # Initialize VU1 ...       # Initialize VIF1 ...
# Initialize GIF ...       # Initialize VU0 ...       # Initialize VIF0 ...
# Initialize IPU ...       # Initialize FPU ...       # Initialize User Memory ...
# Initialize Scratch Pad ...
# Initialize Done.

EE DECI2 Manager version 0.06 Feb  6 2003 08:38:48
  CPUID=2e20, BoardID=0, ROMGEN=2003-0206, 0M
```

Two things in that are new.

**The initialisation order is not the order the strings are stored in.**
`docs/analysis/15-ee-syscall-groups.md` quoted the messages in image order —
DMAC, VU1, VIF1, GIF, VU0, VIF0, IPU, GS, INTC, TIMER, … — and that is *not*
what a full mask produces. Executed, GS, INTC and TIMER come first. The list in
`spec/04` EE-10 was right about the membership and silent about the order;
the order is now observed rather than assumed.

**The kernel states its own TLB layout.** `spec/05` SYS-7b had to leave open
whether syscall `0x09`'s `Random`-chosen index can overwrite the scratchpad
mapping of EE-1a. The kernel answers: the scratchpad is entry 0, the kernel's
own mappings are 1–12, and the allocatable ranges start at 13. So entry 0 is
protected by the layout, and a rebuild must keep that arrangement.

The trailing `0M` is an artefact of this simulator's one boundary, below.

### The one identification register

`RDRAM` reads `0x1000F520` and looks the value up in a table of 12-byte records
in the ROM at `0x9FC43C80`, terminated by `-1`. A miss is fatal: the reset path
jumps to `0xBFC0093C`, which is `j 0xBFC0093C` — a deliberate hang.

```sh
python3 -c "
d=open('assets/SCPH-50000.bin','rb').read()
print([d[0x43c80+i*12:0x43c80+i*12+4].hex() for i in range(6)])"
```

The simulator reports the first value that table accepts. This is the project's
method applied to a device: what the hardware must answer is derived from what
the reference code accepts, not from outside documentation.

## The boundary: `RDRAM` is called but not executed

`RDRAM` negotiates with the memory controller over a serial protocol
(`0x1000F430`/`0x1000F440`), counts devices and reports a size. The simulator
already has its RAM, and **no requirement in any specification concerns that
protocol** — `spec/04` EE-2 requires only that the call happens, by hard-coded
address. So the call is observed and then returned from as though it had
succeeded.

That is a real limit and it is visible in the output: the boot reports `0M`
because the size it would have measured is the value being stubbed. Everything
downstream is genuinely executed — EE-3's ROMDIR search, the `lq`/`sq` copy of
`KERNEL` to physical 0, the jump to `0x80001000`, the kernel's entire
initialisation, and every syscall exercised below.

## What executing confirms

Each of these was previously a static claim. The command is
`python3 tools/eesim.py assets/SCPH-50000.bin --check`.

| Requirement | Confirmed by |
| --- | --- |
| EE-1 | `Config`, `Status`, `Count` and `Compare` are each first written with the value specified |
| EE-1a | the first `tlbwi` writes index 0 with `EntryHi 0x70000000`, `EntryLo0 0x80000007`, `EntryLo1 7` |
| EE-1b | the stack is inside the scratchpad when `RDRAM` is called |
| EE-2 | `0x9FC41000` is reached by call |
| EE-3b | the vector page in RAM after the copy is non-empty and its `0x000`/`0x180` entries match |
| EE-3d | control reaches `0x80001000` |
| EE-10 | the kernel announces `Initialize Done.` |
| EE-7a, EE-7c | a syscall invoked with the number in `$v1` returns to the instruction *after* the `syscall` — if `EPC` were not advanced it would loop forever |
| EE-7b | calling `-0x14` reaches the same handler as `0x14` |
| EE-8g | enabling an INTC source returns `1`, doing it again returns `0`, and the bit really is set in a **toggle** register; the DMAC pair sets bit `16 + n` |
| SYS-5a | calling `0x74` puts the handler into the syscall table at the specified address |
| SYS-7a | the first two alarms are `0` and `1`, and releasing an unallocated one returns `-1` |
| SYS-7b | a rejected `EntryHi` returns `-1`; an accepted one returns an entry index |
| SYS-7c | `0x71` returns the value it set and `0x70` returns it back from the shadow |
| SYS-3b | `0x75` returns, quietly |

Both reference images pass. They are not the same code on this path:
**SCPH-70000's reset sequence uses `padduw rd, $zero, $zero` to clear the upper
64 bits of four registers**, which SCPH-50000 does not — a difference invisible
to `tools/romdis.py`, since LLVM has no R5900 target, and one that only
surfaced because a simulator had to execute it.

## The gate bites

Four mutations, each failing with the requirement it breaks:

| Mutation | Result |
| --- | --- |
| reset writes `Status = 0x70000000` | `EE-1: Status was written 0x70000000, want 0x70400000` |
| slot `0x14` pointed at the alarm handler | `EE-8g: enabling INTC source 3 returned 0, want 1` |
| slot `0x74` pointed at the empty handler | `SYS-5a: slot 0x40 was not installed into the syscall table` |
| slot `0x09` pointed at the empty handler | `SYS-7b: an accepted TLB write returned -1, want an entry index` |

## What is still not executed

- **The memory controller**, as described above.
- **The boot tail.** `spec/04` EE-9 ends at `rom0:OSDSYS`, which crosses the SIF
  to the IOP. The EE simulator has no IOP behind it and the IOP simulator has no
  EE in front of it, so the handshake completes in neither. Joining the two is
  the obvious next capability and would also settle `spec/02` IRX-12, the
  residency requirement that has been unverified since `docs/analysis/12`.
- **The scheduler in motion.** `spec/04` EE-7g's context switch is executed
  whenever a scheduling syscall is called, but nothing here creates two threads
  and observes one resume in the other's place.

Next: `docs/project-state.md` §5 — implementation, with both simulators now
available to judge it.
