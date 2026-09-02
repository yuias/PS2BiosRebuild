// SECRMAN: the card-authentication interface, without the authentication.
//
// docs/spec/06-iop-kernel.md IOP-11, from docs/analysis/48. A memory-card
// driver imports `secrman` ordinals 4, 5 and 6, and an importer of a tag
// nothing exports is refused at link time -- so the library has to exist even
// though docs/clean-room-policy.md puts the mechanism behind it out of scope.
// What is reproduced here is the interface: two handler slots, and an
// `AuthCard` whose answer is 0 or 1 and nothing else.
//
// This module deliberately performs no exchange with the card or the
// mechacon, which is why it imports neither `cdvdman` nor `sio2man` where the
// reference imports the first (docs/implementation.md).

#include "module.hpp"

#include <stdint.h>

namespace {

using ps2::module::ExportTable;
using ps2::module::reservedHook;
using ps2::module::slot;

extern "C" {
int _import_loadcore_register(void *table);
}

// IOP-11b: the transport a card driver registers, and the device-id helper
// beside it. Both are pointers we keep and never validate -- NULL is what a
// driver passes when it unloads.
using CommandHandler = int (*)(int port, int slot, void *descriptor);
using DeviceIdHandler = int (*)(int port, int slot);

CommandHandler command_handler;
DeviceIdHandler device_id_handler;

void setMcCommandHandler(CommandHandler handler) {
    command_handler = handler;
}

void setMcDevIdHandler(DeviceIdHandler handler) {
    device_id_handler = handler;
}

// IOP-11c. The reference runs an exchange between the card and the mechacon
// and answers 1 only if every step of it succeeded. We do not run it, so the
// one honest distinction left is the one that does not depend on the
// exchange: a driver that never registered its transport could not have
// authenticated anything, and is told so.
int authCard(int, int, int) {
    return command_handler != nullptr ? 1 : 0;
}

// The reset pair the reference runs on its own failure paths, answering the
// mechacon's result. Nothing observed imports it; with no exchange to undo,
// there is nothing for it to fail at.
int resetAuthCard(int, int, int) {
    return 1;
}

// Ordinal 2 answers 1 in the reference and has no caller anywhere
// (docs/analysis/48 §7).
int unknownTwo() {
    return 1;
}

// The six entries of the encrypted-module loading path. They are out of
// scope, and no client this project has met imports them; they stay in the
// table so that ordinals 4 to 7 keep their places.
int notBuilt() {
    return -1;
}

[[gnu::used]] ExportTable<14> secrman_exports = {
    ps2::module::kExportMagic,
    0,
    0x0103,
    0,
    {'s', 'e', 'c', 'r', 'm', 'a', 'n', 0},
    {
        slot(reservedHook),             // 0  the entry, in the reference
        slot(reservedHook),             // 1
        slot(unknownTwo),               // 2
        slot(reservedHook),             // 3
        slot(setMcCommandHandler),      // 4  SecrSetMcCommandHandler
        slot(setMcDevIdHandler),        // 5  SecrSetMcDevIDHandler
        slot(authCard),                 // 6  SecrAuthCard
        slot(resetAuthCard),            // 7  SecrResetAuthCard
        slot(notBuilt),                 // 8  SecrCardBootHeader
        slot(notBuilt),                 // 9  SecrCardBootBlock
        slot(notBuilt),                 // 10 SecrCardBootFile
        slot(notBuilt),                 // 11 SecrDiskBootHeader
        slot(notBuilt),                 // 12 SecrDiskBootBlock
        slot(notBuilt),                 // 13 SecrDiskBootFile
        nullptr,
    },
};

}  // namespace

PS2_IMPORTS_BEGIN("loadcore", 0x0101)
PS2_IMPORT(_import_loadcore_register, 6)
PS2_IMPORTS_END()

extern "C" {

// IOP-11d: register, clear the slots, stay. The reference also hands MODLOAD
// its encrypted-module callbacks and ignores the answer; with no such path
// here there is nothing to hand over.
int _module_start(int, char **) {
    if (_import_loadcore_register(&secrman_exports) < 0) {
        return 1;
    }
    command_handler = nullptr;
    device_id_handler = nullptr;
    return 0;                                   // resident
}

}  // extern "C"
