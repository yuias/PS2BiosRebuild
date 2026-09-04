// IOMAN: the IOP's file and device manager.
//
// docs/spec/06-iop-kernel.md IOP-4a to IOP-4d. A driver registers a device
// descriptor -- a name, a type and a table of operations -- and a path of
// the form `name<unit>:rest` is parsed here into the device, its unit and
// the rest, which is what the driver's own `open` receives along with a
// file record this module owns. Every other call takes the descriptor the
// open returned and goes straight through the driver's table.

#include "module.hpp"

#include <stdint.h>

namespace {

using ps2::module::ExportTable;
using ps2::module::reservedHook;
using ps2::module::slot;

constexpr uint32_t kDrivers = 16;               // IOP-4b
constexpr uint32_t kFiles = 16;                 // IOP-4c
constexpr int kErrorBadFd = -9;
constexpr int kErrorNoDevice = -19;             // IOP-4d
constexpr int kErrorBadWhence = -22;
constexpr int kErrorNoFd = -24;

struct File;

// IOP-4a: the operations, in the slot order the header fixes.
struct Operations {
    int (*init)(void *device);
    int (*deinit)(void *device);
    int (*format)(File *file, ...);
    int (*open)(File *file, const char *name, int flags);
    int (*close)(File *file);
    int (*read)(File *file, void *buffer, int size);
    int (*write)(File *file, const void *buffer, int size);
    int (*lseek)(File *file, int offset, int whence);
    int (*ioctl)(File *file, int command, void *argument);
    int (*remove)(File *file, const char *name);
    int (*mkdir)(File *file, const char *name);
    int (*rmdir)(File *file, const char *name);
    int (*dopen)(File *file, const char *name);
    int (*dclose)(File *file);
    int (*dread)(File *file, void *entry);
    int (*getstat)(File *file, const char *name, void *stat);
    int (*chstat)(File *file, const char *name, void *stat, unsigned mask);
};

// IOP-4a: iop_device_t [header].
struct Device {
    const char *name;
    uint32_t type;
    uint32_t version;
    const char *description;
    Operations *ops;
};

// IOP-4c: iop_file_t [header], sixteen bytes; `device` null when free.
struct File {
    uint32_t mode;
    uint32_t unit;
    Device *device;
    void *privdata;
};

Device *drivers[kDrivers];
File files[kFiles];

[[nodiscard]] bool sameName(const char *a, const char *b, uint32_t length) {
    for (uint32_t k = 0; k < length; k++) {
        if (a[k] != b[k]) {
            return false;
        }
    }
    return b[length] == '\0';
}

// IOP-4c: split `name<digits>:rest` -- the digits, walked backwards from the
// colon, are the unit and are not part of the name the table is matched on.
struct Parsed {
    Device *device;
    uint32_t unit;
    const char *rest;
};

[[nodiscard]] Parsed parse(const char *path) {
    Parsed parsed = {nullptr, 0, nullptr};
    uint32_t colon = 0;
    while (path[colon] != '\0' && path[colon] != ':') {
        colon++;
    }
    if (path[colon] != ':') {
        return parsed;
    }
    uint32_t name_length = colon;
    uint32_t unit = 0;
    uint32_t scale = 1;
    while (name_length > 0 && path[name_length - 1] >= '0' && path[name_length - 1] <= '9') {
        name_length--;
        unit += static_cast<uint32_t>(path[name_length] - '0') * scale;
        scale *= 10;
    }
    for (Device *device : drivers) {
        if (device != nullptr && sameName(path, device->name, name_length)) {
            parsed.device = device;
            break;
        }
    }
    parsed.unit = unit;
    parsed.rest = path + colon + 1;
    return parsed;
}

[[nodiscard]] File *fileOf(int fd) {
    if (fd < 0 || static_cast<uint32_t>(fd) >= kFiles || files[fd].device == nullptr) {
        return nullptr;
    }
    return &files[fd];
}

[[nodiscard]] int allocateFile() {
    for (uint32_t k = 0; k < kFiles; k++) {
        if (files[k].device == nullptr) {
            return static_cast<int>(k);
        }
    }
    return kErrorNoFd;
}

// IOP-4d: open(path, flags) -> fd | error.
int open(const char *path, int flags) {
    const Parsed parsed = parse(path);
    if (parsed.device == nullptr) {
        return kErrorNoDevice;
    }
    const int fd = allocateFile();
    if (fd < 0) {
        return fd;
    }
    File &file = files[fd];
    file.mode = static_cast<uint32_t>(flags);
    file.unit = parsed.unit;
    file.device = parsed.device;
    file.privdata = nullptr;
    const int result = parsed.device->ops->open(&file, parsed.rest, flags);
    if (result < 0) {
        file.device = nullptr;
        return result;                          // the driver's own answer
    }
    return fd;
}

int close(int fd) {
    File *file = fileOf(fd);
    if (file == nullptr) {
        return kErrorBadFd;
    }
    const int result = file->device->ops->close(file);
    file->device = nullptr;
    return result;
}

int read(int fd, void *buffer, int size) {
    File *file = fileOf(fd);
    if (file == nullptr) {
        return kErrorBadFd;
    }
    return file->device->ops->read(file, buffer, size);
}

int write(int fd, const void *buffer, int size) {
    File *file = fileOf(fd);
    if (file == nullptr) {
        return kErrorBadFd;
    }
    return file->device->ops->write(file, buffer, size);
}

int lseek(int fd, int offset, int whence) {
    File *file = fileOf(fd);
    if (file == nullptr) {
        return kErrorBadFd;
    }
    if (whence < 0 || whence > 2) {
        return kErrorBadWhence;
    }
    return file->device->ops->lseek(file, offset, whence);
}

int ioctl(int fd, int command, void *argument) {
    File *file = fileOf(fd);
    if (file == nullptr) {
        return kErrorBadFd;
    }
    return file->device->ops->ioctl(file, command, argument);
}

// The path-only calls: a record is made for the driver's sake and released
// afterwards, whatever it answered (IOP-4d's pass-through).
template <typename Call>
[[nodiscard]] int withPath(const char *path, Call call) {
    const Parsed parsed = parse(path);
    if (parsed.device == nullptr) {
        return kErrorNoDevice;
    }
    const int fd = allocateFile();
    if (fd < 0) {
        return fd;
    }
    File &file = files[fd];
    file.mode = 0;
    file.unit = parsed.unit;
    file.device = parsed.device;
    file.privdata = nullptr;
    const int result = call(file, parsed.rest);
    file.device = nullptr;
    return result;
}

int remove(const char *path) {
    return withPath(path, [](File &file, const char *rest) {
        return file.device->ops->remove(&file, rest);
    });
}

int mkdir(const char *path) {
    return withPath(path, [](File &file, const char *rest) {
        return file.device->ops->mkdir(&file, rest);
    });
}

int rmdir(const char *path) {
    return withPath(path, [](File &file, const char *rest) {
        return file.device->ops->rmdir(&file, rest);
    });
}

int dopen(const char *path) {
    const Parsed parsed = parse(path);
    if (parsed.device == nullptr) {
        return kErrorNoDevice;
    }
    const int fd = allocateFile();
    if (fd < 0) {
        return fd;
    }
    File &file = files[fd];
    file.mode = 0;
    file.unit = parsed.unit;
    file.device = parsed.device;
    file.privdata = nullptr;
    const int result = parsed.device->ops->dopen(&file, parsed.rest);
    if (result < 0) {
        file.device = nullptr;
        return result;
    }
    return fd;
}

int dclose(int fd) {
    File *file = fileOf(fd);
    if (file == nullptr) {
        return kErrorBadFd;
    }
    const int result = file->device->ops->dclose(file);
    file->device = nullptr;
    return result;
}

int dread(int fd, void *entry) {
    File *file = fileOf(fd);
    if (file == nullptr) {
        return kErrorBadFd;
    }
    return file->device->ops->dread(file, entry);
}

int getstat(const char *path, void *stat) {
    return withPath(path, [stat](File &file, const char *rest) {
        return file.device->ops->getstat(&file, rest, stat);
    });
}

int chstat(const char *path, void *stat, unsigned mask) {
    return withPath(path, [stat, mask](File &file, const char *rest) {
        return file.device->ops->chstat(&file, rest, stat, mask);
    });
}

int format(const char *path) {
    return withPath(path, [](File &file, const char *) {
        return file.device->ops->format(&file);
    });
}

// IOP-4a: AddDrv keeps the pointer and lets the driver's `init` veto it.
int addDrv(Device *device) {
    for (Device *&entry : drivers) {
        if (entry == nullptr) {
            entry = device;
            if (device->ops->init(device) < 0) {
                entry = nullptr;
                return -1;
            }
            return 0;
        }
    }
    return -1;
}

int delDrv(const char *name) {
    for (Device *&entry : drivers) {
        if (entry != nullptr && sameName(name, entry->name, 0xFFFFFFFFu / 2)) {
            entry->ops->deinit(entry);
            entry = nullptr;
            return 0;
        }
    }
    return 0;                                   // IOP-4a: always 0
}

PS2_EXPORT_TABLE ExportTable<25> ioman_exports = {
    ps2::module::kExportMagic,
    0,
    0x0102,
    0,
    {'i', 'o', 'm', 'a', 'n', 0, 0, 0},
    {
        slot(reservedHook),             // 0
        slot(reservedHook),             // 1
        slot(reservedHook),             // 2
        slot(reservedHook),             // 3
        slot(open),                     // 4
        slot(close),                    // 5
        slot(read),                     // 6
        slot(write),                    // 7
        slot(lseek),                    // 8
        slot(ioctl),                    // 9
        slot(remove),                   // 10
        slot(mkdir),                    // 11
        slot(rmdir),                    // 12
        slot(dopen),                    // 13
        slot(dclose),                   // 14
        slot(dread),                    // 15
        slot(getstat),                  // 16
        slot(chstat),                   // 17
        slot(format),                   // 18
        slot(reservedHook),             // 19
        slot(addDrv),                   // 20
        slot(delDrv),                   // 21
        slot(reservedHook),             // 22
        slot(reservedHook),             // 23
        slot(reservedHook),             // 24
        nullptr,
    },
};

}  // namespace

PS2_IMPORTS_BEGIN("loadcore", 0x0101)
PS2_IMPORT(_import_loadcore_register, 6)
PS2_IMPORTS_END()

extern "C" {

int _import_loadcore_register(void *table);

int _module_start(int, char **) {
    for (Device *&entry : drivers) {
        entry = nullptr;
    }
    for (File &file : files) {
        file.device = nullptr;
    }
    // IRX-10a, the reference's way: the module registers its own library
    // from its entry. The loader has already done so (docs/implementation.md),
    // and LOADCORE answers yes to a table it finds; the call is made all the
    // same, because an emulator watching for it -- PCSX2 learns where
    // `ioman` is from this call, for its `host:` device -- sees it here.
    _import_loadcore_register(&ioman_exports);
    return 0;                                   // resident
}

}  // extern "C"
