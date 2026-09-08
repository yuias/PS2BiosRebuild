// MODLOAD: loading a module by name, on request.
//
// docs/spec/06-iop-kernel.md IOP-5b to IOP-5e. `LoadStartModule` reads the
// whole file through IOMAN into a buffer, has LOADCORE probe it, allocates
// its final place with the module record at the head (IOP-5c), has LOADCORE
// place, link, flush and register it, builds argc/argv from the name and
// the argument bytes (IOP-5d), and calls the entry. What the entry returns
// goes back unmodified through `*result`; the answer is the module's id, or
// one of IOP-5e's errors.

#include "../boot/archive.hpp"
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
constexpr int kUnknownModule = -202;
constexpr int kNoFile = -203;
constexpr int kFileError = -204;
constexpr int kNoMemory = -400;

constexpr int kOpenRead = 1;                    // O_RDONLY [header]
constexpr uint32_t kArgvMax = 16;
constexpr uint32_t kArgBytesMax = 256;

// Modules loaded by ordinals 6/10 and not yet started, for ordinal 8's by-id
// lookup. The reference finds a record through LOADCORE's internals
// (ordinal 3), which has no spec line yet; this list is this module's own,
// the ids are still LOADCORE's, and docs/implementation.md records the
// deviation.
constexpr uint32_t kLoadedMax = 16;
ModuleRecord *loaded[kLoadedMax];

// --- the reboot core (docs/analysis/45 §2) -----------------------------------
//
// Ordinal 4 does not do the work; it traps, through `intrman` 14, into the
// function below, which runs in the exception's own context and never comes
// back. That indirection is the reference's, and the reason for it is the
// stack: what re-enters `IOPBOOT` cannot be standing on the stack of a thread
// whose module is about to be reloaded over it.
constexpr uintptr_t kRomSearchStart = 0xBFC00000;
constexpr uintptr_t kRomSearchEnd = 0xBFC80000;  // BOOT-6b: the first 512 KiB

// BOOT-4 step 2's own value, in `src/boot/reset.S`: the top of the 2 MiB a
// retail machine has. The boot list's modules are placed from `0x800` up and
// the boot's own words sit at `0x1F8100`, so this is above everything a boot
// writes and below the block the stack grows down through.
constexpr uintptr_t kBootStackTop = 0x001FFFF0;

// §2: an update reboot leaves the argument string at this absolute address
// and hands `IOPBOOT` the address rather than the string, because the copy
// has to survive the reload of whatever module the string was living in.
constexpr uintptr_t kCommandLine = 0x480;
constexpr uint32_t kCommandLineMax = 0x80;

constexpr uint32_t kModeSoftReboot = 1;         // BOOT-8b
constexpr uint32_t kModeUpdateStage = 2;
constexpr uint32_t kKeyBootMode = 4;
constexpr uint32_t kKeyCommandLine = 5;

constexpr uint32_t kRamMiB = 2;                 // BOOT-4 step 5's retail latch

extern "C" {
uint32_t _import_intrman_invoke_in_kmode(uint32_t function, uint32_t a, uint32_t b,
                                         uint32_t c);
uint32_t *_import_loadcore_boot_record(uint32_t key);
int _import_loadcore_add_bootup_callback(void (*function)(), int priority,
                                        void *argument);
// Ordinal 6, the versioned library registration -- not ordinal 16, which
// registers a module *record* and is what `_import_loadcore_register` names.
int _import_loadcore_register_library(void *table);
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

// IOP-5b: probe, allocate, load, link, flush, register. The raw image is the
// caller's to free. Answers the record, or a negative error.
[[nodiscard]] int loadImage(const uint8_t *file, const char *name, ModuleRecord **out) {
    ExecutableInfo info;
    if (_import_loadcore_probe(file, &info) < 0 || info.type < 1 || info.type > 4) {
        return kIllegalObject;
    }
    const auto block = static_cast<uint32_t>(
        _import_sysmem_allocate(0, sizeof(ModuleRecord) + info.memory_size, 0));
    if (block == 0) {
        return kNoMemory;
    }
    auto *record = reinterpret_cast<ModuleRecord *>(block);
    info.base = block + sizeof(ModuleRecord);
    if (_import_loadcore_load(file, &info) < 0) {
        _import_sysmem_release(block);
        return kIllegalObject;
    }

    // IOP-5b: link, then flush, then register.
    if (_import_loadcore_link(reinterpret_cast<uint8_t *>(info.base), &info) < 0) {
        _import_sysmem_release(block);
        return kLinkError;
    }
    _import_loadcore_flush();
    record->next = nullptr;
    record->name = name;
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
    *out = record;
    return 0;
}

// IOP-5d: the entry call, with `*result` the entry's raw return.
int startRecord(ModuleRecord *record, const char *name, uint32_t arglen, const char *args,
                int *result) {
    char copy[kArgBytesMax];
    char *argv[kArgvMax];
    const uint32_t argc = buildArguments(name, arglen, args, copy, argv);
    const uint32_t answer = ps2::loader::callEntry(record->entry, record->gp, argc, argv,
                                                   reinterpret_cast<uintptr_t>(record));
    if (result != nullptr) {
        *result = static_cast<int>(answer);
    }
    return record->id;
}

// The file at `path`, loaded and registered but not started.
[[nodiscard]] int loadFromPath(const char *path, ModuleRecord **out) {
    if (isIllegalBootDevice(path) != 0) {
        return kIllegalObject;
    }
    uint8_t *file;
    uint32_t file_size;
    const int read_result = readFile(path, &file, &file_size);
    if (read_result < 0) {
        return read_result;
    }
    const int loaded_result = loadImage(file, baseName(path), out);
    _import_sysmem_release(reinterpret_cast<uintptr_t>(file));
    return loaded_result;
}

// Ordinal 7: LoadStartModule(path, arglen, args, result) -> id | error.
int loadStartModule(const char *path, uint32_t arglen, const char *args, int *result) {
    ModuleRecord *record;
    const int loaded_result = loadFromPath(path, &record);
    if (loaded_result < 0) {
        return loaded_result;
    }
    return startRecord(record, record->name, arglen, args, result);
}

// A record loaded but not started is held until ordinal 8 asks for it. Out
// of slots, the module is loaded and registered but cannot be started later;
// the id is still answered, as the reference's list has no such limit.
void holdLoaded(ModuleRecord *record) {
    for (uint32_t k = 0; k < kLoadedMax; k++) {
        if (loaded[k] == nullptr) {
            loaded[k] = record;
            return;
        }
    }
}

// Ordinal 6: LoadModule(path) -> id | error.
int loadModule(const char *path) {
    ModuleRecord *record;
    const int loaded_result = loadFromPath(path, &record);
    if (loaded_result < 0) {
        return loaded_result;
    }
    holdLoaded(record);
    return record->id;
}

// Ordinal 10: LoadModuleBuffer(buffer) -> id | error. The image is already
// in memory and stays the caller's; the record is named from the image's
// own `.iopmod` name (spec/02 IRX-2, offset 26), there being no path.
int loadModuleBuffer(const uint8_t *buffer) {
    ps2::loader::Segments segments;
    if (!ps2::loader::readHeaders(buffer, segments)) {
        return kIllegalObject;
    }
    const auto *name = reinterpret_cast<const char *>(buffer + segments.iopmod_offset + 26);
    ModuleRecord *record;
    const int loaded_result = loadImage(buffer, name, &record);
    if (loaded_result < 0) {
        return loaded_result;
    }
    holdLoaded(record);
    return record->id;
}

// Ordinal 8: StartModule(id, name, arglen, args, result) -> id | error.
// IOP-5e: an id that names no loaded, unstarted module is -202. The `name`
// argument stands in for the path ordinal 7 would have had, so it is what
// argv[0] is built from when given (docs/analysis/39 §3: both ordinals end
// in the same argument builder); the record keeps its own name.
int startModule(int id, const char *name, uint32_t arglen, const char *args, int *result) {
    for (uint32_t k = 0; k < kLoadedMax; k++) {
        ModuleRecord *record = loaded[k];
        if (record == nullptr || record->id != id) {
            continue;
        }
        loaded[k] = nullptr;
        return startRecord(record, name != nullptr ? baseName(name) : record->name, arglen,
                           args, result);
    }
    return kUnknownModule;
}

int unimplemented() {
    return -1;
}

// The reboot core, reached only through `intrman` 14's trap. It runs in the
// exception's own context, on the interrupt stack, and does not return.
//
// **The reference tears every resident module down first** and this does not:
// its walk calls a per-module hook out of each record, reached through a
// `loadcore` ordinal this project has not identified, and no module here
// exposes one to call. The walk is therefore absent rather than empty, and
// what follows is the rest of §2 -- the argument's copy to a fixed address,
// the mode, and the re-entry into `IOPBOOT`. It also does not re-apply
// BOOT-4 step 1's bus table: nothing between here and `IOPBOOT` disturbs it,
// where on hardware the reference is guarding against a controller that a
// half-finished transfer left in another state.
uint32_t rebootCore(uint32_t /* tag */, uint32_t argument, uint32_t mode) {
    const auto *arg = reinterpret_cast<const char *>(argument);
    uint32_t boot_mode = kModeSoftReboot;
    uint32_t command_line = 0;
    if (arg != nullptr && arg[0] != '\0') {
        // §2: the string is copied out of whatever module is holding it,
        // because that module is about to be loaded over.
        auto *at = reinterpret_cast<char *>(kCommandLine);
        uint32_t k = 0;
        for (; k + 1 < kCommandLineMax && arg[k] != '\0'; k++) {
            at[k] = arg[k];
        }
        at[k] = '\0';
        boot_mode = (mode & 0xFF00) | kModeUpdateStage;
        command_line = kCommandLine;
    }

    const ps2::archive::Found boot = ps2::archive::find(
        kRomSearchStart, kRomSearchEnd, ps2::archive::packName("IOPBOOT"));
    if (boot.address == 0) {
        for (;;) {                              // BOOT-6d: a terminal stop
        }
    }

    // The stack has to move before the jump: this one belongs to a thread
    // whose module the reload is about to overwrite. `$t0`-`$t3` are named in
    // the clobber list so the operands cannot land in them, which is what
    // makes it safe to fill `$a0`-`$a3` from copies rather than directly.
    asm volatile(
        ".set noreorder\n\t"
        "move $t0, %0\n\t"
        "move $t1, %1\n\t"
        "move $t2, %2\n\t"
        "move $t3, %3\n\t"
        "move $a0, $t1\n\t"
        "move $a1, $t2\n\t"
        "move $a2, $t3\n\t"
        "move $a3, $zero\n\t"
        "lui  $sp, 0x1f\n\t"
        "ori  $sp, $sp, 0xfff0\n\t"
        "jr   $t0\n\t"
        "nop\n\t"
        ".set reorder\n\t"
        :
        : "r"(static_cast<uint32_t>(boot.address)), "r"(kRamMiB),
          "r"(boot_mode), "r"(command_line)
        : "$4", "$5", "$6", "$7", "$8", "$9", "$10", "$11", "memory");
    __builtin_unreachable();
}

// The second stage's bootup callback (docs/analysis/45 §2). It runs after the
// boot list has been loaded, reads the command line out of boot record key 5,
// and loads its first word as a module with the rest as that module's
// arguments -- which for a title's reboot is `rom0:UDNL` and the image it
// wants merged. The reference dispatches on the reboot mode's high byte: `0`
// is this path, `1` a second one this project has not read.
//
// Nothing is loaded here that `LoadStartModule` could not load; what the
// reference's `UDNL` then does with its arguments is the merge, and that does
// not exist yet.
void mode2Bootup() {
    const uint32_t *record = _import_loadcore_boot_record(kKeyCommandLine);
    if (record == nullptr) {
        // The reference panics here: an update reboot with no file name is a
        // request that cannot be answered.
        return;
    }
    const auto *line = reinterpret_cast<const char *>(record[1]);
    if (line == nullptr) {
        return;
    }
    const uint32_t *mode_record = _import_loadcore_boot_record(kKeyBootMode);
    if (mode_record == nullptr || ((*mode_record & 0xFFFF) >> 8) != 0) {
        return;                                 // the path that is not read
    }

    // BOOT-8b: key 5 is the line itself, untokenised, so the split happens
    // here. The first token is the module to load; IOP-5d wants the rest as
    // NUL-separated bytes with a length, which is what splitting on spaces
    // into one buffer produces directly -- the terminator each token needs is
    // the separator the next one is behind.
    char name[kArgBytesMax];
    char args[kArgBytesMax];
    uint32_t at = 0;
    while (line[at] == ' ') {
        at++;
    }
    uint32_t n = 0;
    while (line[at] != '\0' && line[at] != ' ' && n + 1 < kArgBytesMax) {
        name[n++] = line[at++];
    }
    name[n] = '\0';
    if (n == 0) {
        return;
    }
    uint32_t length = 0;
    while (line[at] != '\0' && length + 1 < kArgBytesMax) {
        while (line[at] == ' ') {
            at++;
        }
        if (line[at] == '\0') {
            break;
        }
        while (line[at] != '\0' && line[at] != ' ' && length + 2 < kArgBytesMax) {
            args[length++] = line[at++];
        }
        args[length++] = '\0';
    }
    int result = 0;
    (void)loadStartModule(name, length, length != 0 ? args : nullptr, &result);
}

// Ordinal 4, `ReBootStart(argument, mode)`: the trap, and nothing else. An
// empty argument is a plain soft reboot; anything else names a loader and an
// image, and starts the intermediate stage of an update reboot. The first
// argument the core takes is the reference's `"modload"` tag, which nothing
// here reads.
int reBootStart(const char *argument, uint32_t mode) {
    return static_cast<int>(_import_intrman_invoke_in_kmode(
        reinterpret_cast<uint32_t>(rebootCore), 0,
        reinterpret_cast<uint32_t>(argument), mode));
}

PS2_EXPORT_TABLE ExportTable<16> modload_exports = {
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
        slot(reBootStart),              // 4  ReBootStart
        slot(unimplemented),            // 5  LoadModuleAddress
        slot(loadModule),               // 6  LoadModule
        slot(loadStartModule),          // 7  LoadStartModule
        slot(startModule),              // 8  StartModule
        slot(unimplemented),            // 9  LoadModuleBufferAddress
        slot(loadModuleBuffer),         // 10 LoadModuleBuffer
        slot(unimplemented),            // 11 LoadStartKelfModule
        slot(unimplemented),            // 12 SetSecrmanCallbacks
        slot(unimplemented),            // 13 SetCheckKelfPathCallback
        slot(unimplemented),            // 14 GetLoadfileCallbacks
        slot(isIllegalBootDevice),      // 15
        nullptr,
    },
};

}  // namespace

PS2_IMPORTS_BEGIN("intrman\0", 0x0102)
PS2_IMPORT(_import_intrman_invoke_in_kmode, 14)
PS2_IMPORTS_END()

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
PS2_IMPORT(_import_loadcore_register_library, 6)
PS2_IMPORT(_import_loadcore_flush, 4)
PS2_IMPORT(_import_loadcore_link, 8)
PS2_IMPORT(_import_loadcore_boot_record, 12)
PS2_IMPORT(_import_loadcore_register, 16)
PS2_IMPORT(_import_loadcore_add_bootup_callback, 20)
PS2_IMPORT(_import_loadcore_probe, 22)
PS2_IMPORT(_import_loadcore_load, 23)
PS2_IMPORTS_END()

extern "C" {

int _module_start(int, char **) {
    if (_import_loadcore_register_library(&modload_exports) < 0) {
        return 1;                       // IRX-10a: not resident
    }
    // §2: the registration happens at module-init time and only in the
    // intermediate stage of an update reboot, which boot record key 4 names.
    const uint32_t *mode = _import_loadcore_boot_record(kKeyBootMode);
    if (mode != nullptr && (*mode & 0xFF) == kModeUpdateStage) {
        // BOOT-8f: pass 1, which is where the reference's MODLOAD registers it.
        (void)_import_loadcore_add_bootup_callback(mode2Bootup, 1, nullptr);
    }
    return 0;                           // resident
}

}  // extern "C"
