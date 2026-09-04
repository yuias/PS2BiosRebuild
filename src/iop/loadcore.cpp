// LOADCORE: the boot list's loader, the module registry, and the services
// modules call to join it.
//
// docs/spec/02-module-abi.md IRX-10: two entry points register an export
// table, and they are not interchangeable -- ordinal 6 compares tag and major
// version and accepts only a strictly greater minor, ordinal 10 pins a table
// at the head with no comparison at all.
//
// docs/spec/03-boot-chain.md BOOT-8c: `IOPBOOT` places `SYSMEM` and this
// module and then calls this module's entry with the eight-word block at
// `0x20000`; the rest of the boot list is loaded from here. The reason the
// division falls there is that a loader which allocates needs a memory
// manager and a registry to already exist, and nothing else does.
//
// The registry itself is a single word of RAM, and `src/boot/iopboot.cpp`'s
// loader already keeps it: the first module loaded still has to be
// registered, so the head cannot live inside this module's own data. Boot-time
// registration is actually done by that loader, scanning each module's
// segment for the export magic directly, rather than by a module calling
// ordinal 6 from its own entry (docs/implementation.md "The loader registers
// a module's exports; the module does not"); these two ordinals exist so a
// module that registers itself the reference's way -- by calling them -- gets
// the same behaviour over the same registry.
//
// This module also imports `sysmem` 4, which is the first cross-module call in
// the image and therefore the first test of the binding of IRX-9.

#include "loader.hpp"

#include <stddef.h>
#include <stdint.h>

namespace {

constexpr uint32_t kExportMagic = 0x41C00000;

// The registry head, at the address `src/boot/iopboot.cpp` uses. It is an
// absolute constant rather than something this module owns because the two
// are separate images and only the address can be shared.
constexpr uintptr_t kRegistryHead = 0x001F8010;

// Scaffolding, until there is more to see. It sits with the boot's own words
// above the modules: anywhere in the load region would be zeroed by the bss
// of whichever module lands on top of it.
constexpr uintptr_t kRanMarker = 0x001F8020;

[[nodiscard]] volatile uint32_t &registryHead() {
    return *reinterpret_cast<volatile uint32_t *>(kRegistryHead);
}

// IRX-4's shared 20-byte header. `link` is the magic in the stored file and
// becomes the registry link once the table is registered (IRX-4c).
struct LibraryTable {
    uint32_t link;
    uint32_t clients;
    uint16_t version;
    uint16_t flags;
    char tag[8];
};
static_assert(sizeof(LibraryTable) == 0x14);

// IRX-7: slots 0 and 1 are reserved hooks that nothing imports, kept occupied
// rather than removed (IRX-6a), so they need somewhere to point.
[[nodiscard]] int reservedHook(void *) {
    return 0;
}

[[nodiscard]] int unimplemented(void *) {
    return -1;
}

// The shared tail of both registration ordinals (IRX-4c): link the candidate
// in at the registry head.
[[nodiscard]] int registerAccept(LibraryTable *table) {
    const uint32_t head = registryHead();
    table->link = head;
    registryHead() = reinterpret_cast<uint32_t>(table);
    return 0;
}

// Ordinal 6: versioned registration (IRX-10a). A library's identity is its
// tag and major version; the minor is a generation counter and only a
// strictly greater one is allowed to take over.
[[nodiscard]] int registerVersioned(void *table_ptr) {
    auto *table = reinterpret_cast<LibraryTable *>(table_ptr);
    if (table->link != kExportMagic) {
        // Already in: the loader registers a table when it places the module
        // (IOP-5b), so a module registering itself from its entry -- the
        // reference's way -- finds its own table there and is told yes
        // (docs/implementation.md). Anything else is not an export table.
        for (auto *walk = reinterpret_cast<LibraryTable *>(registryHead());
             walk != nullptr;
             walk = reinterpret_cast<LibraryTable *>(walk->link)) {
            if (walk == table) {
                return 0;
            }
        }
        return -1;
    }
    const auto *candidate_tag = reinterpret_cast<const uint32_t *>(table->tag);
    const auto major = static_cast<uint8_t>(table->version >> 8);
    const auto minor = static_cast<uint8_t>(table->version);

    for (auto *walk = reinterpret_cast<LibraryTable *>(registryHead());
         walk != nullptr;
         walk = reinterpret_cast<LibraryTable *>(walk->link)) {
        const auto *walk_tag = reinterpret_cast<const uint32_t *>(walk->tag);
        if (walk_tag[0] != candidate_tag[0] || walk_tag[1] != candidate_tag[1]) {
            continue;                   // a different library entirely
        }
        if (static_cast<uint8_t>(walk->version >> 8) != major) {
            continue;                   // same tag, different major: different library
        }
        // Same tag and major: the same library. Strictly greater supersedes;
        // equal or lower is refused.
        return static_cast<uint8_t>(walk->version) < minor
                   ? registerAccept(table)
                   : -1;
    }
    return registerAccept(table);       // nothing registered under this tag yet
}

// Ordinal 10: pinned registration (IRX-10b). No comparison, and flags bit 0
// is set at run time -- the stored table always has flags 0.
[[nodiscard]] int registerPinned(void *table_ptr) {
    auto *table = reinterpret_cast<LibraryTable *>(table_ptr);
    if (table->link != kExportMagic) {
        return -1;
    }
    table->flags |= 1;
    return registerAccept(table);
}

// --- spec/06 IOP-5a/5b: the loader, for modules loaded on request ---------
//
// The same `src/iop/loader.hpp` the boot block uses, behind the ordinals
// MODLOAD calls in sequence: probe, load, link, flush, register. The module
// id counts on from the boot list, whose modules the boot block loaded
// without records: the first module loaded on request is numbered after the
// last one the list named, which is where the reference's count lands too
// (docs/analysis/34 §6: SIO2MAN is 25 after a 24-entry list).

constexpr uintptr_t kBootList = 0x001F8100;
constexpr uintptr_t kBlCount = 0x000;
constexpr uintptr_t kBlNext = 0x00C;
constexpr uintptr_t kBlLoaded = 0x400;
constexpr uintptr_t kBootListCount = kBootList + kBlCount;

[[nodiscard]] volatile uint32_t &bootListWord(uintptr_t offset) {
    return *reinterpret_cast<volatile uint32_t *>(kBootList + offset);
}

// BOOT-8c: the block the entry is handed, read once into memory of our own.
// **This copy is the entry's first action and must stay so**: on this image
// `0x20000` is `SYSMEM`'s first heap byte, so the first allocation anyone
// makes -- including this module's own, below -- lands on the block.
constexpr uint32_t kBootInfoWords = 8;
constexpr uint32_t kBiRamMiB = 0;
constexpr uint32_t kBiMode = 1;
constexpr uint32_t kBiCommandLine = 2;
constexpr uint32_t kBiSysmemBase = 3;
constexpr uint32_t kBiListCount = 6;
constexpr uint32_t kBiList = 7;

uint32_t boot_info[kBootInfoWords];

// BOOT-8: the boot records, and the two addresses that point at them. A
// record is a header word -- a 16-bit value, the key, the count of extra
// words -- and the list ends at a zero header.
constexpr uintptr_t kBootRecordPointer = 0x3F0;
constexpr uintptr_t kBootRecordPointerAlso = 0x3F4;
constexpr uint32_t kBootRecordWords = 16;
constexpr uint32_t kKeyBootMode = 4;           // BOOT-8b
constexpr uint32_t kKeyCommandLine = 5;

uint32_t boot_records[kBootRecordWords];
uint32_t boot_records_used;

// BOOT-8b: the command line travels as a boot record holding a **pointer to
// a copy of the string** -- it is not tokenised here. The reference copies it
// onto its own stack; ours needs it to outlive this module's entry, because
// `MODLOAD`'s callback is what reads and splits it, so it goes in a static
// buffer instead.
constexpr uint32_t kCommandLineMax = 256;
char command_line[kCommandLineMax];

[[nodiscard]] uint32_t recordHeader(uint32_t value, uint32_t key, uint32_t extra) {
    return (value & 0xFFFF) | (key << 16) | (extra << 24);
}

// The copy the record points at. Bounded rather than trusting the string:
// what is at the other end of the block's pointer came from the EE.
void copyCommandLine(const char *from) {
    uint32_t n = 0;
    while (n + 1 < kCommandLineMax && from[n] != '\0') {
        command_line[n] = from[n];
        n++;
    }
    command_line[n] = '\0';
}

// BOOT-8b: the mode `IOPBOOT` was entered with becomes key 4, which is what
// a banner module reads to tell a cold boot from the stages of a reboot, and
// the command line becomes key 5 -- one extra word, a pointer to the copy.
void buildBootRecords() {
    boot_records_used = 0;
    boot_records[boot_records_used++] =
        recordHeader(boot_info[kBiMode], kKeyBootMode, 0);
    if (boot_info[kBiCommandLine] != 0) {
        copyCommandLine(reinterpret_cast<const char *>(boot_info[kBiCommandLine]));
        boot_records[boot_records_used++] = recordHeader(0, kKeyCommandLine, 1);
        boot_records[boot_records_used++] = reinterpret_cast<uint32_t>(command_line);
    }
    boot_records[boot_records_used] = 0;       // BOOT-8a: a zero header ends it
    const auto table = reinterpret_cast<uint32_t>(boot_records);
    *reinterpret_cast<volatile uint32_t *>(kBootRecordPointer) = table;
    *reinterpret_cast<volatile uint32_t *>(kBootRecordPointerAlso) = table;
}

// Ordinal 12, `QueryBootMode(key)`: the *address* of the record with that
// key, or 0 (BOOT-8a). A caller that wants the 16-bit value reads it from
// what comes back.
[[nodiscard]] int queryBootMode(uint32_t key) {
    const auto *record = *reinterpret_cast<uint32_t *const volatile *>(
        kBootRecordPointer);
    if (record == nullptr) {
        return 0;
    }
    for (; *record != 0; record += ((*record >> 24) & 0xFF) + 1) {
        if (((*record >> 16) & 0xFF) == key) {
            return static_cast<int>(reinterpret_cast<uintptr_t>(record));
        }
    }
    return 0;
}

// Ordinal 20: a module's entry asks to be called back once the boot list has
// run out. `MODLOAD` registers one when key 4 says this is the second stage
// of an update reboot, and that callback is what loads `UDNL`
// (docs/analysis/45 §2).
//
// **The registrant's `$gp` is captured here**, because a cross-module call
// arrives through a tail `j` and never touches `$28`: what is in it at this
// point is still the caller's, put there when the loader entered its module.
// The end-of-list pass installs it again for the call, since by then the
// register belongs to whoever ran last.
constexpr uint32_t kBootupCallbacks = 8;
constexpr uint32_t kBootupPasses = 4;

// BOOT-8f: the pass a callback runs in is carried in the low two bits of the
// stored function pointer, which are free because a function is word-aligned.
// That is why the registration takes a priority and the pass loop reads none:
// the two are the same field.
struct BootupCallback {
    uint32_t function;                 // the entry point, with the pass in bits 0-1
    uint32_t gp;
    void *argument;
};

BootupCallback bootup_callbacks[kBootupCallbacks];
uint32_t bootup_callback_count;

[[nodiscard]] uint32_t currentGp() {
    uint32_t gp;
    asm volatile("move %0, $gp" : "=r"(gp));
    return gp;
}

[[nodiscard]] int addBootupCallback(void (*function)(), int priority,
                                    void *argument) {
    if (function == nullptr || bootup_callback_count == kBootupCallbacks) {
        return -1;
    }
    const auto packed = (reinterpret_cast<uint32_t>(function) & ~uint32_t{3})
                        | (static_cast<uint32_t>(priority) & 3);
    bootup_callbacks[bootup_callback_count].function = packed;
    bootup_callbacks[bootup_callback_count].gp = currentGp();
    bootup_callbacks[bootup_callback_count].argument = argument;
    bootup_callback_count++;
    return 0;
}

ps2::loader::ModuleRecord *module_list;
uint16_t next_module_id;

// Ordinal 22: probe(image, info) -> 0 | -1.
[[nodiscard]] int probeExecutable(const uint8_t *image, ps2::loader::ExecutableInfo *info) {
    ps2::loader::Segments segments;
    if (!ps2::loader::readHeaders(image, segments)) {
        return -1;
    }
    info->type = 4;
    info->entry = 0;
    info->gp = 0;
    info->memory_size = segments.load_memsz;
    info->text_size = segments.load_filesz;
    info->data_size = 0;
    info->bss_size = segments.load_memsz - segments.load_filesz;
    info->base = 0;
    return 0;
}

// Ordinal 23: load(image, info) -> 0 | -1, placing at `info->base`. It does
// not register the module's exports: IRX-10a puts that in the module's own
// entry, through ordinal 6, and a loader that did it first would leave the
// table's magic word overwritten for that call to fail on.
[[nodiscard]] int loadExecutable(const uint8_t *image, ps2::loader::ExecutableInfo *info) {
    ps2::loader::Segments segments;
    if (!ps2::loader::readHeaders(image, segments) || info->base == 0) {
        return -1;
    }
    const ps2::loader::Placed placed = ps2::loader::place(image, segments, info->base);
    info->entry = placed.entry;
    info->gp = placed.gp;
    return 0;
}

// Ordinal 8: link(base, info) -> 0 | -1 when an import found no exporter.
[[nodiscard]] int linkLibraryEntries(uint8_t *base, const ps2::loader::ExecutableInfo *info) {
    return ps2::loader::bind(base, base + info->memory_size) == 0 ? 0 : -1;
}

// Ordinals 4 and 5: the two caches. A plain store is seen by the next fetch
// and the next load on the targets this image runs on
// (docs/implementation.md). Ordinal 5 is a void in the reference, so the
// value here is never read.
int flushIcache() {
    return 0;
}

int flushDcache() {
    return 0;
}

// Ordinal 27 (IRX-10c): store two flag bits on an export table. The bits are
// read by a teardown pass a reboot runs, which this project does not have --
// but a module's entry can return non-resident when this call fails, so the
// answer matters even where the bits do not.
constexpr int kNotRegistered = -213;
constexpr int kNoTable = -214;

[[nodiscard]] int setLibraryFlags(void *table_ptr, uint32_t flags) {
    auto *table = reinterpret_cast<LibraryTable *>(table_ptr);
    if (table == nullptr) {
        return kNoTable;
    }
    bool known = table->link == kExportMagic;    // registration has not eaten it
    for (auto *walk = reinterpret_cast<LibraryTable *>(registryHead());
         !known && walk != nullptr;
         walk = reinterpret_cast<LibraryTable *>(walk->link)) {
        known = walk == table;
    }
    if (!known) {
        return kNotRegistered;
    }
    table->flags = static_cast<uint16_t>((table->flags & ~6u) | (flags & 6u));
    return 0;
}

// Ordinal 16: register(record) -> 0. The id is assigned here (IOP-5c).
int registerModule(ps2::loader::ModuleRecord *record) {
    if (next_module_id == 0) {
        next_module_id = static_cast<uint16_t>(
            *reinterpret_cast<volatile uint32_t *>(kBootListCount) + 1);
    }
    record->id = next_module_id++;
    record->next = module_list;
    module_list = record;
    return 0;
}

int unimplementedCall() {
    return -1;
}

// IRX-4: the export table, kept mutable for the same reason as `sysmem`'s
// (IRX-4c).
struct ExportTable {
    uint32_t magic;
    uint32_t clients;
    uint16_t version;
    uint16_t flags;
    char tag[8];
    int (*entries[29])(void *);
};
static_assert(offsetof(ExportTable, entries) == 0x14);

template <typename F>
[[nodiscard]] constexpr int (*asSlot(F *function))(void *) {
    return reinterpret_cast<int (*)(void *)>(function);
}

// `used`: nothing in this translation unit takes its address -- the loader
// finds it by scanning for the magic word (IRX-4b), not through a reference
// -- so an optimiser that only sees the call graph would otherwise drop it.
[[gnu::used, gnu::section(".iopexport")]] ExportTable loadcore_exports = {
    kExportMagic,
    0,
    0x0101,                             // version 1.01, BCD
    0,
    {'l', 'o', 'a', 'd', 'c', 'o', 'r', 'e'},   // IRX-4a: exactly eight, no terminator
    {
        reservedHook,                   // 0  reserved
        reservedHook,                   // 1  reserved
        unimplemented,                  // 2
        unimplemented,                  // 3  GetLibraryEntryTable
        asSlot(flushIcache),            // 4  FlushIcache
        asSlot(flushDcache),            // 5  FlushDcache
        registerVersioned,              // 6  register, versioned
        unimplemented,                  // 7
        asSlot(linkLibraryEntries),     // 8  LinkLibraryEntries
        unimplemented,                  // 9  UnLinkLibraryEntries
        registerPinned,                 // 10 register, pinned
        unimplemented,                  // 11
        asSlot(queryBootMode),          // 12 QueryBootMode
        unimplemented,                  // 13
        unimplemented,                  // 14
        unimplemented,                  // 15
        asSlot(registerModule),         // 16 RegisterModule
        unimplemented,                  // 17 ReleaseModule
        unimplemented,                  // 18
        unimplemented,                  // 19
        asSlot(addBootupCallback),      // 20 bootup callbacks
        unimplemented,                  // 21 SetCacheCtrl
        asSlot(probeExecutable),        // 22 ProbeExecutableObject
        asSlot(loadExecutable),         // 23 LoadExecutableObject
        unimplemented,                  // 24
        unimplemented,                  // 25
        unimplemented,                  // 26
        asSlot(setLibraryFlags),        // 27 (IRX-10c)
        nullptr,                        // IRX-5b
    },
};

}  // namespace

// IRX-8: an import table is stubs, two instructions each, terminated by a
// zero word, and the loader rewrites only the first word of a bound one into
// a jump (IRX-9a). That exact encoding is one of the few things this project
// keeps as raw instruction words rather than C++ (docs/implementation.md
// "What is written in C++, and what cannot be"), so it stays a small
// top-level `asm` block instead of a struct literal. It lives in the text
// segment, where IRX-8's scan is the only thing that will ever find it.
// `.set noreorder` keeps the assembler
// from doing anything clever with the branch delay slot -- the `addiu` must
// be the literal next word, ordinal and all.
asm(
    ".pushsection .iopimport,\"ax\"\n"
    ".set noreorder\n"
    ".align 2\n"
    ".globl _sysmem_imports\n"
    "_sysmem_imports:\n"
    ".word 0x41E00000\n"                // magic: import
    ".word 0\n"                         // link, unused until bound
    ".short 0x0101\n"                   // version 1.01, BCD
    ".short 0\n"                        // IRX-9b: low bits clear, so it binds
    ".byte 's','y','s','m','e','m',0,0\n"   // IRX-4a: eight bytes
    ".globl _import_sysmem_allocate\n"
    "_import_sysmem_allocate:\n"
    "jr $ra\n"
    "addiu $zero, $zero, 4\n"           // ordinal 4: allocate
    ".globl _import_sysmem_release\n"
    "_import_sysmem_release:\n"
    "jr $ra\n"
    "addiu $zero, $zero, 5\n"           // ordinal 5: release
    ".word 0\n"                         // IRX-8: a whole zero stub,
    ".word 0\n"                         // not one zero word
    ".set reorder\n"
    ".popsection\n"
);

extern "C" {

// The stubs the block above defines. Bound, a call tail-jumps into the export
// it names and returns straight to the original caller (the target's own
// `jr $ra` sees the return address our `jal` set); unbound, it returns the
// ordinal harmlessly (IRX-8a).
int _import_sysmem_allocate(int mode, int size, int address);
int _import_sysmem_release(int address);

// BOOT-8c: this entry is not the ordinary IRX one. `IOPBOOT` calls it with
// the boot-info block's address where `argc` would be, and its own record
// where IRX-12 puts it -- which is all this module needs to know where the
// next module goes, the record's `+0x20` being the base past it.
int _module_start(uint32_t boot_info_address, char **, int, uint32_t record) {
    // **First, before anything allocates.** `0x20000` is `SYSMEM`'s first
    // heap byte on this image (it is not on the reference, whose heap starts
    // above its own placed image), so the block survives exactly as long as
    // it takes to read it -- the allocation five lines down is enough to
    // take it.
    const auto *block = reinterpret_cast<const volatile uint32_t *>(boot_info_address);
    for (uint32_t k = 0; k < kBootInfoWords; k++) {
        boot_info[k] = block[k];
    }

    // BOOT-8c: the registry's first entry is `SYSMEM`'s export table, which
    // is its load base, because a module's table is at its own offset zero.
    // The boot block registers nothing -- registering would overwrite the
    // magic word this module's own ordinal-6 call two lines below needs to
    // see -- so this is where the registry starts.
    registryHead() = boot_info[kBiSysmemBase];
    *reinterpret_cast<volatile uint32_t *>(boot_info[kBiSysmemBase]) = 0;

    // IRX-9, then IRX-10a: with `SYSMEM` in the registry this module can bind
    // its own imports -- which the boot block could not do for it, since the
    // registry did not exist when it was placed -- and then register its own
    // table. Its extent comes from the record the boot block filled in
    // (IRX-12c): the load segment's file bytes at `+0x1c`, its bss at `+0x24`.
    const auto *own_record = reinterpret_cast<const uint32_t *>(record);
    auto *own_base = reinterpret_cast<uint8_t *>(record + ps2::loader::kRecordSize);
    (void)ps2::loader::bind(own_base, own_base + own_record[7] + own_record[9]);
    if (registerVersioned(&loadcore_exports) < 0) {
        for (;;) {                     // nothing after this could bind
        }
    }

    buildBootRecords();

    // IRX-9: calling the bound import proves the binding worked -- the same
    // use a reference module would make of its own imports from its entry --
    // and it is what `tools/imgcheck.py` reads back.
    const int address = _import_sysmem_allocate(0, 64, 0);
    *reinterpret_cast<volatile uint32_t *>(kRanMarker) =
        static_cast<uint32_t>(address);

    // BOOT-8d: the list is single words ending at a zero one, and the walk
    // starts at index 2 -- entries 0 and 1 are `SYSMEM` and this module,
    // which the boot block has already placed.
    const auto *entries = reinterpret_cast<const uint32_t *>(boot_info[kBiList]);
    // The first module of the list goes past this one's own image, whose
    // extent is the record's `+0x1c` and `+0x24` again. With the record below
    // the base (BOOT-8c) the record address is where this module *starts*,
    // not where it ends, so it is not the answer.
    uint32_t running_record =
        (reinterpret_cast<uint32_t>(own_base) + own_record[7] + own_record[9]
         + 15) & ~uint32_t{15};
    uint32_t index = 2;
    for (uint32_t k = 2; entries[k] != 0; k++, index++) {
        const uint32_t word = entries[k];
        if ((word & 0xF) == 1) {
            // BOOT-8d's tagged entry: the module after it goes here.
            running_record = word >> 2;
            index--;
            continue;
        }
        if ((word & 1) != 0) {
            index--;
            continue;                  // any other odd word is skipped
        }
        const uint32_t module_base = running_record + ps2::loader::kRecordSize;
        bootListWord(kBlLoaded + index * 4) = module_base;
        bootListWord(kBlNext) = module_base;
        const ps2::loader::Loaded loaded =
            ps2::loader::placeModule(word, running_record);
        if (loaded.next_base == 0) {
            for (;;) {                 // BOOT-9a: a module that will not load
            }
        }
        // BOOT-8e: the answer is read twice. Its low two bits decide
        // residency -- 1 asks to be unloaded, which a bump-placing loader
        // cannot honour and records rather than acts on -- and everything
        // above them is a function pointer to run once the list is done.
        const uint32_t answer = ps2::loader::callEntry(
            loaded.entry, loaded.gp, 0, nullptr,
            reinterpret_cast<uint32_t>(&entries[k]));
        if ((answer & ~uint32_t{3}) != 0) {
            (void)addBootupCallback(
                reinterpret_cast<void (*)()>(answer & ~uint32_t{3}), 2,
                nullptr);
        }
        running_record = loaded.next_base;
    }
    bootListWord(kBlNext) = running_record;

    // BOOT-8f: four passes over the callbacks, each in registration order and
    // each calling only the ones registered for that pass. `callEntry` is the
    // invoke that installs the registrant's `$gp` and puts this module's back.
    for (uint32_t pass = 0; pass < kBootupPasses; pass++) {
        for (uint32_t k = 0; k < bootup_callback_count; k++) {
            const uint32_t packed = bootup_callbacks[k].function;
            if ((packed & 3) != pass) {
                continue;
            }
            // The reference passes its own callback cursor in `$a0` and 1 in
            // `$a1`; ours passes the argument the registration was given,
            // which is what a callback here could actually use. Neither of
            // ours reads either.
            (void)ps2::loader::callEntry(
                packed & ~uint32_t{3}, bootup_callbacks[k].gp,
                reinterpret_cast<uint32_t>(bootup_callbacks[k].argument),
                reinterpret_cast<char **>(1), 0);
        }
    }

    return 0;                          // resident; `IOPBOOT` sleeps its thread
}

}  // extern "C"
