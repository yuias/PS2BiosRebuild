// PADMAN: the controller driver, and the record it pushes to the EE.
//
// docs/spec/06-iop-kernel.md IOP-14, from docs/analysis/54. What makes this
// module unlike every other service in the archive: an EE client opens a port
// once over RPC and then never asks again. The buttons arrive in its own
// memory, a 0x40-byte record written once per vertical blank into alternating
// halves of a 0x80-byte area, and the RPC service exists only to start and
// stop that.
//
// IOP-14h: the reference reaches this with ten threads and two event flags.
// This does it with two -- one driving the frame, one serving the RPC --
// because what IOP-14 pins is the frames on the wire, the cadence and the
// record, and not the shape behind them.

#include "module.hpp"
#include "sif.hpp"
#include "sifrpc.hpp"

#include <stdint.h>

namespace {

using ps2::module::ExportTable;
using ps2::module::reservedHook;
using ps2::module::slot;
using ps2::sif::Transfer;

// IOP-14a. The values 0x80000100..0x8000010e are function codes inside the
// request, not service ids; the id is this one.
constexpr uint32_t kSid = 0x8000010f;

constexpr uint32_t kThreadAttr = 0x02000000;    // TH_C [header]
constexpr uint32_t kThreadStack = 0x800;
constexpr uint32_t kDriverPriority = 0x20;
constexpr uint32_t kRpcPriority = 0x15;

constexpr uint32_t kEventMulti = 2;             // EA_MULTI [header]
constexpr uint32_t kWaitOr = 1;                 // WEF_OR [header]
constexpr uint32_t kBitVblank = 0x1;

// IOP-14d step 2: the start-of-blank list, at the reference's priority.
constexpr int kVblankStart = 0;
constexpr int32_t kVblankPriority = 0x10;

// IOP-14b's function codes, as offsets from the first.
constexpr uint32_t kCodeBase = 0x80000100;
constexpr uint32_t kCodeOpen = 0x80000100;
constexpr uint32_t kCodePortCount = 0x8000010b;
constexpr uint32_t kCodeSlotCount = 0x8000010c;
constexpr uint32_t kCodeClose = 0x8000010d;
constexpr uint32_t kCodeCount = 15;

constexpr uint32_t kRequestWords = 0x80 / 4;

// IOP-14g: the record, and the area that holds two of them.
constexpr uint32_t kRecordBytes = 0x40;

// IOP-14e. `regdata` is (tx << 8) | 0x40 | (rx << 18) with the port index in
// bits 0-1, which the batch fills in; both frame lengths this driver sends
// are precomputed rather than derived, so that a wrong length is a compile
// error rather than a silent short frame.
constexpr uint32_t kCtrl1 = 0xffc00505;
constexpr uint32_t kCtrl2Probe = 0x0002000a;    // only the first ID probe
constexpr uint32_t kCtrl2 = 0x00020014;
constexpr uint32_t kRegdata5 = 0x00140540;
constexpr uint32_t kRegdata9 = 0x00240940;

// IOP-14d step 4: the status word's verdict on a batch.
constexpr uint32_t kStatusBatchFailed = 1u << 13;
constexpr uint32_t kStatusPortFailed = 1u << 16;    // for index 0

constexpr uint8_t kIdDigital = 0x41;
constexpr uint8_t kIdConfig = 0xf3;
constexpr uint8_t kReplyMagic = 0x5a;
constexpr uint32_t kRetries = 10;

// docs/analysis/40 §4: what sio2man ordinal 25 takes.
struct DmaArgument {
    uint32_t address;
    uint32_t size;
    uint32_t count;
};

struct TransferData {
    uint32_t stat6c;
    uint32_t port_ctrl1[4];
    uint32_t port_ctrl2[4];
    uint32_t stat70;
    uint32_t regdata[16];
    uint32_t stat74;
    uint32_t in_size;
    uint32_t out_size;
    uint8_t *in;
    uint8_t *out;
    DmaArgument in_dma;
    DmaArgument out_dma;
};

struct ThreadParameters {
    uint32_t attr;
    uint32_t option;
    void (*entry)(void *);
    uint32_t stack_size;
    uint32_t priority;
};

struct EventParameters {
    uint32_t attr;
    uint32_t option;
    uint32_t bits;
};

}  // namespace

extern "C" {
int _import_loadcore_register(void *table);
int _import_intrman_enable_all();
int _import_intrman_suspend(uint32_t *state);
int _import_intrman_resume(uint32_t state);
int _import_stdio_printf(const char *format, ...);
int _import_thbase_create(ThreadParameters *parameters);
int _import_thbase_start(uint32_t id, uint32_t arg);
int _import_thbase_get_id();
int _import_thevent_create(const EventParameters *parameters);
int _import_thevent_set(uint32_t id, uint32_t bits);
int _import_thevent_iset(uint32_t id, uint32_t bits);
int _import_thevent_clear(uint32_t id, uint32_t keep);
int _import_thevent_wait(uint32_t id, uint32_t bits, uint32_t mode, uint32_t *result);
uint32_t _import_sio2man_stat70_get();
void _import_sio2man_pad_transfer_init();
int _import_sio2man_transfer(TransferData *td);
int _import_sifcmd_init_rpc(uint32_t mode);
int _import_sifcmd_register_rpc(ps2::sifrpc::Server *server, uint32_t sid, void *function,
                                void *buffer, void *cfunction, void *cbuffer,
                                ps2::sifrpc::Queue *queue);
int _import_sifcmd_set_rpc_queue(ps2::sifrpc::Queue *queue, uint32_t thread_id);
int _import_sifcmd_rpc_loop(ps2::sifrpc::Queue *queue);
int _import_sifman_set_dma(const Transfer *list, uint32_t count);
int _import_sifman_check_init();
int _import_sifman_init();
int _import_vblank_register(int startend, int32_t priority, int (*handler)(void *), void *arg);
}

namespace {

// IOP-14f's sequence, after the controller has been put into the
// configuration mode. Nine bytes each, because the controller answers to the
// ID 0xf3 while it is in that mode. Sent for its own sake: this driver reads
// only the model byte out of the first reply, but a controller left partway
// through never leaves the mode and its ID never matches again.
constexpr uint8_t kConfigFrames[][9] = {
    {0x01, 0x45, 0x00, 0x5a, 0x5a, 0x5a, 0x5a, 0x5a, 0x5a},
    {0x01, 0x46, 0x00, 0x00, 0x5a, 0x5a, 0x5a, 0x5a, 0x5a},
    {0x01, 0x47, 0x00, 0x00, 0x5a, 0x5a, 0x5a, 0x5a, 0x5a},
    {0x01, 0x4c, 0x00, 0x00, 0x5a, 0x5a, 0x5a, 0x5a, 0x5a},
    {0x01, 0x41, 0x00, 0x5a, 0x5a, 0x5a, 0x5a, 0x5a, 0x5a},
    {0x01, 0x43, 0x00, 0x00, 0x5a, 0x5a, 0x5a, 0x5a, 0x5a},
};
constexpr uint32_t kConfigFrameCount = sizeof kConfigFrames / sizeof kConfigFrames[0];
constexpr uint32_t kFrameModel = 0;             // which one answers with it
constexpr uint32_t kFrameMask = kConfigFrameCount - 2;
constexpr uint32_t kFrameExit = kConfigFrameCount - 1;

// IOP-14g's slot-state byte.
constexpr uint8_t kSlotSilent = 0;
constexpr uint8_t kSlotPlain = 2;
constexpr uint8_t kSlotHandshake = 5;
constexpr uint8_t kSlotConfigured = 6;
constexpr uint8_t kSlotFailed = 7;

enum class Stage {
    ProbeId,
    EnterConfig,
    ConfigFrames,
    ReprobeId,
    Poll,
};

uint32_t resident;
uint32_t event;

// The one port this driver serves. IOP-14 is scoped to port 0 slot 0, and a
// second port would need a second set of these and a second batch slot; the
// RPC answers the counts the reference answers, so a client that asks for
// port 1 is told there is one and then refused at open.
struct Port {
    bool open;
    uint32_t ee_address;                        // IOP-14c: taken as given
    uint32_t frame;
    Stage stage;
    uint32_t frame_index;                       // within kConfigFrames
    uint32_t attempts;
    uint8_t id;
    uint8_t model;
    uint8_t configurable;                       // IOP-14g's +0x2d
    uint8_t slot_state;
    uint8_t errors;
    bool valid;                                 // IOP-14g's +0x06
    uint8_t reply[9];
    uint8_t reply_length;
    uint8_t status_bit;                         // IOP-14g's +0x2f
};

Port port;

TransferData td;
uint8_t td_in[16];
uint8_t td_out[16];

alignas(4) uint8_t record[kRecordBytes];

ps2::sifrpc::Queue queue;
ps2::sifrpc::Server server;
alignas(16) uint32_t request[kRequestWords];

void clearRecord() {
    for (uint32_t i = 0; i < kRecordBytes; i++) {
        record[i] = 0;
    }
}

// --- the serial side (IOP-14d, IOP-14e) ------------------------------------

// One frame to port 0 and back. `true` when the batch and the port both
// answered; the reply bytes are then in `port.reply`.
[[nodiscard]] bool exchange(const uint8_t *bytes, uint32_t length, uint32_t ctrl2) {
    for (uint32_t i = 0; i < 4; i++) {
        td.port_ctrl1[i] = 0;
        td.port_ctrl2[i] = 0;
    }
    for (uint32_t i = 0; i < 16; i++) {
        td.regdata[i] = 0;
        td_in[i] = 0;
        td_out[i] = 0;
    }
    for (uint32_t i = 0; i < length; i++) {
        td_in[i] = bytes[i];
    }
    td.port_ctrl1[0] = kCtrl1;
    td.port_ctrl2[0] = ctrl2;
    // Bits 0-1 are the port's index in the batch, which is 0 with one port.
    td.regdata[0] = length == 9 ? kRegdata9 : kRegdata5;
    td.stat6c = 0;
    td.stat70 = 0;
    td.stat74 = 0;
    td.in = td_in;
    td.out = td_out;
    td.in_size = length;
    td.out_size = length;
    td.in_dma.address = 0;
    td.out_dma.address = 0;

    _import_sio2man_pad_transfer_init();
    (void)_import_sio2man_transfer(&td);

    port.reply_length = static_cast<uint8_t>(length);
    for (uint32_t i = 0; i < length; i++) {
        port.reply[i] = td_out[i];
    }
    return (td.stat6c & (kStatusBatchFailed | kStatusPortFailed)) == 0;
}

// IOP-14e: a reply is the controller's only when the magic byte and the ID
// both stand. Checking one without the other is how a driver ends up reading
// a configuration-mode answer as buttons.
[[nodiscard]] bool answered(uint8_t expected_id) {
    return port.reply[2] == kReplyMagic && port.reply[1] == expected_id;
}

constexpr uint8_t kPollFrame[5] = {0x01, 0x42, 0x00, 0x00, 0x00};
constexpr uint8_t kEnterConfigFrame[5] = {0x01, 0x43, 0x00, 0x01, 0x00};

// One frame's worth of the state machine, run once per vertical blank.
void advance() {
    switch (port.stage) {
    case Stage::ProbeId: {
        port.slot_state = kSlotSilent;
        if (!exchange(kPollFrame, sizeof kPollFrame, kCtrl2Probe)
            || port.reply[1] == 0 || port.reply[2] != kReplyMagic) {
            return;                             // keep probing every blank
        }
        port.id = port.reply[1];
        port.slot_state = kSlotHandshake;
        port.attempts = 0;
        port.stage = Stage::EnterConfig;
        return;
    }
    case Stage::EnterConfig: {
        // IOP-14f: success here is the transfer completing, not what came
        // back -- the controller's answer to this frame is still in the old
        // mode's shape.
        if (exchange(kEnterConfigFrame, sizeof kEnterConfigFrame, kCtrl2)) {
            port.configurable = 2;
            port.frame_index = 0;
            port.attempts = 0;
            port.stage = Stage::ConfigFrames;
            return;
        }
        if (++port.attempts >= kRetries) {
            // A controller with no configuration mode. IOP-14f's second
            // ending: poll it as found, and say so in the record.
            port.configurable = 1;
            port.stage = Stage::Poll;
        }
        return;
    }
    case Stage::ConfigFrames: {
        const uint8_t *frame = kConfigFrames[port.frame_index];
        if (!exchange(frame, sizeof kConfigFrames[0], kCtrl2)
            || !answered(kIdConfig)) {
            if (++port.attempts >= kRetries) {
                // Leaving the sequence partway leaves the controller in the
                // mode, so the only safe exit is back to the start: the ID
                // probe will not match and discovery begins again.
                port.id = 0;
                port.stage = Stage::ProbeId;
            }
            return;
        }
        port.attempts = 0;
        if (port.frame_index == kFrameModel) {
            port.model = port.reply[3];
        }
        if (port.frame_index == kFrameExit) {
            port.stage = Stage::ReprobeId;
            return;
        }
        port.frame_index++;
        // IOP-14f: the button-mask query only goes to a controller whose
        // model byte says it has one. Sending it to a controller that does
        // not answers zeroes, which this driver would then store as a mask.
        if (port.frame_index == kFrameMask && (port.model & 2) == 0) {
            port.frame_index++;
        }
        return;
    }
    case Stage::ReprobeId: {
        if (!exchange(kPollFrame, sizeof kPollFrame, kCtrl2)
            || port.reply[1] == 0 || port.reply[2] != kReplyMagic) {
            port.id = 0;
            port.stage = Stage::ProbeId;
            return;
        }
        port.id = port.reply[1];
        port.stage = Stage::Poll;
        return;
    }
    case Stage::Poll: {
        if (!exchange(kPollFrame, sizeof kPollFrame, kCtrl2)) {
            port.valid = false;
            port.slot_state = kSlotFailed;
            if (++port.errors >= kRetries) {
                port.errors = 0;
                port.id = 0;
                port.stage = Stage::ProbeId;
            }
            return;
        }
        port.errors = 0;
        if (!answered(port.id)) {
            port.valid = false;
            port.slot_state = kSlotHandshake;
            port.id = 0;
            port.stage = Stage::ProbeId;
            return;
        }
        port.valid = true;
        port.slot_state = port.configurable == 2 ? kSlotConfigured : kSlotPlain;
        return;
    }
    }
}

// --- the record (IOP-14g) --------------------------------------------------

void pushRecord() {
    if (port.ee_address == 0) {
        // IOP-14c: the reference pushes anyway and closes the port as a
        // consequence. Refusing to push is the same outcome without writing
        // to address zero of the other processor.
        port.open = false;
        return;
    }
    clearRecord();
    const uint32_t frame = port.frame;
    record[0x00] = static_cast<uint8_t>(frame);
    record[0x01] = static_cast<uint8_t>(frame >> 8);
    record[0x02] = static_cast<uint8_t>(frame >> 16);
    record[0x03] = static_cast<uint8_t>(frame >> 24);
    record[0x04] = port.slot_state;
    record[0x05] = 0;                           // no request is ever in flight
    record[0x06] = port.valid ? 1 : 0;
    if (port.valid) {
        // The block is the reply with its first byte replaced by a validity
        // marker and its second by the ID, so everything after the magic byte
        // shifts down one: the buttons land at +0x0a and +0x0b.
        record[0x08] = 0;
        record[0x09] = port.id;
        for (uint32_t i = 3; i < port.reply_length; i++) {
            record[0x08 + i - 1] = port.reply[i];
        }
        record[0x27] = port.reply[2];
        record[0x28] = 0x20;
    } else {
        record[0x08] = 0xff;
    }
    record[0x2c] = port.stage == Stage::Poll ? 1 : 2;
    record[0x2d] = port.configurable;
    record[0x2e] = port.model;
    record[0x2f] = port.status_bit;
    record[0x30] = port.errors;

    Transfer transfer;
    transfer.src = reinterpret_cast<uintptr_t>(record);
    transfer.dest = port.ee_address + ((frame & 1) != 0 ? kRecordBytes : 0);
    transfer.size = kRecordBytes;
    transfer.attr = 0;
    uint32_t state;
    _import_intrman_suspend(&state);
    (void)_import_sifman_set_dma(&transfer, 1);
    _import_intrman_resume(state);
    port.frame = frame + 1;
}

// --- the two threads -------------------------------------------------------

// IOP-14d: the callback only signals once a port has been opened, so an image
// whose client never opens one costs nothing per blank.
int vblankCallback(void *) {
    if (port.open) {
        _import_thevent_iset(event, kBitVblank);
    }
    return 1;
}

void driverThread(void *) {
    for (;;) {
        uint32_t result = 0;
        _import_thevent_wait(event, kBitVblank, kWaitOr, &result);
        _import_thevent_clear(event, ~kBitVblank);
        if (!port.open) {
            continue;
        }
        // IOP-14d step 2. The two bits pick a second set of register words
        // whose meaning docs/analysis/54 could not read; this driver takes
        // the clear path and records the bit so that a machine raising it is
        // visible to the client rather than silently mis-served.
        port.status_bit = static_cast<uint8_t>(
            (_import_sio2man_stat70_get() >> 4) & 1);
        advance();
        pushRecord();
    }
}

// IOP-14b: the function code is request word 0 and the reply is the request
// buffer itself, returned as this function's answer.
void *serve(uint32_t, void *buffer, uint32_t) {
    auto *words = static_cast<uint32_t *>(buffer);
    if (words[0] - kCodeBase >= kCodeCount) {
        return buffer;
    }
    switch (words[0]) {
    case kCodeOpen:
        if (words[1] != 0 || words[2] != 0 || port.open) {
            words[3] = 0;                       // IOP-14 serves port 0 slot 0
            break;
        }
        port.ee_address = words[4];
        port.frame = 0;
        port.stage = Stage::ProbeId;
        port.frame_index = 0;
        port.attempts = 0;
        port.id = 0;
        port.model = 0;
        port.configurable = 0;
        port.slot_state = kSlotSilent;
        port.errors = 0;
        port.valid = false;
        port.status_bit = 0;
        port.open = true;
        words[3] = 1;
        break;
    case kCodeClose:
        words[3] = port.open && words[1] == 0 && words[2] == 0 ? 1 : 0;
        port.open = false;
        break;
    case kCodePortCount:
        words[3] = 2;
        break;
    case kCodeSlotCount:
        words[3] = 1;
        break;
    default:
        break;
    }
    return buffer;
}

void rpcThread(void *) {
    // docs/analysis/54 §2: the reference asks whether the SIF came up and
    // brings it up itself if not, rather than assuming the boot did it.
    if (_import_sifman_check_init() == 0) {
        (void)_import_sifman_init();
    }
    _import_sifcmd_init_rpc(0);
    _import_sifcmd_set_rpc_queue(&queue, static_cast<uint32_t>(_import_thbase_get_id()));
    _import_sifcmd_register_rpc(&server, kSid, reinterpret_cast<void *>(serve),
                                request, nullptr, nullptr, &queue);
    _import_sifcmd_rpc_loop(&queue);
}

[[nodiscard]] bool startThread(void (*entry)(void *), uint32_t priority) {
    ThreadParameters parameters;
    parameters.attr = kThreadAttr;
    parameters.option = 0;
    parameters.entry = entry;
    parameters.stack_size = kThreadStack;
    parameters.priority = priority;
    const int id = _import_thbase_create(&parameters);
    if (id < 0) {
        return false;
    }
    return _import_thbase_start(static_cast<uint32_t>(id), 0) >= 0;
}

extern "C" int _module_start(int, char **);

PS2_EXPORT_TABLE ExportTable<16> padman_exports = {
    ps2::module::kExportMagic,
    0,
    0x0102,
    0,
    {'p', 'a', 'd', 'm', 'a', 'n', 0, 0},
    {
        slot(_module_start),            // 0  _start
        slot(reservedHook),             // 1
        slot(reservedHook),             // 2
        slot(reservedHook),             // 3
        // IOP-14h: the reference exports twelve more entries, all of them the
        // IOP-side face of RPC codes this scope does not serve. The two the
        // digital path uses are reached through the RPC service, not through
        // an ordinal, so nothing in this image imports `padman` at all.
        slot(reservedHook),             // 4
        slot(reservedHook),             // 5
        slot(reservedHook),             // 6
        slot(reservedHook),             // 7
        slot(reservedHook),             // 8
        slot(reservedHook),             // 9
        slot(reservedHook),             // 10
        slot(reservedHook),             // 11
        slot(reservedHook),             // 12
        slot(reservedHook),             // 13
        slot(reservedHook),             // 14
        slot(reservedHook),             // 15
        nullptr,
    },
};

}  // namespace

PS2_IMPORTS_BEGIN("loadcore", 0x0101)
PS2_IMPORT(_import_loadcore_register, 6)
PS2_IMPORTS_END()

PS2_IMPORTS_BEGIN("intrman\0", 0x0102)
PS2_IMPORT(_import_intrman_enable_all, 9)
PS2_IMPORT(_import_intrman_suspend, 17)
PS2_IMPORT(_import_intrman_resume, 18)
PS2_IMPORTS_END()

PS2_IMPORTS_BEGIN("stdio\0\0\0", 0x0102)
PS2_IMPORT(_import_stdio_printf, 4)
PS2_IMPORTS_END()

PS2_IMPORTS_BEGIN("sio2man\0", 0x0101)
PS2_IMPORT(_import_sio2man_stat70_get, 11)
PS2_IMPORT(_import_sio2man_pad_transfer_init, 23)
PS2_IMPORT(_import_sio2man_transfer, 25)
PS2_IMPORTS_END()

PS2_IMPORTS_BEGIN("thbase\0\0", 0x0101)
PS2_IMPORT(_import_thbase_create, 4)
PS2_IMPORT(_import_thbase_start, 6)
PS2_IMPORT(_import_thbase_get_id, 20)
PS2_IMPORTS_END()

PS2_IMPORTS_BEGIN("thevent\0", 0x0101)
PS2_IMPORT(_import_thevent_create, 4)
PS2_IMPORT(_import_thevent_set, 6)
PS2_IMPORT(_import_thevent_iset, 7)
PS2_IMPORT(_import_thevent_clear, 8)
PS2_IMPORT(_import_thevent_wait, 10)
PS2_IMPORTS_END()

PS2_IMPORTS_BEGIN("sifman\0\0", 0x0101)
PS2_IMPORT(_import_sifman_init, 5)
PS2_IMPORT(_import_sifman_set_dma, 7)
PS2_IMPORT(_import_sifman_check_init, 29)
PS2_IMPORTS_END()

PS2_IMPORTS_BEGIN("sifcmd\0\0", 0x0101)
PS2_IMPORT(_import_sifcmd_init_rpc, 14)
PS2_IMPORT(_import_sifcmd_register_rpc, 17)
PS2_IMPORT(_import_sifcmd_set_rpc_queue, 19)
PS2_IMPORT(_import_sifcmd_rpc_loop, 22)
PS2_IMPORTS_END()

PS2_IMPORTS_BEGIN("vblank\0\0", 0x0101)
PS2_IMPORT(_import_vblank_register, 8)
PS2_IMPORTS_END()

extern "C" {

int _module_start(int, char **) {
    if (_import_loadcore_register(&padman_exports) < 0 || resident != 0) {
        return 1;
    }
    resident = 1;

    port.open = false;
    port.ee_address = 0;
    clearRecord();

    EventParameters parameters;
    parameters.attr = kEventMulti;
    parameters.option = 0;
    parameters.bits = 0;
    const int id = _import_thevent_create(&parameters);
    if (id < 0) {
        return 1;
    }
    event = static_cast<uint32_t>(id);

    if (!startThread(driverThread, kDriverPriority)
        || !startThread(rpcThread, kRpcPriority)) {
        return 1;
    }

    // docs/analysis/54 §1: the reference brackets this with CpuSuspendIntr
    // and closes with CpuEnableIntr, which takes no argument and enables
    // unconditionally -- so the saved state is computed and thrown away. Kept
    // as it is because RegisterVblankHandler refuses to run from an open
    // bracket it did not open, and the module's entry runs with interrupts
    // enabled either way.
    uint32_t state;
    _import_intrman_suspend(&state);
    (void)_import_vblank_register(kVblankStart, kVblankPriority, vblankCallback,
                                  nullptr);
    (void)_import_intrman_enable_all();

    _import_stdio_printf("PADMAN: controller driver, one port\n");
    return 0;                                   // resident
}

}  // extern "C"
