// SIO2MAN: the serial interface the pads and memory cards hang off.
//
// docs/spec/06-iop-kernel.md IOP-6, from docs/analysis/40. The module is
// twenty-one accessors over the SIO2 block's thirty-three registers, three
// calls that hand a transfer to a service thread through an event flag's
// bits, and the thread itself: claim, push the caller's registers and bytes,
// start the transfer, wait for the interrupt that says it finished, harvest
// the status and bytes, release. Nothing here polls the hardware; the one
// thing that ends a transfer's wait is IRQ 17's handler setting bit 7.
//
// This is the first module loaded on request rather than from the boot
// list, so its entry is what a program's `SifLoadModule` ends in.

#include "module.hpp"

#include <stdint.h>

namespace {

using ps2::module::ExportTable;
using ps2::module::reservedHook;
using ps2::module::slot;

constexpr uintptr_t kRegisters = 0xBF808200;    // IOP-6b: 33 words from here
constexpr uint32_t kIrq = 0x11;                 // I_STAT bit 17
constexpr uint32_t kDmaIn = 11;
constexpr uint32_t kDmaOut = 12;
constexpr uint32_t kDmaPriority = 3;
constexpr uint32_t kInitialControl = 0x3BC;     // IOP-6b, as observed
constexpr uint32_t kControlPrepare = 0xC;
constexpr uint32_t kControlStart = 0x1;
constexpr uint32_t kThreadAttr = 0x02000000;    // TH_C [header]
constexpr uint32_t kThreadStack = 0x2000;
constexpr uint32_t kThreadPriority = 0x18;
constexpr uint32_t kEventMulti = 2;             // EA_MULTI [header]
constexpr uint32_t kWaitOr = 1;                 // WEF_OR [header]
constexpr uint32_t kWaitAnd = 0;

// IOP-6c and docs/analysis/40 §4: the event flag's eight bits.
constexpr uint32_t kBitPadRequest = 0x01;
constexpr uint32_t kBitPadAck = 0x02;
constexpr uint32_t kBitMcRequest = 0x04;
constexpr uint32_t kBitMcAck = 0x08;
constexpr uint32_t kBitRun = 0x10;
constexpr uint32_t kBitDone = 0x20;
constexpr uint32_t kBitRelease = 0x40;
constexpr uint32_t kBitFinished = 0x80;

// Register indices, as the accessors name them [header sio2man.h].
constexpr uint32_t kRegData = 0;                // 0..15
constexpr uint32_t kPortCtrl1 = 16;             // 16, 18, 20, 22
constexpr uint32_t kPortCtrl2 = 17;             // 17, 19, 21, 23
constexpr uint32_t kDataOut = 24;
constexpr uint32_t kDataIn = 25;
constexpr uint32_t kCtrl = 26;
constexpr uint32_t kStat6c = 27;
constexpr uint32_t kStat70 = 28;
constexpr uint32_t kStat74 = 29;
constexpr uint32_t kUnknown78 = 30;
constexpr uint32_t kUnknown7c = 31;
constexpr uint32_t kStat = 32;

struct DmaArgument {
    uint32_t address;
    uint32_t size;
    uint32_t count;
};

// sio2_transfer_data_t [header]: what a caller hands to sio2_transfer.
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

extern "C" {
int _import_loadcore_register(void *table);
int _import_intrman_register(uint32_t irq, uint32_t mode, int (*handler)(void *), void *arg);
int _import_intrman_release(uint32_t irq);
int _import_intrman_enable(uint32_t irq);
int _import_intrman_disable(uint32_t irq, uint32_t *res);
int _import_intrman_suspend(uint32_t *state);
int _import_intrman_resume(uint32_t state);
int _import_stdio_printf(const char *format, ...);
int _import_dmacman_set_slice(uint32_t channel, uint32_t address, uint32_t size, uint32_t count, uint32_t direction);
void _import_dmacman_start(uint32_t channel);
void _import_dmacman_set_priority(uint32_t channel, uint32_t priority);
void _import_dmacman_enable(uint32_t channel);
void _import_dmacman_disable(uint32_t channel);
int _import_thbase_create(ThreadParameters *parameters);
int _import_thbase_start(uint32_t id, uint32_t arg);
int _import_thbase_get_id();
int _import_thevent_create(const EventParameters *parameters);
int _import_thevent_set(uint32_t id, uint32_t bits);
int _import_thevent_iset(uint32_t id, uint32_t bits);
int _import_thevent_clear(uint32_t id, uint32_t keep);
int _import_thevent_wait(uint32_t id, uint32_t bits, uint32_t mode, uint32_t *result);
}

uint32_t resident;
uint32_t event;
uint32_t thread;
TransferData *transfer;

[[nodiscard]] volatile uint32_t &reg(uint32_t index) {
    return *reinterpret_cast<volatile uint32_t *>(kRegisters + index * 4);
}

// --- the accessors, ordinals 4..22 ---------------------------------------

void ctrlSet(uint32_t value) { reg(kCtrl) = value; }
uint32_t ctrlGet() { return reg(kCtrl); }
uint32_t stat6cGet() { return reg(kStat6c); }
void portCtrl1Set(uint32_t port, uint32_t value) { if (port < 4) reg(kPortCtrl1 + port * 2) = value; }
uint32_t portCtrl1Get(uint32_t port) { return port < 4 ? reg(kPortCtrl1 + port * 2) : 0; }
void portCtrl2Set(uint32_t port, uint32_t value) { if (port < 4) reg(kPortCtrl2 + port * 2) = value; }
uint32_t portCtrl2Get(uint32_t port) { return port < 4 ? reg(kPortCtrl2 + port * 2) : 0; }
uint32_t stat70Get() { return reg(kStat70); }
void regSet(uint32_t index, uint32_t value) { if (index < 16) reg(kRegData + index) = value; }
uint32_t regGet(uint32_t index) { return index < 16 ? reg(kRegData + index) : 0; }
uint32_t stat74Get() { return reg(kStat74); }
void unknown78Set(uint32_t value) { reg(kUnknown78) = value; }
uint32_t unknown78Get() { return reg(kUnknown78); }
void unknown7cSet(uint32_t value) { reg(kUnknown7c) = value; }
uint32_t unknown7cGet() { return reg(kUnknown7c); }
void dataOut(uint8_t value) { reg(kDataOut) = value; }
uint8_t dataIn() { return static_cast<uint8_t>(reg(kDataIn)); }
void statSet(uint32_t value) { reg(kStat) = value; }
uint32_t statGet() { return reg(kStat); }

// --- the transfer (docs/analysis/40 §4 steps 5-9) ---------------------------

// Push the caller's registers and bytes, and start the channels it asked
// for. `port_ctrl2` is not pushed: the reference's push does not either.
void pushTransfer(const TransferData &td) {
    for (uint32_t port = 0; port < 4; port++) {
        portCtrl1Set(port, td.port_ctrl1[port]);
    }
    for (uint32_t index = 0; index < 16; index++) {
        regSet(index, td.regdata[index]);
    }
    for (uint32_t k = 0; k < td.in_size; k++) {
        dataOut(td.in[k]);
    }
    if (td.in_dma.address != 0) {
        _import_dmacman_set_slice(kDmaIn, td.in_dma.address, td.in_dma.size, td.in_dma.count, 1);
        _import_dmacman_start(kDmaIn);
    }
    if (td.out_dma.address != 0) {
        _import_dmacman_set_slice(kDmaOut, td.out_dma.address, td.out_dma.size, td.out_dma.count, 0);
        _import_dmacman_start(kDmaOut);
    }
}

void harvestTransfer(TransferData &td) {
    td.stat6c = stat6cGet();
    td.stat70 = stat70Get();
    td.stat74 = stat74Get();
    for (uint32_t k = 0; k < td.out_size; k++) {
        td.out[k] = dataIn();
    }
}

// IOP-6b: the handler acknowledges STAT by writing back what it read, and
// tells the thread with the one bit nothing else sets.
int interruptHandler(void *) {
    statSet(statGet());
    _import_thevent_iset(event, kBitFinished);
    return 1;
}

// The service thread: IOP-6c's parked state is step 1's wait.
void serviceThread(void *) {
    for (;;) {
        uint32_t result = 0;
        _import_thevent_wait(event, kBitPadRequest | kBitMcRequest, kWaitOr, &result);
        if (result & kBitPadRequest) {
            _import_thevent_clear(event, ~kBitPadRequest);
            _import_thevent_set(event, kBitPadAck);
        } else if (result & kBitMcRequest) {
            _import_thevent_clear(event, ~kBitMcRequest);
            _import_thevent_set(event, kBitMcAck);
        } else {
            _import_stdio_printf("SIO2_BASIC_THREAD : why I wakeup ? %08lx\n", result);
            return;                             // the reference's thread exits too
        }
        _import_thevent_wait(event, kBitRun, kWaitAnd, &result);
        _import_thevent_clear(event, ~kBitRun);
        ctrlSet(ctrlGet() | kControlPrepare);
        pushTransfer(*transfer);
        ctrlSet(ctrlGet() | kControlStart);
        _import_thevent_wait(event, kBitFinished, kWaitAnd, &result);
        _import_thevent_clear(event, ~kBitFinished);
        harvestTransfer(*transfer);
        _import_thevent_set(event, kBitDone);
        _import_thevent_wait(event, kBitRelease, kWaitAnd, &result);
        _import_thevent_clear(event, ~kBitRelease);
    }
}

// --- ordinals 23..25: the callers' side of the handshake -------------------

void claim(uint32_t request, uint32_t ack) {
    uint32_t result;
    _import_thevent_set(event, request);
    _import_thevent_wait(event, ack, kWaitAnd, &result);
    _import_thevent_clear(event, ~ack);
}

void padTransferInit() {
    claim(kBitPadRequest, kBitPadAck);
}

void mcTransferInit() {
    claim(kBitMcRequest, kBitMcAck);
}

int doTransfer(TransferData *td) {
    uint32_t result;
    transfer = td;
    _import_thevent_set(event, kBitRun);
    _import_thevent_wait(event, kBitDone, kWaitAnd, &result);
    _import_thevent_clear(event, ~kBitDone);
    _import_thevent_set(event, kBitRelease);
    return 1;
}

// IOP-6a's mirror.
int deinit() {
    (void)_import_thbase_get_id();
    uint32_t state;
    uint32_t pending;
    _import_intrman_suspend(&state);
    _import_intrman_disable(kIrq, &pending);
    _import_intrman_release(kIrq);
    _import_intrman_resume(state);
    _import_dmacman_disable(kDmaIn);
    _import_dmacman_disable(kDmaOut);
    return 0;
}

extern "C" int _module_start(int, char **);

[[gnu::used]] ExportTable<26> sio2man_exports = {
    ps2::module::kExportMagic,
    0,
    0x0101,
    0,
    {'s', 'i', 'o', '2', 'm', 'a', 'n', 0},
    {
        slot(_module_start),            // 0  _start
        slot(reservedHook),             // 1
        slot(deinit),                   // 2  _deinit
        slot(reservedHook),             // 3
        slot(ctrlSet),                  // 4
        slot(ctrlGet),                  // 5
        slot(stat6cGet),                // 6
        slot(portCtrl1Set),             // 7
        slot(portCtrl1Get),             // 8
        slot(portCtrl2Set),             // 9
        slot(portCtrl2Get),             // 10
        slot(stat70Get),                // 11
        slot(regSet),                   // 12
        slot(regGet),                   // 13
        slot(stat74Get),                // 14
        slot(unknown78Set),             // 15
        slot(unknown78Get),             // 16
        slot(unknown7cSet),             // 17
        slot(unknown7cGet),             // 18
        slot(dataOut),                  // 19
        slot(dataIn),                   // 20
        slot(statSet),                  // 21
        slot(statGet),                  // 22
        slot(padTransferInit),          // 23
        slot(mcTransferInit),           // 24
        slot(doTransfer),               // 25
        nullptr,
    },
};

}  // namespace

PS2_IMPORTS_BEGIN("loadcore", 0x0101)
PS2_IMPORT(_import_loadcore_register, 6)
PS2_IMPORTS_END()

PS2_IMPORTS_BEGIN("intrman\0", 0x0102)
PS2_IMPORT(_import_intrman_register, 4)
PS2_IMPORT(_import_intrman_release, 5)
PS2_IMPORT(_import_intrman_enable, 6)
PS2_IMPORT(_import_intrman_disable, 7)
PS2_IMPORT(_import_intrman_suspend, 17)
PS2_IMPORT(_import_intrman_resume, 18)
PS2_IMPORTS_END()

PS2_IMPORTS_BEGIN("stdio\0\0\0", 0x0102)
PS2_IMPORT(_import_stdio_printf, 4)
PS2_IMPORTS_END()

PS2_IMPORTS_BEGIN("dmacman\0", 0x0102)
PS2_IMPORT(_import_dmacman_set_slice, 28)
PS2_IMPORT(_import_dmacman_start, 32)
PS2_IMPORT(_import_dmacman_set_priority, 33)
PS2_IMPORT(_import_dmacman_enable, 34)
PS2_IMPORT(_import_dmacman_disable, 35)
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

extern "C" {

// IOP-6a, in the reference's order.
int _module_start(int, char **) {
    if (_import_loadcore_register(&sio2man_exports) < 0 || resident != 0) {
        return 1;
    }
    resident = 1;
    ctrlSet(kInitialControl);

    EventParameters event_parameters;
    event_parameters.attr = kEventMulti;
    event_parameters.option = 0;
    event_parameters.bits = 0;
    event = static_cast<uint32_t>(_import_thevent_create(&event_parameters));

    ThreadParameters thread_parameters;
    thread_parameters.attr = kThreadAttr;
    thread_parameters.option = 0;
    thread_parameters.entry = serviceThread;
    thread_parameters.stack_size = kThreadStack;
    thread_parameters.priority = kThreadPriority;
    thread = static_cast<uint32_t>(_import_thbase_create(&thread_parameters));

    uint32_t state;
    _import_intrman_suspend(&state);
    _import_intrman_register(kIrq, 1, interruptHandler, &transfer);
    _import_intrman_enable(kIrq);
    _import_intrman_resume(state);

    _import_dmacman_set_priority(kDmaIn, kDmaPriority);
    _import_dmacman_set_priority(kDmaOut, kDmaPriority);
    _import_dmacman_enable(kDmaIn);
    _import_dmacman_enable(kDmaOut);
    _import_thbase_start(thread, 0);
    return 0;
}

}  // extern "C"
