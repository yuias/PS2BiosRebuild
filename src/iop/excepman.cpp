// EXCEPMAN: the IOP's exception vector and the handler chains behind it.
//
// docs/spec/06-iop-kernel.md IOP-1. The R3000's general exception vector is
// at physical 0x80, and what this module installs there saves what the
// handlers will need, reads `Cause`'s ExcCode and jumps through a sixteen-
// entry table of chain heads. A handler is a structure its owner keeps --
// `{ next, info, funccode... }` -- and registration (IOP-1b) does not build
// a table for the vector to walk at exception time: it rewrites each
// handler's own `next` word to the next handler's code, so a chain is
// entered by one jump and continued by `jr` through those words, falling
// through at its end to a default chain every cause shares.

#include "module.hpp"

#include <stdint.h>

namespace {

using ps2::module::ExportTable;
using ps2::module::reservedHook;
using ps2::module::slot;

constexpr uint32_t kCauses = 16;
constexpr uint32_t kNodes = 32;                 // IOP-1d: the reference's pool

// IOP-1c: where the installed vector keeps what it saved, and the table it
// jumps through. These are low memory below the first module (the boot list
// places it at 0x800), at the addresses the reference uses, so that the
// vector's absolute loads and stores are the reference's too.
constexpr uintptr_t kVectorAddress = 0x00000080;
constexpr uintptr_t kSavedAt = 0x00000400;
constexpr uintptr_t kSavedEpc = 0x00000404;
constexpr uintptr_t kSavedStatus = 0x00000408;
constexpr uintptr_t kSavedCause = 0x0000040C;
constexpr uintptr_t kCauseTable = 0x00000440;

constexpr int kErrorBadCause = -0x32;           // IOP-1b
constexpr int kErrorLinked = -0x34;

// A handler as its owner keeps it (IOP-1a): `next` is rewritten by this
// module, `info` is the owner's, and the code begins at `funccode`.
struct Handler {
    uint32_t next;
    uint32_t info;
    uint32_t funccode[1];
};

// The bookkeeping this module keeps per registration, in insertion order
// within a cause, best priority first.
struct Node {
    Node *next;
    Handler *handler;
    uint32_t priority;
};

Node nodes[kNodes];
Node *free_nodes;
Node *chain[kCauses];
Handler *default_chain;                         // IOP-1a ordinal 6's list
bool pool_ready;

[[nodiscard]] uint32_t *causeTable() {
    return reinterpret_cast<uint32_t *>(kCauseTable);
}

[[nodiscard]] uint32_t codeOf(const Handler *handler) {
    return reinterpret_cast<uintptr_t>(handler->funccode);
}

void preparePool() {
    if (pool_ready) {
        return;
    }
    pool_ready = true;
    free_nodes = nullptr;
    for (uint32_t k = kNodes; k-- > 0;) {
        nodes[k].next = free_nodes;
        free_nodes = &nodes[k];
    }
}

// IOP-1b: the `next` words. A cause's chain runs from the table entry
// through each handler's `next` to the default chain, and the default chain
// ends where nothing is registered at all: a handler of this module's own
// that stops the machine where it can be seen.
extern "C" void _excepman_unhandled();
extern "C" Handler _excepman_last;

void rebuild() {
    // The default chain is linked through `next` by ordinal 6 as it grows,
    // ending at the stop; a cause's chain falls through to its head.
    const uint32_t fall_through = default_chain != nullptr
                                      ? codeOf(default_chain)
                                      : codeOf(&_excepman_last);
    for (uint32_t cause = 0; cause < kCauses; cause++) {
        Node *node = chain[cause];
        if (node == nullptr) {
            causeTable()[cause] = fall_through;
            continue;
        }
        causeTable()[cause] = codeOf(node->handler);
        for (; node != nullptr; node = node->next) {
            node->handler->next =
                node->next != nullptr ? codeOf(node->next->handler) : fall_through;
        }
    }
}

// Ordinal 5, IOP-1b: a chain executes the highest priority number first.
int registerPriority(uint32_t cause, uint32_t priority, Handler *handler) {
    preparePool();
    if (cause >= kCauses) {
        return kErrorBadCause;
    }
    if (handler->next != 0) {
        return kErrorLinked;
    }
    if (free_nodes == nullptr) {
        return -1;
    }
    Node *node = free_nodes;
    free_nodes = node->next;
    node->handler = handler;
    node->priority = priority & 3;

    Node **link = &chain[cause];
    while (*link != nullptr && (*link)->priority >= node->priority) {
        link = &(*link)->next;
    }
    node->next = *link;
    *link = node;
    rebuild();
    return 0;
}

int registerHandler(uint32_t cause, Handler *handler) {
    return registerPriority(cause, 2, handler);
}

// Ordinal 6: the cause-independent chain, newest first.
int registerDefault(Handler *handler) {
    preparePool();
    if (handler->next != 0) {
        return kErrorLinked;
    }
    handler->next = default_chain != nullptr ? codeOf(default_chain)
                                             : codeOf(&_excepman_last);
    default_chain = handler;
    rebuild();
    return 0;
}

int releaseHandler(uint32_t cause, Handler *handler) {
    preparePool();
    if (cause >= kCauses) {
        return kErrorBadCause;
    }
    for (Node **link = &chain[cause]; *link != nullptr; link = &(*link)->next) {
        if ((*link)->handler == handler) {
            Node *node = *link;
            *link = node->next;
            node->next = free_nodes;
            free_nodes = node;
            handler->next = 0;
            rebuild();
            return 0;
        }
    }
    return -1;
}

int releaseDefault(Handler *handler) {
    preparePool();
    if (default_chain == handler) {
        const uint32_t following = handler->next;
        default_chain = following == codeOf(&_excepman_last)
                            ? nullptr
                            : reinterpret_cast<Handler *>(following - 8);
        handler->next = 0;
        rebuild();
        return 0;
    }
    return -1;
}

// Ordinal 3: the address of the cell holding the table's base.
uint32_t table_base = kCauseTable;

uint32_t *getTable() {
    return &table_base;
}

// IRX-4: the export table.
[[gnu::used]] ExportTable<9> excepman_exports = {
    ps2::module::kExportMagic,
    0,
    0x0101,
    0,
    {'e', 'x', 'c', 'e', 'p', 'm', 'a', 'n'},
    {
        slot(reservedHook),             // 0
        slot(reservedHook),             // 1
        slot(reservedHook),             // 2
        slot(getTable),                 // 3  GetExHandlersTable
        slot(registerHandler),          // 4  RegisterExceptionHandler
        slot(registerPriority),         // 5  RegisterPriorityExceptionHandler
        slot(registerDefault),          // 6  RegisterDefaultExceptionHandler
        slot(releaseHandler),           // 7  ReleaseExceptionHandler
        slot(releaseDefault),           // 8  ReleaseDefaultExceptionHandler
        nullptr,                        // IRX-5b
    },
};

// IOP-1c: the vector, copied to 0x80 at start. It is assembled here and
// copied rather than linked there because this module is relocatable and the
// vector is not: every address in it is absolute. `$k0` is the one register
// an exception may use freely; `$at` is saved first so the handlers may use
// it too.
asm(
    ".pushsection .text\n"
    ".set noreorder\n"
    ".set noat\n"
    ".align 2\n"
    ".globl _excepman_vector\n"
    "_excepman_vector:\n"
    "sw   $at, 0x400($zero)\n"
    "mfc0 $k0, $14\n"                   // EPC
    "nop\n"
    "sw   $k0, 0x404($zero)\n"
    "mfc0 $k0, $12\n"                   // Status
    "nop\n"
    "sw   $k0, 0x408($zero)\n"
    "mfc0 $k0, $13\n"                   // Cause
    "nop\n"
    "sw   $k0, 0x40c($zero)\n"
    "andi $k0, $k0, 0x3c\n"             // ExcCode * 4
    "lw   $k0, 0x440($k0)\n"            // the chain head
    "nop\n"
    "jr   $k0\n"
    "nop\n"
    ".globl _excepman_vector_end\n"
    "_excepman_vector_end:\n"
    // The end of every chain: nothing handled the exception. Stop where a
    // debugger can see it, with the saved registers intact.
    ".globl _excepman_last\n"
    ".align 2\n"
    "_excepman_last:\n"
    ".word 0\n"                         // next: nothing follows
    ".word 0\n"                         // info
    ".globl _excepman_unhandled\n"
    "_excepman_unhandled:\n"
    "1:\n"
    "b    1b\n"                         // a local label: no fixup (IRX-3)
    "nop\n"
    ".set at\n"
    ".set reorder\n"
    ".popsection\n"
);

}  // namespace

extern "C" {

extern const uint32_t _excepman_vector[];
extern const uint32_t _excepman_vector_end[];

// IRX-12: the entry. Installs the vector and an empty table -- every cause
// falls through to the stop -- and stays resident.
int _module_start(int, char **) {
    preparePool();
    for (uint32_t cause = 0; cause < kCauses; cause++) {
        chain[cause] = nullptr;
    }
    default_chain = nullptr;
    rebuild();

    auto *to = reinterpret_cast<uint32_t *>(kVectorAddress);
    for (const uint32_t *from = _excepman_vector; from < _excepman_vector_end;
         from++, to++) {
        *to = *from;
    }
    // The copy is instructions: make sure the cache does not hold the old
    // bytes of the vector page. The R3000's I-cache is invalidated by a
    // `mtc0` Status swap in the reference's flush routine; on the emulators
    // this image runs on, a plain store is seen by the next fetch, and the
    // flush is LOADCORE ordinal 4's business once it exists (IOP-5a).
    return 0;                           // resident
}

}  // extern "C"
