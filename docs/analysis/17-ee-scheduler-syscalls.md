# EE Syscalls: The Scheduler Group

`docs/analysis/15-ee-syscall-groups.md` noticed a dense run of syscall handlers
between `0x80002FC0` and `0x800035C0`, evenly spaced and all about the same
size. That regularity has a cause: fifteen slots share one skeleton and differ
by a single instruction. This documents the skeleton, what it does on the way
out, and the pairing convention it creates.

## Fifteen handlers, one skeleton

Every handler in the run opens with `jal 0x80003680` and then calls one
operation of its own:

```sh
python3 tools/romdir.py assets/SCPH-50000.bin --extract <outdir>   # outside the repo
python3 tools/romdis.py <outdir>/KERNEL --cpu ee --vma 0x80000000 \
    --range 0x800030c0 0x800030f4
```

```
800030c0  jal   0x80003680        # common prologue
800030c8  jal   0x80003ff0        # <- the only part that varies
800030d0  mtc0  $v0, $14          # EPC = the operation's return value
800030d4  sync.p
800030d8  jal   0x80003800        # common epilogue, yields a stack in $v1
800030dc  move  $sp, $v1
800030e0  mfc0  $k0, $12
800030e4  ori   $k0, $k0, 0x13    # restore the mode bits
800030e8  mtc0  $k0, $12
800030ec  sync.p
800030f0  eret
```

The slots built this way, with the operation each one wraps:

```sh
python3 - <<'PY'
import struct
d = open('<outdir>/KERNEL', 'rb').read()
def jals(off):
    out = []
    for k in range(10):
        w = struct.unpack_from('<I', d, off + k * 4)[0]
        if w >> 26 == 3:
            out.append((w & 0x3FFFFFF) << 2)
    return out
for i in range(0x7d):
    t = struct.unpack_from('<I', d, 0x14f40 + i * 4)[0] & 0x1FFFFFFF
    j = jals(t)
    if len(j) > 1 and j[0] == 0x3680:
        print(f'slot {i:#04x} -> op {j[1]:#07x}')
PY
```

| Slot | Operation | Slot | Operation |
| --- | --- | --- | --- |
| `0x07` | `0x57E0` | `0x2D` | `0x43C8` |
| `0x21` | `0x3EF8` | `0x32` | `0x4650` |
| `0x22` | `0x3F68` | `0x33` | `0x46A8` |
| `0x23` | `0x3FF0` | `0x39` | `0x48B8` |
| `0x24` | `0x4108` | `0x41` | `0x4A40` |
| `0x25` | `0x3DF8` | `0x42` | `0x4BC0` |
| `0x29` | `0x4280` | `0x44` | `0x4CF0` |
| `0x2B` | `0x4380` | | |

Fifteen slots, fifteen distinct operations — the skeleton is shared but nothing
is duplicated.

## The exit is a context switch

Three instructions in that skeleton make it more than a wrapper:

- **`mtc0 $v0, $14`** puts the operation's *return value* into `EPC`. The
  eventual `eret` therefore resumes at whatever address the operation chose,
  not at the instruction after the `syscall`.
- **`jal 0x80003800` followed by `move $sp, $v1`** installs a stack the common
  epilogue supplies, rather than the caller's.
- **`eret`** then returns into that PC with that stack.

So these syscalls do not necessarily return to their caller: they return to
whichever thread the scheduler selected. That is the defining behaviour of a
scheduling call, and it is why the whole group is built from one skeleton —
the switch has to be identical in every case or a thread would resume with a
mismatched stack.

It also explains why `spec/04` EE-7c matters. The syscall entry advances `EPC`
by 4 so an ordinary syscall resumes after itself; these handlers **overwrite**
that value. The rebuild must keep both behaviours: the default advance, and the
scheduler's right to replace it.

## Eight operations are published twice

Several of those operations are *also* the direct target of another syscall
slot. Comparing the two:

```sh
python3 tools/eeksys.py <outdir>/KERNEL --all
```

| Operation | Reschedules | Direct |
| --- | --- | --- |
| `0x3DF8` | `0x25` | `0x26` |
| `0x4280` | `0x29` | `0x2A` |
| `0x4380` | `0x2B` | `0x2C` |
| `0x43C8` | `0x2D` | `0x2E` |
| `0x46A8` | `0x33` | `0x34` |
| `0x48B8` | `0x39` | `0x3A` |
| `0x4A40` | `0x41` | `0x49` |
| `0x4BC0` | `0x42` | `0x43` |

Eight operations, each reachable two ways: through the skeleton, which performs
the context switch, and as a bare handler, which returns to the caller
normally. Seven of the eight pairs are consecutive slot numbers; `0x41`/`0x49`
is the exception.

**This is a different convention from the aliasing of `spec/04` EE-8c**, and
the difference matters for a rebuild:

| | EE-8c aliases | These pairs |
| --- | --- | --- |
| Table entries | identical | different |
| Implementation | one | one operation, two entry paths |
| Can be collapsed? | yes, one function serves both numbers | **no** — they differ in whether they reschedule |

A rebuild that treated these like EE-8c aliases and pointed both numbers at one
handler would make every "direct" call reschedule, or every scheduling call
return to its caller. Neither would fail loudly.

## What this pins for the rebuild

- Fifteen slots share one prologue/epilogue skeleton, and the skeleton must be
  byte-for-byte consistent between them: prologue, operation, `EPC` from the
  return value, epilogue-supplied stack, `Status |= 0x13`, `eret`.
- A scheduling syscall may return to a different thread; the `EPC` advance of
  EE-7c is a default that these handlers deliberately overwrite.
- Eight operations must be published at **two** slot numbers with **two**
  distinct handlers, one rescheduling and one not. They are not aliases and
  must not be collapsed.

Next: the `0x60`–`0x6A` band, including the three slots `spec/04` EE-8e
publishes through KSEG1 (`docs/project-state.md` §5).
