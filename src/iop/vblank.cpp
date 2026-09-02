// VBLANK: the two vertical-blank interrupt lines, and the callbacks on them.
//
// docs/spec/06-iop-kernel.md IOP-10, from docs/analysis/47. The module owns
// I_STAT bits 0 and 11 -- the start and the end of vertical blank -- and turns
// each into a priority-ordered list of callbacks run from the interrupt
// itself, plus one event flag whose four bits let a thread wait for an edge or
// test the current level. It creates no thread of its own: everything happens
// either in the interrupt or in the caller.

#include "module.hpp"

#include <stdint.h>

namespace {

using ps2::module::ExportTable;
using ps2::module::reservedHook;
using ps2::module::slot;

constexpr uint32_t kIrqStart = 0x0;             // I_STAT bit 0
constexpr uint32_t kIrqEnd = 0xB;               // I_STAT bit 11
constexpr uint32_t kEventMulti = 2;             // EA_MULTI [header]
constexpr uint32_t kWaitOr = 1;                 // WEF_OR [header]

// IOP-10b: one pool for both lists, and the module's own two callbacks come
// out of it, so a client has fourteen.
constexpr uint32_t kHandlers = 16;
constexpr uint16_t kNone = 0xFFFF;
constexpr int32_t kOwnPriority = 0x80;

// kerr.h [header]
constexpr int kOk = 0;
constexpr int kIllegalContext = -100;
constexpr int kFoundHandler = -104;
constexpr int kNotFoundHandler = -105;
constexpr int kNoMemory = -400;

// IOP-10d: two pulses, set and cleared inside one interrupt, and two levels
// that hold until the other edge.
constexpr uint32_t kBitStart = 0x1;
constexpr uint32_t kBitInside = 0x2;
constexpr uint32_t kBitEnd = 0x4;
constexpr uint32_t kBitOutside = 0x8;

// The bit VBLANK raises once, on the first vertical blank after boot. No
// module in the reference archive waits on it (docs/analysis/47 §8); it is
// raised because the reference raises it.
constexpr uint32_t kSystemFirstVblank = 0x200;

struct EventParameters {
    uint32_t attr;
    uint32_t option;
    uint32_t bits;
};

extern "C" {
int _import_loadcore_register(void *table);
int _import_intrman_register(uint32_t irq, uint32_t mode, int (*handler)(void *), void *arg);
int _import_intrman_enable(uint32_t irq);
int _import_intrman_suspend(uint32_t *state);
int _import_intrman_resume(uint32_t state);
int _import_intrman_query_context();
int _import_thbase_system_status_flag();
int _import_thevent_create(const EventParameters *parameters);
int _import_thevent_iset(uint32_t id, uint32_t bits);
int _import_thevent_clear(uint32_t id, uint32_t keep);
int _import_thevent_wait(uint32_t id, uint32_t bits, uint32_t mode, uint32_t *result);
}

using Handler = int (*)(void *arg);

struct Record {
    uint16_t next;
    uint16_t previous;
    int32_t priority;
    Handler handler;
    void *arg;
};

// A list is a head index and nothing else; a record's `next` chains it and
// `kNone` ends it. The reference uses self-sentinelled circular lists, which
// is the same list with a different empty test.
struct List {
    uint16_t head;
};

Record records[kHandlers];
List lists[2];                                  // 0 start, 1 end
List free_list;
uint32_t event;
uint32_t started;                               // dispatches on the start line

[[nodiscard]] List &listOf(int startend) {
    return lists[startend != 0 ? 1 : 0];        // IOP-10b: anything but 0 is "end"
}

void unlink(List &list, uint16_t index) {
    Record &record = records[index];
    if (record.previous == kNone) {
        list.head = record.next;
    } else {
        records[record.previous].next = record.next;
    }
    if (record.next != kNone) {
        records[record.next].previous = record.previous;
    }
    record.next = kNone;
    record.previous = kNone;
}

void insertBefore(List &list, uint16_t index, uint16_t before) {
    Record &record = records[index];
    record.next = before;
    if (before == kNone) {
        uint16_t tail = kNone;
        for (uint16_t at = list.head; at != kNone; at = records[at].next) {
            tail = at;
        }
        record.previous = tail;
        if (tail == kNone) {
            list.head = index;
        } else {
            records[tail].next = index;
        }
        return;
    }
    record.previous = records[before].previous;
    if (record.previous == kNone) {
        list.head = index;
    } else {
        records[record.previous].next = index;
    }
    records[before].previous = index;
}

// --- the two ordinals (IOP-10b) ----------------------------------------------

int registerHandler(int startend, int32_t priority, Handler handler, void *arg) {
    if (_import_intrman_query_context()) {
        return kIllegalContext;                 // before any bracket is opened
    }
    uint32_t state;
    _import_intrman_suspend(&state);
    List &list = listOf(startend);
    for (uint16_t at = list.head; at != kNone; at = records[at].next) {
        if (records[at].handler == handler) {
            _import_intrman_resume(state);
            return kFoundHandler;               // identity is (list, handler)
        }
    }
    if (free_list.head == kNone) {
        _import_intrman_resume(state);
        return kNoMemory;
    }
    // Ahead of the first entry of greater priority, so ties stay behind and a
    // lower number runs first.
    uint16_t before = kNone;
    for (uint16_t at = list.head; at != kNone; at = records[at].next) {
        if (records[at].priority > priority) {
            before = at;
            break;
        }
    }
    const uint16_t index = free_list.head;
    unlink(free_list, index);
    records[index].priority = priority;
    records[index].handler = handler;
    records[index].arg = arg;
    insertBefore(list, index, before);
    _import_intrman_resume(state);
    return kOk;
}

int releaseHandler(int startend, Handler handler) {
    if (_import_intrman_query_context()) {
        return kIllegalContext;
    }
    uint32_t state;
    _import_intrman_suspend(&state);
    List &list = listOf(startend);
    for (uint16_t at = list.head; at != kNone; at = records[at].next) {
        if (records[at].handler == handler) {
            unlink(list, at);
            insertBefore(free_list, at, free_list.head);
            _import_intrman_resume(state);
            return kOk;
        }
    }
    _import_intrman_resume(state);
    return kNotFoundHandler;
}

// --- the dispatch (IOP-10c) ---------------------------------------------------

// A record's successor is taken before its callback runs, so a callback that
// unregisters itself -- which is what answering 0 does -- cannot lose the walk.
void runList(List &list) {
    uint16_t at = list.head;
    while (at != kNone) {
        const uint16_t following = records[at].next;
        if (records[at].handler(records[at].arg) == 0) {
            unlink(list, at);
            insertBefore(free_list, at, free_list.head);
        }
        at = following;
    }
}

int startInterrupt(void *) {
    if (started == 0) {
        _import_thevent_iset(static_cast<uint32_t>(_import_thbase_system_status_flag()),
                             kSystemFirstVblank);
    }
    started++;
    runList(lists[0]);
    return 1;                                   // IOP-10c: keeps the line enabled
}

int endInterrupt(void *) {
    runList(lists[1]);
    return 1;
}

// IOP-10d: set the edge's pulse and its level, then drop the pulse and the
// other edge's level. The waiters were released at the set, so a caller
// arriving afterwards waits for the next edge.
int pulseStart(void *) {
    _import_thevent_iset(event, kBitStart);
    _import_thevent_iset(event, kBitInside);
    _import_thevent_clear(event, ~(kBitStart | kBitOutside));
    return 1;
}

int pulseEnd(void *) {
    _import_thevent_iset(event, kBitEnd);
    _import_thevent_iset(event, kBitOutside);
    _import_thevent_clear(event, ~(kBitInside | kBitEnd));
    return 1;
}

// --- the waits (IOP-10d) ------------------------------------------------------

int waitFor(uint32_t bits) {
    return _import_thevent_wait(event, bits, kWaitOr, nullptr);
}

int waitVblankStart() { return waitFor(kBitStart); }
int waitVblankEnd() { return waitFor(kBitEnd); }
int waitVblank() { return waitFor(kBitInside); }
int waitNonVblank() { return waitFor(kBitOutside); }

// Ordinal 3: the module's own state, the shape `GetThreadmanData` has.
void *internalData() {
    return &event;
}

[[gnu::used]] ExportTable<10> vblank_exports = {
    ps2::module::kExportMagic,
    0,
    0x0101,
    0,
    {'v', 'b', 'l', 'a', 'n', 'k', 0, 0},
    {
        slot(reservedHook),             // 0  the entry, in the reference
        slot(reservedHook),             // 1
        slot(reservedHook),             // 2
        slot(internalData),             // 3
        slot(waitVblankStart),          // 4
        slot(waitVblankEnd),            // 5
        slot(waitVblank),               // 6
        slot(waitNonVblank),            // 7
        slot(registerHandler),          // 8  RegisterVblankHandler
        slot(releaseHandler),           // 9  ReleaseVblankHandler
        nullptr,
    },
};

}  // namespace

PS2_IMPORTS_BEGIN("loadcore", 0x0101)
PS2_IMPORT(_import_loadcore_register, 6)
PS2_IMPORTS_END()

PS2_IMPORTS_BEGIN("intrman\0", 0x0102)
PS2_IMPORT(_import_intrman_register, 4)
PS2_IMPORT(_import_intrman_enable, 6)
PS2_IMPORT(_import_intrman_suspend, 17)
PS2_IMPORT(_import_intrman_resume, 18)
PS2_IMPORT(_import_intrman_query_context, 23)
PS2_IMPORTS_END()

PS2_IMPORTS_BEGIN("thbase\0\0", 0x0101)
PS2_IMPORT(_import_thbase_system_status_flag, 41)
PS2_IMPORTS_END()

PS2_IMPORTS_BEGIN("thevent\0", 0x0101)
PS2_IMPORT(_import_thevent_create, 4)
PS2_IMPORT(_import_thevent_iset, 7)
PS2_IMPORT(_import_thevent_clear, 9)
PS2_IMPORT(_import_thevent_wait, 10)
PS2_IMPORTS_END()

extern "C" {

// IOP-10a, in the reference's order. The whole of it runs with interrupts
// held off, and a library that will not register ends it before anything else
// exists -- no event flag, no interrupt handler, not resident.
int _module_start(int, char **) {
    uint32_t state;
    _import_intrman_suspend(&state);
    if (_import_loadcore_register(&vblank_exports) < 0) {
        _import_intrman_resume(state);
        return 1;
    }
    for (uint16_t k = 0; k < kHandlers; k++) {
        records[k].next = kNone;
        records[k].previous = kNone;
    }
    lists[0].head = kNone;
    lists[1].head = kNone;
    free_list.head = kNone;
    for (uint16_t k = kHandlers; k > 0; k--) {
        insertBefore(free_list, k - 1, free_list.head);
    }

    EventParameters parameters;
    parameters.attr = kEventMulti;
    parameters.option = 0;
    parameters.bits = 0;
    event = static_cast<uint32_t>(_import_thevent_create(&parameters));

    // Through its own ordinal, at a priority behind any client's, so a client
    // callback sees the edge before the flag's bits move.
    (void)registerHandler(0, kOwnPriority, pulseStart, nullptr);
    (void)registerHandler(1, kOwnPriority, pulseEnd, nullptr);

    _import_intrman_register(kIrqStart, 1, startInterrupt, nullptr);
    _import_intrman_register(kIrqEnd, 1, endInterrupt, nullptr);
    _import_intrman_enable(kIrqStart);
    _import_intrman_enable(kIrqEnd);
    _import_intrman_resume(state);
    return 0;                                   // resident
}

}  // extern "C"
