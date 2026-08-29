// REBOOT: the module that answers the EE's `sceSifIopReset`.
//
// docs/analysis/45-iop-reboot.md is the analysis; there is no spec section
// for the reboot yet. On the reference, an EE client that wants a fresher
// IOP module set than `rom0:`'s own `IOPBTCONF` carries sends command id
// `0x80000003` with an argument string naming a loader and an image --
// `"rom0:UDNL cdrom0:\MODULES\IOPRP310.IMG;1"` for a retail title, or
// `"rom0:UDNL rom0:EELOADCNF"` for the boot's own -- and REBOOT hands that
// string, verbatim, to MODLOAD's syscall-12 reboot core, which tears every
// resident module down and lets UDNL merge the named image over rom0's
// archive by highest module version (§2).
//
// **This rebuild does not do that.** What it implements is the wire protocol
// and the hand-back: the packet is parsed as the reference parses it, the
// dispatch-context handler does nothing but record and wake (§1's split, and
// a requirement rather than a style choice -- the handler runs in the SIF
// interrupt), and the worker thread re-arms the receiving channel and raises
// the flags a rebooted IOP raises. No module is torn down, nothing is
// reloaded, and the image the argument names is never opened, so the modules
// running after this "reboot" are the ones that were running before it.
//
// That is a real deviation and it is visible to a client in one way: a title
// rebooting to get a *newer* CDVDMAN/CDVDFSV than `rom0:` holds gets this
// image's own instead. It is deliberate. The merge is the large half of
// analysis 45 and most of that document's open questions live in it, while
// the hand-back is what unblocks the boot -- a title that has sent this
// command spins in `sceSifIopSync` until `SMFLG`'s `SIF_STAT_BOOTEND` comes
// back, and nothing else it does can proceed first.
//
// Why the flags and not just the one bit: the EE's own reset zeroes the
// software registers its RPC layer keeps its addresses in, so the client
// renegotiates `INIT_CMD` from scratch immediately afterwards (§1's "what is
// inferred"), which is the same bring-up `SIF_STAT_SIFINIT` and
// `SIF_STAT_CMDINIT` announce at cold boot. All three are raised, in the
// order the boot raises them. A write from this side can only set bits in
// SMFLG (`spec/03` BOOT-10c), so none of this can erase what the EE has done.

#include "module.hpp"

#include <stdint.h>

namespace {

// §1: `SifCmdResetData_t`, confirmed byte for byte from the reference's own
// handler -- a 16-byte command header, then the argument length, the mode,
// and the argument string.
struct ResetPacket {
    uint32_t size;
    uint32_t dest;
    uint32_t cid;
    uint32_t opt;
    uint32_t arglen;
    uint32_t mode;
    char arg[80];
};

constexpr uint32_t kResetCid = 0x80000003;
constexpr uint32_t kArgBytes = sizeof(ResetPacket::arg);

// BOOT-10/BOOT-12b's own bits, plus the one only a completed boot raises.
constexpr uint32_t kStatSifInit = 0x00010000;
constexpr uint32_t kStatCmdInit = 0x00020000;
constexpr uint32_t kStatBootEnd = 0x00040000;

// Long enough that a client which clears SMFLG *after* sending the packet --
// the order was not observed either way -- has finished clearing before the
// bits go back up, and short enough to be invisible against a real reboot's
// own cost.
constexpr uint32_t kSettleMicroseconds = 20000;

constexpr uint32_t kThreadAttr = 0x02000000;    // TH_C [header]
constexpr uint32_t kThreadPriority = 0x20;
constexpr uint32_t kThreadStack = 0x800;

struct ThreadParameters {
    uint32_t attr;
    uint32_t option;
    void (*entry)(void *);
    uint32_t stack_size;
    uint32_t priority;
};

extern "C" {
int _import_thbase_create(ThreadParameters *parameters);
int _import_thbase_start(uint32_t id, uint32_t arg);
int _import_thbase_get_id();
int _import_thbase_sleep();
int _import_thbase_iwakeup(uint32_t id);
int _import_thbase_delay(uint32_t usec);
int _import_sifcmd_add_cmd_handler(uint32_t cid, void *function, void *arg);
int _import_sifman_set_dchain();
int _import_sifman_set_smflag(uint32_t bits);
}

// What the handler recorded for the worker, exactly the two fields the
// reference's own handler copies out of the packet before posting its event.
struct Request {
    uint32_t thread_id;
    volatile uint32_t pending;
    uint32_t mode;
    char arg[kArgBytes + 1];
};

Request request;

// §1: this runs in the SIF interrupt's own dispatch, so it copies and wakes
// and does nothing else. The argument string is kept even though nothing
// reads it yet -- it is what a merge would be handed, and recording it is
// what makes the deviation above a missing step rather than a missing fact.
void handleReset(void *packet, void *) {
    const auto &reset = *static_cast<const ResetPacket *>(packet);
    uint32_t length = reset.arglen;
    if (length > kArgBytes) {
        length = kArgBytes;
    }
    for (uint32_t k = 0; k < length; k++) {
        request.arg[k] = reset.arg[k];
    }
    request.arg[length] = '\0';
    request.mode = reset.mode;
    request.pending = 1;
    _import_thbase_iwakeup(request.thread_id);
}

void rebootThread(void *) {
    request.thread_id = static_cast<uint32_t>(_import_thbase_get_id());
    _import_sifcmd_add_cmd_handler(kResetCid, reinterpret_cast<void *>(handleReset), nullptr);
    for (;;) {
        _import_thbase_sleep();
        if (request.pending == 0) {
            continue;
        }
        request.pending = 0;
        _import_thbase_delay(kSettleMicroseconds);
        // A reboot discards whatever was in flight on the receiving channel,
        // so it is armed again before anything is announced.
        _import_sifman_set_dchain();
        _import_sifman_set_smflag(kStatSifInit);
        _import_sifman_set_smflag(kStatCmdInit);
        _import_sifman_set_smflag(kStatBootEnd);
    }
}

}  // namespace

PS2_IMPORTS_BEGIN("thbase\0\0", 0x0101)
PS2_IMPORT(_import_thbase_create, 4)
PS2_IMPORT(_import_thbase_start, 6)
PS2_IMPORT(_import_thbase_get_id, 20)
PS2_IMPORT(_import_thbase_sleep, 24)
PS2_IMPORT(_import_thbase_iwakeup, 26)
PS2_IMPORT(_import_thbase_delay, 33)
PS2_IMPORTS_END()

PS2_IMPORTS_BEGIN("sifcmd\0\0", 0x0101)
PS2_IMPORT(_import_sifcmd_add_cmd_handler, 10)
PS2_IMPORTS_END()

PS2_IMPORTS_BEGIN("sifman\0\0", 0x0101)
PS2_IMPORT(_import_sifman_set_dchain, 5)
PS2_IMPORT(_import_sifman_set_smflag, 22)
PS2_IMPORTS_END()

extern "C" {

// §1: the entry's only job is to spawn the worker; the registration happens
// on that thread, not here.
int _module_start(int, char **) {
    ThreadParameters parameters;
    parameters.attr = kThreadAttr;
    parameters.option = 0;
    parameters.entry = rebootThread;
    parameters.stack_size = kThreadStack;
    parameters.priority = kThreadPriority;
    const int id = _import_thbase_create(&parameters);
    if (id < 0) {
        return 1;
    }
    _import_thbase_start(static_cast<uint32_t>(id), 0);
    return 0;                                   // resident
}

}  // extern "C"
