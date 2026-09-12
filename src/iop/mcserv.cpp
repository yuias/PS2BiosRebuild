// MCSERV: the EE's way in to the card driver.
//
// docs/spec/06-iop-kernel.md IOP-15h, from docs/analysis/55 §5.2. One thread,
// one service, and a dispatcher that does nothing but turn an `fno` into a
// call on `MCMAN`. Everything about a card is the driver's; this is the wire.
//
// Scoped with the driver: the two entry points a shell needs to ask "what is
// in this slot" and "what is in its root". The other fifteen the reference
// serves are the title's, and reach driver entry points this rebuild does not
// have, so they answer a refusal rather than a wrong success.

#include "module.hpp"
#include "sif.hpp"
#include "sifrpc.hpp"

#include <stdint.h>

namespace {

using ps2::module::ExportTable;
using ps2::module::reservedHook;
using ps2::module::slot;
using ps2::sif::Transfer;
using ps2::sifrpc::Queue;
using ps2::sifrpc::Server;

constexpr uint32_t kSid = 0x80000400;

// IOP-15h: the table starts here and runs seventeen entries.
constexpr uint32_t kFnoBase = 0x70;
constexpr uint32_t kFnoCount = 17;
constexpr uint32_t kFnoGetDir = 0x76;
constexpr uint32_t kFnoSlotInfo = 0x78;

constexpr uint32_t kThreadAttr = 0x02000000;    // TH_C [header]
constexpr uint32_t kThreadStack = 0x1000;
constexpr uint32_t kThreadPriority = 0x68;

// The request area the reference gives the layer, and the record size both
// entry points move.
constexpr uint32_t kRequestBytes = 0x418;
constexpr uint32_t kRecordBytes = 0x40;

// A refusal that is not a plausible count or card code.
constexpr int32_t kNotServed = -5;

struct ThreadParameters {
    uint32_t attr;
    uint32_t option;
    void (*entry)(void *);
    uint32_t stack_size;
    uint32_t priority;
};

}  // namespace

extern "C" {
int _import_loadcore_register(void *table);
int _import_intrman_suspend(uint32_t *state);
int _import_intrman_resume(uint32_t state);
int _import_thbase_create(ThreadParameters *parameters);
int _import_thbase_start(uint32_t id, uint32_t arg);
int _import_thbase_get_id();
int _import_thbase_delay(uint32_t microseconds);
int _import_sifcmd_init_rpc(uint32_t mode);
int _import_sifcmd_register_rpc(Server *server, uint32_t sid, void *function, void *buffer,
                                void *cfunction, void *cbuffer, Queue *queue);
int _import_sifcmd_set_rpc_queue(Queue *queue, uint32_t thread_id);
int _import_sifcmd_rpc_loop(Queue *queue);
int _import_sifman_init();
int _import_sifman_set_dma(const Transfer *list, uint32_t count);
int _import_sifman_dma_stat(uint32_t id);
int _import_sifman_check_init();
int _import_stdio_printf(const char *format, ...);
int _import_mcman_detect(int port, int slot);
int _import_mcman_type(int port);
int _import_mcman_getdir(int port, int slot, const char *path, int mode, int max,
                         void *out);
}

namespace {

uint32_t resident;
Queue queue;
Server server;
alignas(16) uint8_t request[kRequestBytes];
alignas(16) uint8_t record[kRecordBytes];
int32_t answer;

void sendToEe(uint32_t ee_address, const void *from, uint32_t bytes) {
    Transfer transfer;
    transfer.src = reinterpret_cast<uintptr_t>(from);
    transfer.dest = ee_address;
    transfer.size = bytes;
    transfer.attr = 0;
    uint32_t state;
    _import_intrman_suspend(&state);
    const int id = _import_sifman_set_dma(&transfer, 1);
    _import_intrman_resume(state);
    // The reference polls the transfer's own status with a short sleep
    // between tries rather than returning while it is still in flight, and
    // the reply packet that follows would otherwise overtake the record.
    for (uint32_t attempt = 0; id != 0 && attempt < 1000; attempt++) {
        if (_import_sifman_dma_stat(static_cast<uint32_t>(id)) < 0) {
            break;
        }
        (void)_import_thbase_delay(100);
    }
}

[[nodiscard]] uint32_t word(uint32_t index) {
    return *reinterpret_cast<const uint32_t *>(request + index * 4);
}

void clearRecord() {
    for (uint32_t i = 0; i < kRecordBytes; i++) {
        record[i] = 0;
    }
}

// IOP-15h's `0x78`: detect, then the two optional questions, then a record.
[[nodiscard]] int32_t serveSlotInfo() {
    const int port = static_cast<int>(word(1));
    const int slot = static_cast<int>(word(2));
    const int32_t found = _import_mcman_detect(port, slot);
    clearRecord();
    if (word(3) != 0) {
        *reinterpret_cast<uint32_t *>(record) =
            static_cast<uint32_t>(_import_mcman_type(port));
    }
    // The free-cluster count is the one question this rebuild's driver has no
    // answer to: counting it means walking the allocation table, which is the
    // write path's arithmetic. The field stays zero and the record says so
    // nowhere, so a client that needs it must not read this one.
    if (word(7) != 0) {
        sendToEe(word(7), record, kRecordBytes);
    }
    return found;
}

// IOP-15h's `0x76`: one entry per driver call, each record sent on its own.
[[nodiscard]] int32_t serveGetDir() {
    const int port = static_cast<int>(word(0));
    const int slot = static_cast<int>(word(1));
    int mode = static_cast<int>(word(2));
    const uint32_t wanted = word(3);
    uint32_t ee_address = word(4);
    const char *path = reinterpret_cast<const char *>(request + 0x14);
    if (ee_address == 0) {
        return 0;
    }
    uint32_t sent = 0;
    while (sent < wanted) {
        if (_import_mcman_getdir(port, slot, path, mode, 1, record) != 1) {
            break;
        }
        mode = 1;                               // anything but zero continues
        sendToEe(ee_address, record, kRecordBytes);
        ee_address += kRecordBytes;
        sent++;
    }
    return static_cast<int32_t>(sent);
}

// IOP-15h: the reply is a fixed word holding the driver's return, not the
// request buffer -- which is what makes this service unlike every other one
// in the archive.
void *serve(uint32_t fno, void *, uint32_t) {
    if (fno - kFnoBase >= kFnoCount) {
        answer = kNotServed;
        return &answer;
    }
    switch (fno) {
    case kFnoSlotInfo:
        answer = serveSlotInfo();
        break;
    case kFnoGetDir:
        answer = serveGetDir();
        break;
    default:
        answer = kNotServed;
        break;
    }
    return &answer;
}

void serverThread(void *) {
    if (_import_sifman_check_init() == 0) {
        (void)_import_sifman_init();
    }
    _import_sifcmd_init_rpc(0);
    _import_sifcmd_set_rpc_queue(&queue, static_cast<uint32_t>(_import_thbase_get_id()));
    _import_sifcmd_register_rpc(&server, kSid, reinterpret_cast<void *>(serve),
                                request, nullptr, nullptr, &queue);
    _import_sifcmd_rpc_loop(&queue);
}

extern "C" int _module_start(int, char **);

PS2_EXPORT_TABLE ExportTable<8> mcserv_exports = {
    ps2::module::kExportMagic,
    0,
    0x0101,
    0,
    {'m', 'c', 's', 'e', 'r', 'v', 0, 0},
    {
        slot(_module_start),            // 0
        slot(reservedHook),             // 1
        slot(reservedHook),             // 2
        slot(reservedHook),             // 3
        slot(reservedHook),             // 4
        slot(reservedHook),             // 5
        slot(reservedHook),             // 6
        slot(reservedHook),             // 7
        nullptr,
    },
};

}  // namespace

PS2_IMPORTS_BEGIN("loadcore", 0x0101)
PS2_IMPORT(_import_loadcore_register, 6)
PS2_IMPORTS_END()

PS2_IMPORTS_BEGIN("intrman\0", 0x0102)
PS2_IMPORT(_import_intrman_suspend, 17)
PS2_IMPORT(_import_intrman_resume, 18)
PS2_IMPORTS_END()

PS2_IMPORTS_BEGIN("thbase\0\0", 0x0101)
PS2_IMPORT(_import_thbase_create, 4)
PS2_IMPORT(_import_thbase_start, 6)
PS2_IMPORT(_import_thbase_get_id, 20)
PS2_IMPORT(_import_thbase_delay, 33)
PS2_IMPORTS_END()

PS2_IMPORTS_BEGIN("sifman\0\0", 0x0101)
PS2_IMPORT(_import_sifman_init, 5)
PS2_IMPORT(_import_sifman_set_dma, 7)
PS2_IMPORT(_import_sifman_dma_stat, 8)
PS2_IMPORT(_import_sifman_check_init, 29)
PS2_IMPORTS_END()

PS2_IMPORTS_BEGIN("sifcmd\0\0", 0x0101)
PS2_IMPORT(_import_sifcmd_init_rpc, 14)
PS2_IMPORT(_import_sifcmd_register_rpc, 17)
PS2_IMPORT(_import_sifcmd_set_rpc_queue, 19)
PS2_IMPORT(_import_sifcmd_rpc_loop, 22)
PS2_IMPORTS_END()

PS2_IMPORTS_BEGIN("stdio\0\0\0", 0x0102)
PS2_IMPORT(_import_stdio_printf, 4)
PS2_IMPORTS_END()

PS2_IMPORTS_BEGIN("mcman\0\0\0", 0x0101)
PS2_IMPORT(_import_mcman_detect, 5)
PS2_IMPORT(_import_mcman_getdir, 12)
PS2_IMPORT(_import_mcman_type, 39)
PS2_IMPORTS_END()

extern "C" {

int _module_start(int, char **) {
    if (_import_loadcore_register(&mcserv_exports) < 0 || resident != 0) {
        return 1;
    }
    resident = 1;

    ThreadParameters parameters;
    parameters.attr = kThreadAttr;
    parameters.option = 0;
    parameters.entry = serverThread;
    parameters.stack_size = kThreadStack;
    parameters.priority = kThreadPriority;
    const int id = _import_thbase_create(&parameters);
    if (id < 0 || _import_thbase_start(static_cast<uint32_t>(id), 0) < 0) {
        return 1;
    }
    _import_stdio_printf("MCSERV: card service\n");
    return 0;                                   // resident
}

}  // extern "C"
