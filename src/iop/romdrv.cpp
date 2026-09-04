// ROMDRV: the archive as a device, `rom0:`.
//
// docs/spec/06-iop-kernel.md IOP-4e to IOP-4g. One device named `rom`, whose
// unit -- the digit IOMAN strips off the name -- selects one of four images;
// unit 0 is this ROM's own archive, found by the self-locating scan of
// `src/boot/archive.hpp`. A file is opened by name in that archive's table,
// read from the ROM window, and seeked within; nothing is written.

#include "../boot/archive.hpp"
#include "module.hpp"

#include <stdint.h>

namespace {

using namespace ps2::archive;
using ps2::module::ExportTable;
using ps2::module::reservedHook;
using ps2::module::slot;

constexpr uintptr_t kRomSearchStart = 0xBFC00000;
constexpr uintptr_t kRomSearchEnd = 0xBFC80000;
constexpr uint32_t kUnits = 4;
constexpr uint32_t kHandles = 8;
constexpr int kErrorNotFound = -2;              // IOP-4f
constexpr int kErrorWrite = -5;
constexpr int kErrorBadUnit = -6;
constexpr int kErrorBadHandle = -9;
constexpr int kErrorHandlesFull = -12;
constexpr int kErrorBadFlags = -13;
constexpr int kErrorBadWhence = -22;
constexpr int kErrorAddFailed = -160;           // ROMDRV_ADD_FAILED [header]
constexpr int kErrorDelFailed = -161;
constexpr int kErrorBadImage = -162;
constexpr uint32_t kDeviceFilesystem = 0x10;    // IOP_DT_FS [header]

// IOP-4e: RomImg [header].
struct Image {
    uintptr_t image_start;
    const Entry *romdir_start;
    const Entry *romdir_end;
};

struct Handle {
    bool in_use;
    const uint8_t *bytes;
    uint32_t size;
    uint32_t position;
};

struct File {
    uint32_t mode;
    uint32_t unit;
    void *device;
    void *privdata;
};

Image images[kUnits];
Handle handles[kHandles];

extern "C" {
int _import_ioman_add_drv(void *device);
int _import_intrman_suspend(uint32_t *state);
int _import_intrman_resume(uint32_t state);
int _import_loadcore_register(void *table);
}

// Walk a table from `image_start`, as ARC-3 says offsets are implied.
[[nodiscard]] bool lookup(const Image &image, const char *name, Handle &out) {
    if (image.romdir_start == nullptr) {
        return false;
    }
    uintptr_t at = image.image_start;
    for (const Entry *entry = image.romdir_start; !terminator(*entry); entry++) {
        bool same = true;
        for (uint32_t k = 0; k < kNameLength; k++) {
            const char c = name[k];
            if (entry->name[k] != (c == '\0' ? '\0' : c)) {
                same = false;
                break;
            }
            if (c == '\0') {
                break;
            }
        }
        if (same) {
            out.bytes = reinterpret_cast<const uint8_t *>(at);
            out.size = entry->size;
            return true;
        }
        at += align16(entry->size);
    }
    return false;
}

int init(void *) {
    return 0;
}

int deinit(void *) {
    return 0;
}

int returnZero(File *, ...) {
    return 0;
}

int write(File *, const void *, int) {
    return kErrorWrite;                         // IOP-4g
}

// IOP-4f: open(file, name, flags), with the name already past the colon.
int open(File *file, const char *name, int flags) {
    if (file->unit >= kUnits) {
        return kErrorBadUnit;
    }
    if (flags != 1) {
        return kErrorBadFlags;
    }
    Handle found;
    if (!lookup(images[file->unit], name, found)) {
        return kErrorNotFound;
    }
    uint32_t state;
    _import_intrman_suspend(&state);
    int result = kErrorHandlesFull;
    for (uint32_t k = 0; k < kHandles; k++) {
        if (!handles[k].in_use) {
            handles[k].in_use = true;
            handles[k].bytes = found.bytes;
            handles[k].size = found.size;
            handles[k].position = 0;
            file->privdata = reinterpret_cast<void *>(k);
            result = 0;
            break;
        }
    }
    _import_intrman_resume(state);
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

// IOP-4f: a read past the end is shortened, never refused.
int read(File *file, void *buffer, int size) {
    Handle *handle = handleOf(file);
    if (handle == nullptr) {
        return kErrorBadHandle;
    }
    uint32_t length = size < 0 ? 0 : static_cast<uint32_t>(size);
    if (length > handle->size - handle->position) {
        length = handle->size - handle->position;
    }
    const uint8_t *from = handle->bytes + handle->position;
    auto *to = static_cast<uint8_t *>(buffer);
    for (uint32_t k = 0; k < length; k++) {
        to[k] = from[k];
    }
    handle->position += length;
    return static_cast<int>(length);
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

// IOP-4a's table, IOP-4g's stubs.
struct Operations {
    int (*init)(void *);
    int (*deinit)(void *);
    int (*format)(File *, ...);
    int (*open)(File *, const char *, int);
    int (*close)(File *);
    int (*read)(File *, void *, int);
    int (*write)(File *, const void *, int);
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
    init, deinit, returnZero, open, close, read, write, lseek,
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

const char kName[] = "rom";                     // IOP-4e: no digit
const char kDescription[] = "ROM/Flash";
Device device = {kName, kDeviceFilesystem, 0x0103, kDescription, &operations};

// IOP-4e: an image is bound to a unit by the address of its table, found by
// the scan the boot used (ARC-4).
[[nodiscard]] bool bindImage(uint32_t unit, uintptr_t search_start, uintptr_t search_end) {
    const Found romdir = find(search_start, search_end, packName("ROMDIR"));
    if (romdir.address == 0) {
        return false;
    }
    const auto *table = reinterpret_cast<const Entry *>(romdir.address);
    images[unit].romdir_start = table;
    images[unit].image_start = romdir.address - align16(table->size);
    const Entry *end = table;
    while (!terminator(*end)) {
        end++;
    }
    images[unit].romdir_end = end;
    return true;
}

// IOP-4g: romdrv's own exports.
int romAddDevice(uint32_t unit, uintptr_t image) {
    if (unit >= kUnits) {
        return kErrorAddFailed;
    }
    if (!bindImage(unit, image, image + 0x80000)) {
        return kErrorBadImage;
    }
    return 0;
}

int romDelDevice(uint32_t unit) {
    if (unit >= kUnits || images[unit].romdir_start == nullptr) {
        return kErrorDelFailed;
    }
    images[unit].romdir_start = nullptr;
    return 0;
}

int initDevice() {
    for (Handle &handle : handles) {
        handle.in_use = false;
    }
    for (Image &image : images) {
        image.romdir_start = nullptr;
    }
    if (!bindImage(0, kRomSearchStart, kRomSearchEnd)) {
        return -1;
    }
    return _import_ioman_add_drv(&device);
}

[[gnu::used]] ExportTable<6> romdrv_exports = {
    ps2::module::kExportMagic,
    0,
    0x0201,                                     // IRX-2b: romdrv 2.01
    0,
    {'r', 'o', 'm', 'd', 'r', 'v', 0, 0},
    {
        slot(initDevice),               // 0  the init, re-exported
        slot(reservedHook),             // 1
        slot(reservedHook),             // 2
        slot(reservedHook),             // 3
        slot(romAddDevice),             // 4
        slot(romDelDevice),             // 5
        nullptr,
    },
};

}  // namespace

PS2_IMPORTS_BEGIN("ioman\0\0\0", 0x0102)
PS2_IMPORT(_import_ioman_add_drv, 20)
PS2_IMPORTS_END()

PS2_IMPORTS_BEGIN("intrman\0", 0x0102)
PS2_IMPORT(_import_intrman_suspend, 17)
PS2_IMPORT(_import_intrman_resume, 18)
PS2_IMPORTS_END()

PS2_IMPORTS_BEGIN("loadcore", 0x0101)
PS2_IMPORT(_import_loadcore_register, 6)
PS2_IMPORTS_END()

extern "C" {

// The residency code is the registration's sign bit, as the reference
// derives it (docs/analysis/11).
int _module_start(int, char **) {
    if (_import_loadcore_register(&romdrv_exports) < 0) {
        return 1;
    }
    return initDevice() < 0 ? 1 : 0;
}

}  // extern "C"
