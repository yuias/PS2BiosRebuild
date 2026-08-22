// LOADCORE: the module registry, and the services modules call to join it.
//
// docs/spec/02-module-abi.md IRX-10: two entry points register an export
// table, and they are not interchangeable -- ordinal 6 compares tag and major
// version and accepts only a strictly greater minor, ordinal 10 pins a table
// at the head with no comparison at all.
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
        return -1;                     // not an export table
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

// IRX-4: the export table, kept mutable for the same reason as `sysmem`'s
// (IRX-4c).
struct ExportTable {
    uint32_t magic;
    uint32_t clients;
    uint16_t version;
    uint16_t flags;
    char tag[8];
    int (*entries[14])(void *);
};
static_assert(offsetof(ExportTable, entries) == 0x14);

// `used`: nothing in this translation unit takes its address -- the loader
// finds it by scanning for the magic word (IRX-4b), not through a reference
// -- so an optimiser that only sees the call graph would otherwise drop it.
[[gnu::used]] ExportTable loadcore_exports = {
    kExportMagic,
    0,
    0x0101,                             // version 1.01, BCD
    0,
    {'l', 'o', 'a', 'd', 'c', 'o', 'r', 'e'},   // IRX-4a: exactly eight, no terminator
    {
        reservedHook,                   // 0  reserved
        reservedHook,                   // 1  reserved
        unimplemented,                  // 2
        unimplemented,                  // 3
        unimplemented,                  // 4
        unimplemented,                  // 5
        registerVersioned,              // 6  register, versioned
        unimplemented,                  // 7
        unimplemented,                  // 8
        unimplemented,                  // 9
        registerPinned,                 // 10 register, pinned
        unimplemented,                  // 11
        unimplemented,                  // 12
        nullptr,                        // IRX-5b
    },
};

}  // namespace

// IRX-8: an import table is stubs, two instructions each, terminated by a
// zero word, and the loader rewrites only the first word of a bound one into
// a jump (IRX-9a). That exact encoding is one of the few things this project
// keeps as raw instruction words rather than C++ (docs/implementation.md
// "What is written in C++, and what cannot be"), so it stays a small
// top-level `asm` block instead of a struct literal. It lives in `.data`,
// like the assembly it replaces: IOP memory carries no execute permission to
// lose, and IRX-8 does not ask for any. `.set noreorder` keeps the assembler
// from doing anything clever with the branch delay slot -- the `addiu` must
// be the literal next word, ordinal and all.
asm(
    ".pushsection .data,\"aw\"\n"
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
    "addiu $v0, $zero, 4\n"             // ordinal 4: allocate
    ".globl _import_sysmem_release\n"
    "_import_sysmem_release:\n"
    "jr $ra\n"
    "addiu $v0, $zero, 5\n"             // ordinal 5: release
    ".word 0\n"                         // the terminator
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

// IRX-12: entry(argc, argv, 0, module_record), with the module's own $gp
// installed. Calling the bound import proves the binding worked -- the same
// use a reference module would make of its own imports from its entry.
int _module_start(int, char **) {
    const int address = _import_sysmem_allocate(0, 64, 0);
    *reinterpret_cast<volatile uint32_t *>(kRanMarker) =
        static_cast<uint32_t>(address);
    return 0;                          // resident
}

}  // extern "C"
