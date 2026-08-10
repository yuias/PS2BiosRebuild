# CMake toolchain file for the PlayStation 2's two processors.
#
# The image is freestanding: no host libc, no startup files, no loader.
# CMAKE_SYSTEM_NAME=Generic tells CMake to skip all host-platform assumptions.
#
# The PS2 has two CPUs and this file serves both. The EE is an R5900 (MIPS III
# plus its own multimedia set) and the IOP an R3000A (MIPS I), so the assembler
# is set to accept MIPS III -- the wider of the two. That decides what the
# *assembler* takes, not who runs it: code on the IOP path must still be
# written in instructions an R3000A has, and the shared reset dispatch of
# docs/spec/03-boot-chain.md BOOT-1 in instructions both do.

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR mipsel)

# Debian and Ubuntu ship versioned binaries (clang-22, ld.lld-22); the official
# Windows and macOS installers do not. Probe rather than assume, so the same
# toolchain file works on all of them. Override -DPS2_LLVM_SUFFIX=... to pin a
# particular version when several are installed.
if(NOT DEFINED PS2_LLVM_SUFFIX)
  find_program(PS2_PROBE_VERSIONED_CLANG clang-22)
  if(PS2_PROBE_VERSIONED_CLANG)
    set(PS2_LLVM_SUFFIX "-22")
  else()
    set(PS2_LLVM_SUFFIX "")
  endif()
endif()
set(PS2_LLVM_SUFFIX "${PS2_LLVM_SUFFIX}"
    CACHE STRING "Version suffix of the LLVM tool binaries")

# Resolve to absolute paths. On Windows the build tool does not necessarily
# inherit the PATH that configured the project.
function(ps2FindTool out_var name)
  find_program(${out_var} NAMES "${name}${PS2_LLVM_SUFFIX}" "${name}" REQUIRED)
endfunction()

ps2FindTool(PS2_CC clang)
ps2FindTool(PS2_AR llvm-ar)
ps2FindTool(PS2_RANLIB llvm-ranlib)
ps2FindTool(PS2_LD ld.lld)
ps2FindTool(PS2_OBJCOPY llvm-objcopy)
ps2FindTool(PS2_OBJDUMP llvm-objdump)

set(CMAKE_C_COMPILER "${PS2_CC}")
set(CMAKE_ASM_COMPILER "${PS2_CC}")
set(CMAKE_AR "${PS2_AR}")
set(CMAKE_RANLIB "${PS2_RANLIB}")

set(CMAKE_C_COMPILER_TARGET mipsel-none-elf)
set(CMAKE_ASM_COMPILER_TARGET mipsel-none-elf)

# CMake's default compiler check links an executable, which needs a link script
# we only supply for real targets. A static library is enough to prove the
# assembler works.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(PS2_ARCH_FLAGS
    -march=mips3
    -mno-abicalls
    -fno-pic
    -G0
    -msoft-float
    -fomit-frame-pointer
    -Wno-unused-command-line-argument)

set(PS2_FREESTANDING_FLAGS
    -ffreestanding
    -fno-builtin
    -fno-stack-protector
    -fno-unwind-tables
    -fno-asynchronous-unwind-tables)

string(JOIN " " PS2_COMMON_FLAGS ${PS2_ARCH_FLAGS} ${PS2_FREESTANDING_FLAGS})

set(CMAKE_C_FLAGS_INIT "${PS2_COMMON_FLAGS}")
set(CMAKE_ASM_FLAGS_INIT "${PS2_COMMON_FLAGS}")

# Link with ld.lld directly rather than through the clang driver. For
# mipsel-none-elf the driver delegates to the host gcc, which reaches for the
# host GNU ld and fails on MIPS objects; -fuse-ld=lld does not help, because
# the delegation happens first.
set(CMAKE_LINKER "${PS2_LD}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "--build-id=none")

set(PS2_LINK_RULE
    "<CMAKE_LINKER> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")
set(CMAKE_C_LINK_EXECUTABLE "${PS2_LINK_RULE}")
set(CMAKE_ASM_LINK_EXECUTABLE "${PS2_LINK_RULE}")

# Only look inside the project; the host sysroot is irrelevant to a bare-metal
# MIPS image.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
