// MODLOAD: loading a module by name, on request.
//
// docs/spec/06-iop-kernel.md IOP-5b to IOP-5e. `LoadStartModule` reads the
// whole file through IOMAN into a buffer, has LOADCORE probe it, allocates
// its final place with the module record at the head (IOP-5c), has LOADCORE
// place, link, flush and register it, builds argc/argv from the name and
// the argument bytes (IOP-5d), and calls the entry. What the entry returns
// goes back unmodified through `*result`; the answer is the module's id, or
// one of IOP-5e's errors.

#include "loader.hpp"
#include "module.hpp"

#include <stdint.h>

namespace {

using ps2::module::ExportTable;
using ps2::module::reservedHook;
using ps2::module::slot;
using ps2::loader::ExecutableInfo;
using ps2::loader::ModuleRecord;

// kerr.h [header], IOP-5e
constexpr int kLinkError = -200;
constexpr int kIllegalObject = -201;
constexpr int kNoFile = -203;
constexpr int kFileError = -204;
constexpr int kNoMemory = -400;

constexpr int kOpenRead = 1;                    // O_RDONLY [header]
constexpr uint32_t kArgvMax = 16;
constexpr uint32_t kArgBytesMax = 256;

extern "C" {
int _import_ioman_open(const char *path, int flags);
int _import_ioman_close(int fd);
int _import_ioman_read(int fd, void *buffer, int size);
int _import_ioman_lseek(int fd, int offset, int whence);
int _import_sysmem_allocate(uint32_t mode, uint32_t size, uint32_t address);
int _import_sysmem_release(uint32_t address);
int _import_loadcore_flush();
int _import_loadcore_link(uint8_t *base, const ExecutableInfo *info);
int _import_loadcore_register(ModuleRecord *record);
int _import_loadcore_probe(const uint8_t *image, ExecutableInfo *info);
int _import_loadcore_load(const uint8_t *image, ExecutableInfo *info);
}

// IOP-5g: what makes a path illegal is not read yet; nothing is.
int isIllegalBootDevice(const char *) {
    return 0;
}

// The file, whole, in a buffer of its own size (IOP-5b).
[[nodiscard]] int readFile(const char *path, uint8_t **buffer, uint32_t *size) {
    const int fd = _import_ioman_open(path, kOpenRead);
    if (fd < 0) {
        return kNoFile;
    }
    const int length = _import_ioman_lseek(fd, 0, 2);
    if (length <= 0) {
        _import_ioman_close(fd);
        return kFileError;
    }
    const auto block = static_cast<uint32_t>(
        _import_sysmem_allocate(1, static_cast<uint32_t>(length), 0));
    if (block == 0) {
        _import_ioman_close(fd);
        return kNoMemory;
    }
    _import_ioman_lseek(fd, 0, 0);
    const int got = _import_ioman_read(fd, reinterpret_cast<void *>(block), length);
    _import_ioman_close(fd);
    if (got != length) {
        _import_sysmem_release(block);
        return kFileError;
    }
    *buffer = reinterpret_cast<uint8_t *>(block);
    *size = static_cast<uint32_t>(length);
    return 0;
}

// The name after the device, for argv[0] and the record.
[[nodiscard]] const char *baseName(const char *path) {
    const char *name = path;
    for (const char *at = path; *at != '\0'; at++) {
        if (*at == ':' || *at == '/') {
            name = at + 1;
        }
    }
    return name;
}

// IOP-5d: argv[0] is the name, then each NUL-terminated token of the
// argument bytes, and a NUL pointer after the last.
[[nodiscard]] uint32_t buildArguments(const char *name, uint32_t arglen, const char *args,
                                      char *copy, char **argv) {
    uint32_t used = 0;
    uint32_t argc = 0;
    argv[argc++] = copy;
    for (const char *at = name; *at != '\0' && used + 1 < kArgBytesMax; at++) {
        copy[used++] = *at;
    }
    copy[used++] = '\0';
    if (args != nullptr && arglen > 0) {
        if (arglen > kArgBytesMax - used) {
            arglen = kArgBytesMax - used;
        }
        const uint32_t start = used;
        for (uint32_t k = 0; k < arglen; k++) {
            copy[used++] = args[k];
        }
        copy[used - 1] = '\0';
        bool token = false;
        for (uint32_t k = start; k < used && argc < kArgvMax - 1; k++) {
            if (!token && copy[k] != '\0') {
                argv[argc++] = copy + k;
                token = true;
            } else if (copy[k] == '\0') {
                token = false;
            }
        }
    }
    argv[argc] = nullptr;
    return argc;
}

// Ordinal 7: LoadStartModule(path, arglen, args, result) -> id | error.
int loadStartModule(const char *path, uint32_t arglen, const char *args, int *result) {
    if (isIllegalBootDevice(path) != 0) {
        return kIllegalObject;
    }
    uint8_t *file;
    uint32_t file_size;
    const int read_result = readFile(path, &file, &file_size);
    if (read_result < 0) {
        return read_result;
    }

    ExecutableInfo info;
    if (_import_loadcore_probe(file, &info) < 0 || info.type < 1 || info.type > 4) {
        _import_sysmem_release(reinterpret_cast<uintptr_t>(file));
        return kIllegalObject;
    }
    const auto block = static_cast<uint32_t>(
        _import_sysmem_allocate(0, sizeof(ModuleRecord) + info.memory_size, 0));
    if (block == 0) {
        _import_sysmem_release(reinterpret_cast<uintptr_t>(file));
        return kNoMemory;
    }
    auto *record = reinterpret_cast<ModuleRecord *>(block);
    info.base = block + sizeof(ModuleRecord);
    if (_import_loadcore_load(file, &info) < 0) {
        _import_sysmem_release(block);
        _import_sysmem_release(reinterpret_cast<uintptr_t>(file));
        return kIllegalObject;
    }
    _import_sysmem_release(reinterpret_cast<uintptr_t>(file));

    // IOP-5b: link, then flush, then register.
    if (_import_loadcore_link(reinterpret_cast<uint8_t *>(info.base), &info) < 0) {
        _import_sysmem_release(block);
        return kLinkError;
    }
    _import_loadcore_flush();
    record->next = nullptr;
    record->name = baseName(path);
    record->version = 0;
    record->newflags = 0;
    record->flags = 0;
    record->entry = info.entry;
    record->gp = info.gp;
    record->text_start = info.base;
    record->text_size = info.text_size;
    record->data_size = info.data_size;
    record->bss_size = info.bss_size;
    _import_loadcore_register(record);

    char copy[kArgBytesMax];
    char *argv[kArgvMax];
    const uint32_t argc = buildArguments(baseName(path), arglen, args, copy, argv);
    const uint32_t answer = ps2::loader::callEntry(info.entry, info.gp, argc, argv,
                                                   reinterpret_cast<uintptr_t>(record));
    if (result != nullptr) {
        *result = static_cast<int>(answer);
    }
    return record->id;
}

int unimplemented() {
    return -1;
}

[[gnu::used]] ExportTable<16> modload_exports = {
    ps2::module::kExportMagic,
    0,
    0x0101,
    0,
    {'m', 'o', 'd', 'l', 'o', 'a', 'd', 0},
    {
        slot(reservedHook),             // 0
        slot(reservedHook),             // 1
        slot(reservedHook),             // 2
        slot(reservedHook),             // 3
        slot(unimplemented),            // 4  ReBootStart
        slot(unimplemented),            // 5  LoadModuleAddress
        slot(unimplemented),            // 6  LoadModule
        slot(loadStartModule),          // 7  LoadStartModule
        slot(unimplemented),            // 8  StartModule
        slot(unimplemented),            // 9  LoadModuleBufferAddress
        slot(unimplemented),            // 10 LoadModuleBuffer
        slot(unimplemented),            // 11 LoadStartKelfModule
        slot(unimplemented),            // 12 SetSecrmanCallbacks
        slot(unimplemented),            // 13 SetCheckKelfPathCallback
        slot(unimplemented),            // 14 GetLoadfileCallbacks
        slot(isIllegalBootDevice),      // 15
        nullptr,
    },
};

}  // namespace

PS2_IMPORTS_BEGIN("ioman\0\0\0", 0x0102)
PS2_IMPORT(_import_ioman_open, 4)
PS2_IMPORT(_import_ioman_close, 5)
PS2_IMPORT(_import_ioman_read, 6)
PS2_IMPORT(_import_ioman_lseek, 8)
PS2_IMPORTS_END()

PS2_IMPORTS_BEGIN("sysmem\0\0", 0x0101)
PS2_IMPORT(_import_sysmem_allocate, 4)
PS2_IMPORT(_import_sysmem_release, 5)
PS2_IMPORTS_END()

PS2_IMPORTS_BEGIN("loadcore", 0x0101)
PS2_IMPORT(_import_loadcore_flush, 4)
PS2_IMPORT(_import_loadcore_link, 8)
PS2_IMPORT(_import_loadcore_register, 16)
PS2_IMPORT(_import_loadcore_probe, 22)
PS2_IMPORT(_import_loadcore_load, 23)
PS2_IMPORTS_END()

extern "C" {

int _module_start(int, char **) {
    return 0;                           // resident
}

}  // extern "C"
