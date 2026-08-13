cmake_minimum_required(VERSION 3.28)

project(PS2BiosRebuild
        VERSION 0.1.0
        DESCRIPTION "Reimplementation of the PlayStation 2 BIOS"
        LANGUAGES ASM CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Generic")
  message(FATAL_ERROR
      "This project only builds for the PS2 target. Configure with:\n"
      "  cmake -B build -G Ninja "
      "-DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mipsel-ps2.cmake")
endif()

# The language is available in full; the standard *library* is not, since there
# is no libc++ for mipsel-none-elf. `<stdint.h>` is the compiler's own
# freestanding header and is the one include the image uses.
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)

set(PS2_IMAGE_SIZE 0x400000)             # spec/01 ARC-1
# mkromdir.py resolves a manifest's paths from the manifest's own directory, so
# the configured copy carries absolute ones and can live in the build tree.
set(PS2_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(PS2_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}")
set(PS2_MANIFEST "${CMAKE_CURRENT_BINARY_DIR}/manifest.txt")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/rom/manifest.txt.in"
               "${PS2_MANIFEST}" @ONLY)

# ROMVER's fields are ours to choose and must not impersonate a retail ROM
# (spec/01 ARC-7a): version 01.00, region X, type P, for this project.
set(PS2_ROMVER_VERSION "01.00")
set(PS2_ROMVER_REGION "X")
set(PS2_ROMVER_TYPE "P")
set(PS2_ROMVER_DATE "2026-08-10")

# Each component is linked at the address it runs from and then lifted back out
# as raw bytes: the archive stores files, not ELFs.
function(ps2AddComponent name)
  cmake_parse_arguments(PARSE_ARGV 1 ARG "" "SCRIPT" "SOURCES")
  add_executable(${name}.elf ${ARG_SOURCES})
  set(script "${CMAKE_CURRENT_SOURCE_DIR}/src/link/${ARG_SCRIPT}")
  target_link_options(${name}.elf PRIVATE "-T${script}")
  set_target_properties(${name}.elf PROPERTIES LINK_DEPENDS "${script}")
  add_custom_command(TARGET ${name}.elf POST_BUILD
      COMMAND "${PS2_OBJCOPY}" -O binary
              --only-section=.vectors --only-section=.text
              --only-section=.rodata --only-section=.syscalls
              --only-section=.exctable --only-section=.inttable
              --only-section=.cop0reads
              "$<TARGET_FILE:${name}.elf>"
              "${CMAKE_CURRENT_BINARY_DIR}/${name}.bin"
      BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/${name}.bin"
      COMMENT "Extracting ${name}.bin")
endfunction()

# An IOP module goes through one more step than a raw component: it is linked
# at zero with its fixups kept, and `tools/mkirx.py` rewrites that into the
# container of spec/02 IRX-1, emitting the matching EXTINFO as it goes so the
# two carry the same name and version (IRX-2a).
function(ps2AddModule name)
  cmake_parse_arguments(PARSE_ARGV 1 ARG "" "MODULE;VERSION" "SOURCES")
  add_executable(${name}.elf ${ARG_SOURCES})
  # The toolchain's default is MIPS III, which the EE has and the IOP does not.
  # Hand-written assembly is the author's responsibility, but a compiler will
  # emit `daddu`/`sd` for a 64-bit type and `teq` for a division check, so IOP
  # compiled code is pinned to what an R3000A implements. See
  # docs/implementation.md for why there is no compiled IOP code yet.
  target_compile_options(${name}.elf PRIVATE
      "$<$<COMPILE_LANGUAGE:CXX>:-march=mips1;-mno-check-zero-division>")
  set(script "${CMAKE_CURRENT_SOURCE_DIR}/src/link/irx.ld")
  target_link_options(${name}.elf PRIVATE "-T${script}" --emit-relocs)
  set_target_properties(${name}.elf PROPERTIES LINK_DEPENDS "${script}")
  add_custom_command(TARGET ${name}.elf POST_BUILD
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/tools/mkirx.py"
              "$<TARGET_FILE:${name}.elf>"
              -o "${CMAKE_CURRENT_BINARY_DIR}/${name}.irx"
              --name "${ARG_MODULE}" --version "${ARG_VERSION}"
              --date "${PS2_ROMVER_DATE}"
              --extinfo "${CMAKE_CURRENT_BINARY_DIR}/${name}.extinfo"
      BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/${name}.irx"
                 "${CMAKE_CURRENT_BINARY_DIR}/${name}.extinfo"
      COMMENT "Building the ${ARG_MODULE} module")
endfunction()

ps2AddComponent(reset  SCRIPT reset.ld  SOURCES src/boot/reset.S)
ps2AddComponent(rdram  SCRIPT rdram.ld  SOURCES src/boot/rdram.S)
ps2AddComponent(iopboot SCRIPT iopboot.ld SOURCES src/boot/iopboot.S)
ps2AddModule(sysmem MODULE Memory_Manager VERSION 1.00
    SOURCES src/iop/sysmem.S)
ps2AddModule(loadcore MODULE Module_Loader VERSION 1.00
    SOURCES src/iop/loadcore.S)
ps2AddModule(eesync MODULE EE_Synchroniser VERSION 1.00
    SOURCES src/iop/eesync.S)

# An EE program is stored as the ELF it is: the kernel's loader reads the
# program headers rather than being told a layout.
add_executable(osdsys.elf src/ee/osdsys.S src/ee/osdsys.cpp)
# Without a small page size the linker aligns the segment's file offset to
# 64 KiB, which would store sixty-odd kilobytes of padding in the archive for
# a program of a few hundred bytes.
target_link_options(osdsys.elf PRIVATE
    "-T${CMAKE_CURRENT_SOURCE_DIR}/src/link/osdsys.ld"
    -z max-page-size=16)
set_target_properties(osdsys.elf PROPERTIES
    LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/link/osdsys.ld")

ps2AddComponent(kernel SCRIPT kernel.ld SOURCES
    src/kernel/vectors.S
    src/kernel/entry.S
    src/kernel/entry.cpp
    src/kernel/syscall.S
    src/kernel/sif.cpp
    src/kernel/program.S
    src/kernel/program.cpp
    src/kernel/cache.cpp
    src/kernel/tables.cpp)

set(PS2_ROMVER "${CMAKE_CURRENT_BINARY_DIR}/romver.bin")
add_custom_command(
    OUTPUT "${PS2_ROMVER}"
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/mkromver.py"
            --version "${PS2_ROMVER_VERSION}"
            --region "${PS2_ROMVER_REGION}"
            --type "${PS2_ROMVER_TYPE}"
            --date "${PS2_ROMVER_DATE}"
            -o "${PS2_ROMVER}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/mkromver.py"
    COMMENT "Generating ROMVER")

set(PS2_IMAGE "${CMAKE_CURRENT_BINARY_DIR}/rom.bin")
add_custom_command(
    OUTPUT "${PS2_IMAGE}"
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/mkromdir.py"
            "${PS2_MANIFEST}" -o "${PS2_IMAGE}" --size ${PS2_IMAGE_SIZE}
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/mkromdir.py"
            "${PS2_MANIFEST}" "${PS2_ROMVER}"
            reset.elf rdram.elf iopboot.elf kernel.elf sysmem.elf
            loadcore.elf eesync.elf osdsys.elf
    COMMENT "Assembling the ROM archive")

add_custom_target(image ALL DEPENDS "${PS2_IMAGE}")

# The image is judged by the same simulators that judge the reference, at the
# depth this build has reached. `tools/imgcheck.py` says what that depth is.
add_custom_target(check
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/imgcheck.py" "${PS2_IMAGE}"
    DEPENDS "${PS2_IMAGE}"
    USES_TERMINAL
    COMMENT "Judging the image against docs/spec/")
