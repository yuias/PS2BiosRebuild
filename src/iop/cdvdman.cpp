// CDVDMAN: the disc driver, `cdrom0:`.
//
// docs/analysis/42-cdvd.md is what this follows; a spec section for it
// (working title IOP-8) has not been merged yet, so the comments below cite
// that analysis directly by section. One device, `cdrom` (§1), with a real
// open/close/read/lseek resolving `\DIR\FILE;1`-shaped paths against the
// disc's ISO9660 layout (§4) and every other op sharing one "return 0" stub,
// write included (§1's table) -- unlike ROMDRV, which gives `write` its own
// error. Sectors are moved by DMA channel 3, armed directly by this driver
// (no `dmacman` import) and completed by IRQ 2, whose handler this module
// registers itself (§1, §2b, §3). No dedicated thread exists: every hardware
// command runs on whichever thread called in, serialised by a semaphore.

#include "module.hpp"

#include <stdint.h>

namespace {

using ps2::module::ExportTable;
using ps2::module::slot;

constexpr uint32_t kSectorSize = 2048;          // §3: the plain, datapattern-0 case
constexpr uint32_t kPvdLba = 16;                // §4: the ISO9660 PVD
constexpr uint32_t kRootRecordOffset = 156;     // §4: the PVD's embedded root record
constexpr uint32_t kMaxLevel = 8;               // §4 step 3: CdlMAXLEVEL [header]
constexpr uint32_t kHandles = 8;
constexpr uint32_t kDeviceFilesystem = 0x10;    // IOP_DT_FS [header]

constexpr int kErrorNotFound = -2;
constexpr int kErrorBadHandle = -9;
constexpr int kErrorHandlesFull = -12;
constexpr int kErrorBadWhence = -22;

// DMA channel 3, bank 1 -- CDVD's channel, poked directly (§1: no `dmacman`
// import exists in the reference either).
constexpr uintptr_t kMadr = 0xBF8010B0;
constexpr uintptr_t kBcr = 0xBF8010B4;
constexpr uintptr_t kChcr = 0xBF8010B8;
constexpr uintptr_t kDpcr = 0xBF8010F0;
constexpr uint32_t kDpcrCh3Enable = 0x8000;
constexpr uint32_t kChcrStart = 0x41000200;
// BCR's low half is the per-block size in words; §3 step 2's `count << 4`
// selects a per-datapattern multiplier for the command's own length field,
// but the word this driver only implements -- datapattern 0, a plain
// 2048-byte sector -- moves exactly one block per sector: 2048 / 4 = 512
// words = 0x200, matching §3's own literal BCR formula for this case.
constexpr uint32_t kBlockWords = 0x200;

// The N-command register block (§2b) -- separate from the S-command pair
// `docs/analysis/26` already covers, and not imported through this module.
constexpr uintptr_t kNCommand = 0xBF402004;
constexpr uintptr_t kNStatus = 0xBF402005;      // read: status; write: one param byte
constexpr uintptr_t kNSubmode = 0xBF402006;     // write: submode; read: result byte
constexpr uintptr_t kNBreak = 0xBF402007;
constexpr uintptr_t kNIrqStat = 0xBF402008;
constexpr uintptr_t kNReady = 0xBF40200A;       // sceCdStatus/sceCdDiskReady
constexpr uintptr_t kNDiskType = 0xBF40200F;

constexpr uint32_t kCdvdIrq = 2;                // §1: the CDVD interrupt line
// §2b's observed submode set is {0x40,0x80,0x83,0x85,0x86,0x8f}, chosen by
// disk type and datapattern; 0x80 is the ordinary PS2 CD/DVD, datapattern-0
// value -- the only case this driver builds.
constexpr uint8_t kSubmodePs2Disc = 0x80;

extern "C" {
int _import_loadcore_register(void *table);
int _import_ioman_add_drv(void *device);
int _import_ioman_del_drv(const char *name);
int _import_intrman_register(uint32_t irq, uint32_t mode, int (*handler)(void *), void *arg);
int _import_intrman_enable(uint32_t irq);
int _import_intrman_suspend(uint32_t *state);
int _import_intrman_resume(uint32_t state);
int _import_thbase_getid();
int _import_thbase_delay(uint32_t usec);
int _import_thsemap_create(void *sema);
int _import_thsemap_delete(int id);
int _import_thsemap_wait(int id);
int _import_thsemap_signal(int id);
int _import_thsemap_poll(int id);
}

[[nodiscard]] uint32_t readReg32(uintptr_t address) {
    return *reinterpret_cast<volatile uint32_t *>(address);
}

void writeReg32(uintptr_t address, uint32_t value) {
    *reinterpret_cast<volatile uint32_t *>(address) = value;
}

[[nodiscard]] uint8_t readReg8(uintptr_t address) {
    return *reinterpret_cast<volatile uint8_t *>(address);
}

void writeReg8(uintptr_t address, uint8_t value) {
    *reinterpret_cast<volatile uint8_t *>(address) = value;
}

// iop_sema_t [header]: the caller-supplied fields only, not the kernel's own
// bookkeeping record (IOP-3g).
struct Sema {
    uint32_t attr;
    uint32_t option;
    int32_t initial;
    int32_t max;
};

int sema_driver = -1;   // guards a whole open()/read() call and the sector cache
int sema_ncmd = -1;     // guards the N-command channel itself (§2b step 1)

volatile int g_done;              // 0 pending, 1 ok, -1 error -- the IRQ-2 handler's word
volatile uint8_t g_result;        // the command's raw result byte (sceCdGetError)
volatile uint32_t g_issuer_thread;  // stashed at issue time, not consumed -- §2b step 3

// `data` is a DMA destination, so it is aligned rather than packed in behind
// the two bookkeeping fields: channel 3 writes RAM directly and the address it
// is handed is not rounded for it.
struct SectorCache {
    uint32_t lba;
    bool valid;
    alignas(16) uint8_t data[kSectorSize];
};
SectorCache cache;

// ---- the hardware sequence: arm, submode, params, command (§3) ------------

void armDma(uint32_t dest, uint32_t sectors) {
    writeReg32(kChcr, 0);
    writeReg32(kMadr, dest);
    writeReg32(kBcr, (sectors << 16) | kBlockWords);
    writeReg32(kChcr, kChcrStart);
}

// Issue N-command 6 (read) for `sectors` 2048-byte sectors at `lbn` into
// `dest`. Returns once the command is accepted, not once the data has
// landed (§3 step 6) -- the caller waits separately, via `cdSync`.
bool issueRead(uint32_t lbn, uint32_t sectors, void *dest) {
    if (_import_thsemap_poll(sema_ncmd) < 0) {
        return false;                            // declined: channel busy (§2b step 1)
    }
    armDma(reinterpret_cast<uint32_t>(dest), sectors);
    // §2b step 2: a precondition check, not a spin -- an unready channel
    // declines immediately here. `sceCdInit(mode=0)` is what actually spins
    // on this same bit, before any command is ever sent.
    if ((readReg8(kNStatus) & 0xC0) != 0x40) {
        _import_thsemap_signal(sema_ncmd);
        return false;
    }
    g_issuer_thread = static_cast<uint32_t>(_import_thbase_getid());
    g_done = 0;
    writeReg8(kNSubmode, kSubmodePs2Disc);
    uint8_t params[11];
    params[0] = static_cast<uint8_t>(lbn);
    params[1] = static_cast<uint8_t>(lbn >> 8);
    params[2] = static_cast<uint8_t>(lbn >> 16);
    params[3] = static_cast<uint8_t>(lbn >> 24);
    params[4] = static_cast<uint8_t>(sectors);
    params[5] = static_cast<uint8_t>(sectors >> 8);
    params[6] = static_cast<uint8_t>(sectors >> 16);
    params[7] = static_cast<uint8_t>(sectors >> 24);
    params[8] = 0;                                // mode->trycount, defaulted
    params[9] = kSubmodePs2Disc;                   // the same byte written to kNSubmode
    params[10] = 0;                                // mode->datapattern: 0, plain 2048 bytes
    for (uint8_t byte : params) {
        writeReg8(kNStatus, byte);
    }
    writeReg8(kNCommand, 6);
    (void)readReg8(kNCommand);                     // the reference reads it back too
    _import_thsemap_signal(sema_ncmd);
    return true;
}

// sceCdSync(mode): 0 busy-polls with a 1 ms sleep between checks; 1 checks
// once and returns immediately -- never a semaphore wait (§2b/§3).
int cdSync(int mode) {
    if (mode == 1) {
        return g_done;
    }
    while (g_done == 0) {
        _import_thbase_delay(1000);
    }
    return g_done;
}

// A blocking read for the driver's own use (the path walk, IOMAN's `read`):
// issue and wait, retrying on a declined channel the way the reference's
// `sceCdApplyNCmd` wrapper does around the same sender (§2b).
bool readSectorBlocking(uint32_t lba, void *dest) {
    for (int attempt = 0; attempt < 8; attempt++) {
        if (issueRead(lba, 1, dest)) {
            return cdSync(0) > 0;
        }
        _import_thbase_delay(2000);
    }
    return false;
}

// One sector of cache, so a byte-range request (LOADFILE's 4 KiB chunks at
// arbitrary offsets) costs one hardware read per sector touched, not one per
// call.
const uint8_t *sectorCached(uint32_t lba) {
    if (cache.valid && cache.lba == lba) {
        return cache.data;
    }
    cache.valid = false;
    if (!readSectorBlocking(lba, cache.data)) {
        return nullptr;
    }
    cache.lba = lba;
    cache.valid = true;
    return cache.data;
}

// ---- IRQ 2: the completion side of an N-command (§2b) ----------------------

int cdvdIrqHandler(void *) {
    g_result = readReg8(kNSubmode);               // the result byte, sceCdGetError's answer
    if ((readReg8(kNIrqStat) & 1) == 0) {
        writeReg8(kNIrqStat, 2);                   // ack, retry path
        return 1;
    }
    // Bit 0 of the status byte is the command's *error* flag: set means the
    // command failed. The reference decides this in a branch delay slot --
    // `addiu $2,$zero,-1` sits in the slot of the `bnez` and so runs on both
    // paths, which is why the taken (bit set) path is the -1 one.
    g_done = (readReg8(kNStatus) & 1) ? -1 : 1;
    writeReg8(kNIrqStat, 1);                       // ack, done path
    return 1;
}

void initIrqDma() {
    static bool installed = false;
    if (installed) {
        return;
    }
    installed = true;
    _import_intrman_register(kCdvdIrq, 1, cdvdIrqHandler, nullptr);
    _import_intrman_enable(kCdvdIrq);
    writeReg32(kDpcr, readReg32(kDpcr) | kDpcrCh3Enable);
    writeReg32(kChcr, 0);
}

// ---- ISO9660: the PVD, directory records, the path walk (§4) ---------------

[[nodiscard]] uint32_t readLe32(const uint8_t *at) {
    return static_cast<uint32_t>(at[0]) | (static_cast<uint32_t>(at[1]) << 8)
         | (static_cast<uint32_t>(at[2]) << 16) | (static_cast<uint32_t>(at[3]) << 24);
}

struct DirEntry {
    uint32_t lba;
    uint32_t size;
    bool is_directory;
    const uint8_t *name;      // inside the cache's current sector
    uint8_t name_length;
    const uint8_t *date;      // 7 raw ISO9660 date bytes, also inside the cache
};

// A directory entry's own name may carry a ";1" version suffix a caller's
// component omits; compare the base name unless the caller supplied a
// version itself (§4: "with or without the version suffix").
[[nodiscard]] bool componentMatches(const uint8_t *id, uint8_t id_length,
                                     const char *component, uint32_t component_length) {
    bool component_has_version = false;
    for (uint32_t k = 0; k < component_length; k++) {
        if (component[k] == ';') {
            component_has_version = true;
            break;
        }
    }
    uint8_t compare_length = id_length;
    if (!component_has_version) {
        for (uint8_t k = 0; k < id_length; k++) {
            if (id[k] == ';') {
                compare_length = k;
                break;
            }
        }
    }
    if (compare_length != component_length) {
        return false;
    }
    for (uint32_t k = 0; k < component_length; k++) {
        if (static_cast<char>(id[k]) != component[k]) {
            return false;
        }
    }
    return true;
}

// Scan one directory's extent for `component`, sector by sector (§4 step 4).
[[nodiscard]] bool findInDirectory(uint32_t dir_lba, uint32_t dir_size, const char *component,
                                    uint32_t component_length, DirEntry &out) {
    uint32_t sectors = (dir_size + kSectorSize - 1) / kSectorSize;
    for (uint32_t s = 0; s < sectors; s++) {
        const uint8_t *sector = sectorCached(dir_lba + s);
        if (sector == nullptr) {
            return false;
        }
        uint32_t pos = 0;
        while (pos < kSectorSize) {
            uint8_t length = sector[pos];
            if (length == 0 || pos + length > kSectorSize) {
                break;                            // padding to the sector boundary
            }
            uint8_t id_length = sector[pos + 32];
            const uint8_t *id = sector + pos + 33;
            if (componentMatches(id, id_length, component, component_length)) {
                out.lba = readLe32(sector + pos + 2);
                out.size = readLe32(sector + pos + 10);
                out.is_directory = (sector[pos + 25] & 2) != 0;
                out.name = id;
                out.name_length = id_length;
                out.date = sector + pos + 18;
                return true;
            }
            pos += length;
        }
    }
    return false;
}

// Resolve a `\DIR\FILE;1`-shaped path from the PVD's root directory (§4),
// splitting at '\' and descending up to `kMaxLevel` component levels.
[[nodiscard]] bool resolvePath(const char *path, DirEntry &out) {
    if (path[0] != '\\') {
        return false;                             // §4 step 2: anything else is "not found"
    }
    const uint8_t *pvd = sectorCached(kPvdLba);
    if (pvd == nullptr || pvd[1] != 'C' || pvd[2] != 'D' || pvd[3] != '0'
        || pvd[4] != '0' || pvd[5] != '1') {
        return false;
    }
    uint32_t dir_lba = readLe32(pvd + kRootRecordOffset + 2);
    uint32_t dir_size = readLe32(pvd + kRootRecordOffset + 10);

    uint32_t at = 1;
    for (uint32_t level = 0; level < kMaxLevel; level++) {
        uint32_t start = at;
        while (path[at] != '\0' && path[at] != '\\') {
            at++;
        }
        uint32_t length = at - start;
        if (length == 0) {
            return false;
        }
        DirEntry entry;
        if (!findInDirectory(dir_lba, dir_size, path + start, length, entry)) {
            return false;                         // §4 step 4: "dir was not found"
        }
        bool last = path[at] == '\0';
        if (last) {
            out = entry;
            return !entry.is_directory;
        }
        if (!entry.is_directory) {
            return false;                          // a non-final component must be a directory
        }
        dir_lba = entry.lba;
        dir_size = entry.size;
        at++;                                      // past the '\'
    }
    return false;                                  // deeper than kMaxLevel (§4 step 3)
}

// ---- the `cdrom` device: open/close/read/lseek (§1) -------------------------

struct Handle {
    bool in_use;
    uint32_t lba;
    uint32_t size;
    uint32_t position;
};
Handle handles[kHandles];

struct File {
    uint32_t mode;
    uint32_t unit;
    void *device;
    void *privdata;
};

int init(void *) {
    Sema driver_sema = {1 /* SA_THPRI */, 0, 1, 1};
    Sema ncmd_sema = {1, 0, 1, 1};
    sema_driver = _import_thsemap_create(&driver_sema);
    sema_ncmd = _import_thsemap_create(&ncmd_sema);
    for (Handle &handle : handles) {
        handle.in_use = false;
    }
    cache.valid = false;
    initIrqDma();
    return 0;
}

int deinit(void *) {
    _import_thsemap_delete(sema_driver);           // §1: deinit frees only this one
    cache.valid = false;
    return 0;
}

// §1's shared stub: every op besides init/deinit/open/close/read/lseek,
// write included, unconditionally succeeds and does nothing.
int returnZero(File *, ...) {
    return 0;
}

int open(File *file, const char *name, int /* flags */) {
    _import_thsemap_wait(sema_driver);
    DirEntry entry;
    bool found = resolvePath(name, entry);
    int result = kErrorNotFound;
    if (found) {
        uint32_t state;
        _import_intrman_suspend(&state);
        result = kErrorHandlesFull;
        for (uint32_t k = 0; k < kHandles; k++) {
            if (!handles[k].in_use) {
                handles[k].in_use = true;
                handles[k].lba = entry.lba;
                handles[k].size = entry.size;
                handles[k].position = 0;
                file->privdata = reinterpret_cast<void *>(k);
                result = 0;
                break;
            }
        }
        _import_intrman_resume(state);
    }
    _import_thsemap_signal(sema_driver);
    return result;
}

[[nodiscard]] Handle *handleOf(File *file) {
    const auto index = reinterpret_cast<uintptr_t>(file->privdata);
    if (index >= kHandles || !handles[index].in_use) {
        return nullptr;
    }
    return &handles[index];
}

int close(File *file) {
    Handle *handle = handleOf(file);
    if (handle == nullptr) {
        return kErrorBadHandle;
    }
    handle->in_use = false;
    return 0;
}

int read(File *file, void *buffer, int size) {
    Handle *handle = handleOf(file);
    if (handle == nullptr) {
        return kErrorBadHandle;
    }
    uint32_t length = size < 0 ? 0 : static_cast<uint32_t>(size);
    if (length > handle->size - handle->position) {
        length = handle->size - handle->position;
    }
    _import_thsemap_wait(sema_driver);
    auto *to = static_cast<uint8_t *>(buffer);
    uint32_t done = 0;
    while (done < length) {
        uint32_t position = handle->position + done;
        uint32_t sector = handle->lba + position / kSectorSize;
        uint32_t offset = position % kSectorSize;
        const uint8_t *data = sectorCached(sector);
        if (data == nullptr) {
            break;
        }
        uint32_t chunk = kSectorSize - offset;
        if (chunk > length - done) {
            chunk = length - done;
        }
        for (uint32_t k = 0; k < chunk; k++) {
            to[done + k] = data[offset + k];
        }
        done += chunk;
    }
    _import_thsemap_signal(sema_driver);
    handle->position += done;
    return static_cast<int>(done);
}

int lseek(File *file, int offset, int whence) {
    Handle *handle = handleOf(file);
    if (handle == nullptr) {
        return kErrorBadHandle;
    }
    int32_t base;
    switch (whence) {
    case 0:
        base = 0;
        break;
    case 1:
        base = static_cast<int32_t>(handle->position);
        break;
    case 2:
        base = static_cast<int32_t>(handle->size);
        break;
    default:
        return kErrorBadWhence;
    }
    int32_t position = base + offset;
    if (position < 0) {
        position = 0;
    }
    if (position > static_cast<int32_t>(handle->size)) {
        position = static_cast<int32_t>(handle->size);
    }
    handle->position = static_cast<uint32_t>(position);
    return position;
}

// §1's ops table; `write` shares the stub too, unlike ROMDRV's dedicated
// error for it.
struct Operations {
    int (*init)(void *);
    int (*deinit)(void *);
    int (*format)(File *, ...);
    int (*open)(File *, const char *, int);
    int (*close)(File *);
    int (*read)(File *, void *, int);
    int (*write)(File *, ...);
    int (*lseek)(File *, int, int);
    int (*ioctl)(File *, ...);
    int (*remove)(File *, ...);
    int (*mkdir)(File *, ...);
    int (*rmdir)(File *, ...);
    int (*dopen)(File *, ...);
    int (*dclose)(File *, ...);
    int (*dread)(File *, ...);
    int (*getstat)(File *, ...);
    int (*chstat)(File *, ...);
};

Operations operations = {
    init, deinit, returnZero, open, close, read, returnZero, lseek,
    returnZero, returnZero, returnZero, returnZero, returnZero, returnZero,
    returnZero, returnZero, returnZero,
};

struct Device {
    const char *name;
    uint32_t type;
    uint32_t version;
    const char *description;
    Operations *ops;
};

const char kName[] = "cdrom";                   // §1: no digit
const char kDescription[] = "CD-ROM";
Device device = {kName, kDeviceFilesystem, 1, kDescription, &operations};

int moduleInit() {
    _import_ioman_del_drv("cdrom");
    return _import_ioman_add_drv(&device);        // triggers ops.init (§1)
}

// ---- the exported ordinals (§0, §2b, §3, §4) --------------------------------

// sceCdInit(mode): mode 0 spins on the N-command channel's ready bit before
// anything else may be sent (§1/§7a); every mode installs the IRQ/DMA state
// (idempotent -- §1's routine runs once from the entry and again here).
int sceCdInit(int mode) {
    if (mode == 0) {
        while ((readReg8(kNStatus) & 0xC0) != 0x40) {
        }
    }
    initIrqDma();
    return 0;
}

// sceCdRead(lbn, sectors, buf, mode): non-blocking -- issues the command and
// arms the DMA, and returns "accepted," not "done" (§3 step 6). `mode` is
// unused: this driver only builds the datapattern-0, plain 2048-byte case.
int sceCdRead(uint32_t lbn, uint32_t sectors, void *buffer, void * /* mode */) {
    return issueRead(lbn, sectors, buffer) ? 0 : -1;
}

int sceCdGetError() {
    return g_result;
}

// sceCdlFILE [header].
struct CdlFile {
    uint32_t lsn;
    uint32_t size;
    char name[16];
    uint8_t date[8];
};

int sceCdSearchFile(CdlFile *file, const char *name) {
    _import_thsemap_wait(sema_driver);
    DirEntry entry;
    bool found = resolvePath(name, entry);
    if (found) {
        file->lsn = entry.lba;
        file->size = entry.size;
        uint8_t k = 0;
        for (; k < entry.name_length && k < 15; k++) {
            file->name[k] = static_cast<char>(entry.name[k]);
        }
        for (; k < 16; k++) {
            file->name[k] = '\0';
        }
        for (uint8_t d = 0; d < 7; d++) {
            file->date[d] = entry.date[d];
        }
        file->date[7] = 0;
    }
    _import_thsemap_signal(sema_driver);
    return found ? 1 : 0;
}

int sceCdSync(int mode) {
    return cdSync(mode);
}

int sceCdGetDiskType() {
    return readReg8(kNDiskType);
}

// mode 1: a single check; blocking mode is a bare register-polling spin, no
// event flag (§8's correction of the sibling-project lead).
int sceCdDiskReady(int mode) {
    if (mode == 1) {
        return readReg8(kNReady);
    }
    while (readReg8(kNReady) != 0x0A) {
    }
    return 0x0A;
}

int sceCdCheckCmd() {
    return g_done;
}

int sceCdStatus() {
    return readReg8(kNReady);
}

int sceCdBreak() {
    writeReg8(kNBreak, 1);
    return 0;
}

int sceCdNop() {
    return 0;
}

// The rest of the 62-slot table (§0's ordinal ceiling): a shared "return 0"
// filler, matching the reference's own table size exactly.
int exportStub() {
    return 0;
}

PS2_EXPORT_TABLE ExportTable<62> cdvdman_exports = {
    ps2::module::kExportMagic,
    0,
    0x0101,                          // IRX-2b: the reference exports cdvdman v1.01
    0,
    {'c', 'd', 'v', 'd', 'm', 'a', 'n', 0},
    {
        slot(moduleInit),            // 0  the module's own init, re-exported
        slot(exportStub),            // 1
        slot(exportStub),            // 2
        slot(exportStub),            // 3
        slot(sceCdInit),             // 4
        slot(exportStub),            // 5  sceCdStandby
        slot(sceCdRead),             // 6
        slot(exportStub),            // 7  sceCdSeek
        slot(sceCdGetError),         // 8
        slot(exportStub),            // 9  sceCdGetToc
        slot(sceCdSearchFile),       // 10
        slot(sceCdSync),             // 11
        slot(sceCdGetDiskType),      // 12
        slot(sceCdDiskReady),        // 13
        slot(exportStub),            // 14
        slot(exportStub),            // 15
        slot(exportStub),            // 16
        slot(exportStub),            // 17
        slot(exportStub),            // 18
        slot(exportStub),            // 19
        slot(exportStub),            // 20
        slot(sceCdCheckCmd),         // 21
        slot(exportStub),            // 22
        slot(exportStub),            // 23
        slot(exportStub),            // 24
        slot(exportStub),            // 25
        slot(exportStub),            // 26
        slot(exportStub),            // 27
        slot(sceCdStatus),           // 28
        slot(exportStub),            // 29
        slot(exportStub),            // 30
        slot(exportStub),            // 31
        slot(exportStub),            // 32
        slot(exportStub),            // 33
        slot(exportStub),            // 34
        slot(exportStub),            // 35
        slot(exportStub),            // 36
        slot(exportStub),            // 37
        slot(exportStub),            // 38
        slot(sceCdBreak),            // 39
        slot(exportStub),            // 40
        slot(exportStub),            // 41
        slot(exportStub),            // 42
        slot(exportStub),            // 43
        slot(exportStub),            // 44
        slot(exportStub),            // 45
        slot(sceCdNop),              // 46
        slot(exportStub),            // 47  sceGetFsvRbuf
        slot(exportStub),            // 48
        slot(exportStub),            // 49
        slot(exportStub),            // 50
        slot(exportStub),            // 51
        slot(exportStub),            // 52
        slot(exportStub),            // 53
        slot(exportStub),            // 54
        slot(exportStub),            // 55
        slot(exportStub),            // 56
        slot(exportStub),            // 57
        slot(exportStub),            // 58
        slot(exportStub),            // 59
        slot(exportStub),            // 60
        slot(exportStub),            // 61
        nullptr,
    },
};

}  // namespace

PS2_IMPORTS_BEGIN("ioman\0\0\0", 0x0102)
PS2_IMPORT(_import_ioman_add_drv, 20)
PS2_IMPORT(_import_ioman_del_drv, 21)
PS2_IMPORTS_END()

PS2_IMPORTS_BEGIN("intrman\0", 0x0102)
PS2_IMPORT(_import_intrman_register, 4)
PS2_IMPORT(_import_intrman_enable, 6)
PS2_IMPORT(_import_intrman_suspend, 17)
PS2_IMPORT(_import_intrman_resume, 18)
PS2_IMPORTS_END()

PS2_IMPORTS_BEGIN("thbase\0\0", 0x0101)
PS2_IMPORT(_import_thbase_getid, 20)
PS2_IMPORT(_import_thbase_delay, 33)
PS2_IMPORTS_END()

PS2_IMPORTS_BEGIN("thsemap\0", 0x0101)
PS2_IMPORT(_import_thsemap_create, 4)
PS2_IMPORT(_import_thsemap_delete, 5)
PS2_IMPORT(_import_thsemap_wait, 8)
PS2_IMPORT(_import_thsemap_signal, 6)
PS2_IMPORT(_import_thsemap_poll, 9)
PS2_IMPORTS_END()

PS2_IMPORTS_BEGIN("loadcore", 0x0101)
PS2_IMPORT(_import_loadcore_register, 6)
PS2_IMPORTS_END()

extern "C" {

// The residency code is the registration's sign bit (docs/analysis/11), the
// same convention ROMDRV/IOMAN use.
int _module_start(int, char **) {
    if (_import_loadcore_register(&cdvdman_exports) != 0) {
        return 1;
    }
    return moduleInit() < 0 ? 1 : 0;
}

}  // extern "C"
