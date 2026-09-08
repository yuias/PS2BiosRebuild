// Interrupt handlers and their delivery: spec/05 SYS-12.
//
// docs/spec/05-ee-syscall-abi.md SYS-12, from docs/analysis/35 and 16. Slots
// 0x10-0x13 keep a list of handlers per INTC cause and per DMAC channel; the
// interrupt entry in syscall.S pushes the interrupted registers on the
// interrupt stack and calls `interruptDispatch` with that frame, which reads
// the controllers, acknowledges what is pending before any handler runs, calls
// the handlers, and lets the scheduler rewrite the frame if a handler made a
// better thread ready (SYS-12c).

#include <stdint.h>

namespace {

constexpr uint32_t kIntcCauses = 15;      // 0..14; cause 1 is refused (SYS-12a)
constexpr uint32_t kDmacChannels = 16;
constexpr uint32_t kHandlers = 64;        // records shared by both kinds
constexpr uint16_t kNone = 0xFFFF;

constexpr uintptr_t kIntcStat = 0xB000F000;
constexpr uintptr_t kIntcMask = 0xB000F010;
constexpr uintptr_t kDmacStat = 0xB000E010;   // low half status, high half mask

// Cause's IP bits, where the vector already agreed the source is.
constexpr uint32_t kIpIntc = 1u << 10;
constexpr uint32_t kIpDmac = 1u << 11;

using Handler = int (*)(int cause, void *arg);

struct Record {
    Handler handler;
    void *arg;
    uint32_t gp;
    uint32_t id;            // 0 when the record is free
    uint16_t next;
};

Record records[kHandlers];
uint16_t intc_head[kIntcCauses];
uint16_t dmac_head[kDmacChannels];
uint32_t next_id = 1;
bool lists_ready = false;

void prepareLists() {
    if (lists_ready) {
        return;
    }
    lists_ready = true;
    for (auto &head : intc_head) {
        head = kNone;
    }
    for (auto &head : dmac_head) {
        head = kNone;
    }
}

[[nodiscard]] uint32_t readWord(uintptr_t address) {
    return *reinterpret_cast<volatile uint32_t *>(address);
}

void writeWord(uintptr_t address, uint32_t value) {
    *reinterpret_cast<volatile uint32_t *>(address) = value;
}

// SYS-12a: a new handler goes to the head of its list; ids come from one
// counter and are the caller's only handle on the installation.
[[nodiscard]] int32_t install(uint16_t &head, Handler handler, void *arg) {
    prepareLists();
    for (uint16_t k = 0; k < kHandlers; k++) {
        if (records[k].id == 0) {
            uint32_t gp;
            asm volatile("move %0, $gp" : "=r"(gp));
            records[k] = {handler, arg, gp, next_id, head};
            head = k;
            return static_cast<int32_t>(next_id++);
        }
    }
    return -1;
}

[[nodiscard]] int32_t remove(uint16_t &head, uint32_t id) {
    prepareLists();
    uint16_t *link = &head;
    while (*link != kNone) {
        Record &record = records[*link];
        if (record.id == id) {
            *link = record.next;
            record.id = 0;
            return static_cast<int32_t>(id);
        }
        link = &record.next;
    }
    return -1;
}

// SYS-12b: every handler on the list, youngest first; the return values are
// not consulted, as the reference's dispatch does not consult them.
// SYS-12b: a handler is entered with the `$gp` its installer had -- which is
// what SYS-12a keeps that value for, and what the reference's own dispatch
// does (`docs/analysis/35`: "+0x0C=$gp (caller's, so the handler runs with the
// installer's gp)") -- and on a **quadword-aligned stack**.
//
// The alignment is not housekeeping. `sq` ignores the low four bits of its
// address, so a handler that saves its frame with one -- and a compiled EE
// handler does, its registers being 128 bits wide -- has its stores land up to
// eight bytes below where it put them if it was entered on a merely
// doubleword-aligned `$sp`. It then reads its own frame back at the offsets it
// wrote, finds them shifted, and acts on the wrong words. Nothing faults.
//
// Written as assembly because the argument registers, the two swaps and the
// call have to happen in one piece. `$16` and `$17` are callee-saved, so they
// carry the kernel's own `$gp` and `$sp` across the call; naming them as
// clobbers is what makes the compiler preserve them for whoever called us.
void callHandlers(uint16_t head, int cause) {
    for (uint16_t at = head; at != kNone; at = records[at].next) {
        const Record &record = records[at];
        register int argument asm("$4") = cause;
        register void *installed asm("$5") = record.arg;
        register Handler target asm("$25") = record.handler;
        register uint32_t handler_gp asm("$8") = record.gp;
        asm volatile(
            "move  $16, $gp\n\t"
            "move  $17, $sp\n\t"
            "addiu $3, $zero, -16\n\t"
            "and   $sp, $sp, $3\n\t"
            "move  $gp, %[gp]\n\t"
            "jalr  %[target]\n\t"
            "nop\n\t"
            "move  $sp, $17\n\t"
            "move  $gp, $16"
            : "+r"(argument), "+r"(installed), [target] "+r"(target),
              [gp] "+r"(handler_gp)
            :
            : "$2", "$3", "$6", "$7", "$9", "$10", "$11", "$12", "$13", "$14",
              "$15", "$16", "$17", "$24", "$31", "memory");
    }
}

// The lowest set bit's index, for taking pending sources one at a time.
[[nodiscard]] uint32_t lowestBit(uint32_t bits) {
    uint32_t index = 0;
    while ((bits & 1) == 0) {
        bits >>= 1;
        index++;
    }
    return index;
}

}  // namespace

extern "C" {

// thread.cpp: SYS-12c, the reschedule check on the way out.
void clearRescheduleRequest();
bool interruptReschedule(uint32_t *frame);
extern uint32_t _interrupt_depth;

// The interrupt entry's C++ half. `frame` is the pushed register frame, EPC in
// slot 0 and Status in slot 26; the entry restores from it afterwards.
void interruptDispatch(uint32_t *frame) {
    prepareLists();
    // A handler that enables interrupts -- the SDK's SIF command handler
    // does, first thing -- lets another interrupt nest inside it, and only
    // the outermost exit may switch threads (SYS-12c). So only the outermost
    // entry starts a fresh request: a nested one clearing it would drop a
    // wakeup the outer handler had already asked for.
    if (_interrupt_depth == 1) {
        clearRescheduleRequest();
    }
    uint32_t cause;
    uint32_t status;
    asm volatile("mfc0 %0, $13" : "=r"(cause));
    status = frame[1];                          // beside EPC; syscall.S's SLOT_STATUS
    const uint32_t pending = cause & status;

    if (pending & kIpIntc) {
        uint32_t stat = readWord(kIntcStat) & readWord(kIntcMask);
        while (stat != 0) {
            const uint32_t source = lowestBit(stat);
            const uint32_t bit = 1u << source;
            writeWord(kIntcStat, bit);          // acknowledged before the handlers
            if (source < kIntcCauses) {
                callHandlers(intc_head[source], static_cast<int>(source));
            }
            stat &= ~bit;
        }
    }
    if (pending & kIpDmac) {
        const uint32_t both = readWord(kDmacStat);
        uint32_t stat = both & (both >> 16) & 0xFFFF;
        while (stat != 0) {
            const uint32_t channel = lowestBit(stat);
            const uint32_t bit = 1u << channel;
            writeWord(kDmacStat, bit);          // write-to-clear the status bit
            if (channel < kDmacChannels) {
                callHandlers(dmac_head[channel], static_cast<int>(channel));
            }
            stat &= ~bit;
        }
    }
    // Other IPs -- the timers -- have no handlers yet; nothing to acknowledge
    // that a program has asked for.

    // SYS-12c: only the outermost interrupt may switch threads; a nested one
    // returns to the handler it interrupted. A handler may have left
    // interrupts enabled (the SDK's SIF command handler does), so close them
    // here, before the request is read: an interrupt nesting between this
    // check and the `eret` would wake a thread that nothing then switches
    // to, and the next outermost entry would clear its request -- the
    // machine idles with runnable threads. The exit restores the interrupted
    // context's Status, so the interrupted code is not affected.
    // `di` and `sync.p`, which the assembler does not know for this target.
    asm volatile(".word 0x42000039\n\t.word 0x0000040f" ::: "memory");
    if (_interrupt_depth == 1) {
        interruptReschedule(frame);
    }
}

// Slot 0x10, SYS-12a: (cause, handler, next, arg) -> id | -1.
int32_t sysAddIntcHandler(uint32_t cause, Handler handler, int32_t next, void *arg)
    asm("_sys_add_intc_handler");
int32_t sysAddIntcHandler(uint32_t cause, Handler handler, int32_t next, void *arg) {
    (void)next;
    if (cause >= kIntcCauses || cause == 1) {
        return -1;
    }
    return install(intc_head[cause], handler, arg);
}

// Slot 0x11: (cause, id) -> id | -1.
int32_t sysRemoveIntcHandler(uint32_t cause, uint32_t id) asm("_sys_remove_intc_handler");
int32_t sysRemoveIntcHandler(uint32_t cause, uint32_t id) {
    if (cause >= kIntcCauses) {
        return -1;
    }
    return remove(intc_head[cause], id);
}

// Slot 0x12, SYS-12a: (channel, handler, next, arg) -> id | -1.
int32_t sysAddDmacHandler(uint32_t channel, Handler handler, int32_t next, void *arg)
    asm("_sys_add_dmac_handler");
int32_t sysAddDmacHandler(uint32_t channel, Handler handler, int32_t next, void *arg) {
    (void)next;
    if (channel >= kDmacChannels) {
        return -1;
    }
    return install(dmac_head[channel], handler, arg);
}

// Slot 0x13: (channel, id) -> id | -1.
int32_t sysRemoveDmacHandler(uint32_t channel, uint32_t id) asm("_sys_remove_dmac_handler");
int32_t sysRemoveDmacHandler(uint32_t channel, uint32_t id) {
    if (channel >= kDmacChannels) {
        return -1;
    }
    return remove(dmac_head[channel], id);
}

}  // extern "C"
