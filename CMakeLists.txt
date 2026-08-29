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

# A BIOS image is judged by what it does and how much room it takes, not by how
# it debugs, so the default is the size-minimising build rather than none at
# all. -O0 was never a decision; it was what an unset build type gives.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE MinSizeRel CACHE STRING "" FORCE)
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
# The program the boot ends by running is our OSDSYS unless a test program is
# named instead: an EE ELF built elsewhere (docs/project-state.md §6, M1) is
# stored in the OSDSYS slot so the boot places and enters it with no further
# machinery. The ELF is an input like the reference images and is not committed.
set(PS2_TEST_PROGRAM "" CACHE FILEPATH
    "EE ELF to store as OSDSYS in place of our own (empty: build/osdsys.elf)")
if(PS2_TEST_PROGRAM)
    get_filename_component(PS2_OSDSYS "${PS2_TEST_PROGRAM}" ABSOLUTE)
    message(STATUS "OSDSYS slot: ${PS2_OSDSYS}")
else()
    set(PS2_OSDSYS "${CMAKE_CURRENT_BINARY_DIR}/osdsys.elf")
endif()
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
              --only-section=.rodata --only-section=.data
              --only-section=.syscalls
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
  # docs/implementation.md for what compiled IOP code asks of `mkirx`.
  target_compile_options(${name}.elf PRIVATE
      "$<$<COMPILE_LANGUAGE:CXX>:-march=mips1;-mno-check-zero-division>")
  set(script "${CMAKE_CURRENT_SOURCE_DIR}/src/link/irx.ld")
  target_link_options(${name}.elf PRIVATE "-T${script}" --emit-relocs)
  # The container is made after the link; a change to the tool that makes it
  # has to make it again, which a relink is the one way to ask for.
  set_target_properties(${name}.elf PROPERTIES LINK_DEPENDS
      "${script};${CMAKE_CURRENT_SOURCE_DIR}/tools/mkirx.py")
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

# The boot block serves both processors, so its two compiled halves are not
# compiled alike: the R3000A has neither MIPS III nor the `teq` a division check
# emits, and only the IOP's half may be built for it.
set_source_files_properties(src/boot/reset_iop.cpp PROPERTIES
    COMPILE_OPTIONS "-march=mips1;-mno-check-zero-division")
ps2AddComponent(reset  SCRIPT reset.ld  SOURCES
    src/boot/reset.S
    src/boot/reset_ee.cpp
    src/boot/reset_iop.cpp)
ps2AddComponent(rdram  SCRIPT rdram.ld  SOURCES src/boot/rdram.cpp)
ps2AddModule(sysmem MODULE Memory_Manager VERSION 1.00
    SOURCES src/iop/sysmem.cpp)
ps2AddModule(loadcore MODULE Module_Loader VERSION 1.00
    SOURCES src/iop/loadcore.cpp)
ps2AddModule(excepman MODULE Exception_Manager VERSION 1.00
    SOURCES src/iop/excepman.cpp)
ps2AddModule(intrman MODULE Interrupt_Manager VERSION 1.00
    SOURCES src/iop/intrman.S src/iop/intrman.cpp)
ps2AddModule(timrman MODULE Timer_Manager VERSION 1.00
    SOURCES src/iop/timrman.cpp)
ps2AddModule(threadman MODULE Multi_Thread_Manager VERSION 1.00
    SOURCES src/iop/threadman.cpp)
ps2AddModule(sysclib MODULE System_C_lib VERSION 1.00
    SOURCES src/iop/sysclib.cpp)
ps2AddModule(dmacman MODULE DMA_Controller_Manager VERSION 1.00
    SOURCES src/iop/dmacman.cpp)
ps2AddModule(ioman MODULE IO_File_Manager VERSION 1.00
    SOURCES src/iop/ioman.cpp)
ps2AddModule(modload MODULE Module_File_Loader VERSION 1.00
    SOURCES src/iop/modload.cpp)
ps2AddModule(romdrv MODULE ROM_File_Driver VERSION 1.00
    SOURCES src/iop/romdrv.cpp)
ps2AddModule(stdio MODULE Standard_IO VERSION 1.00
    SOURCES src/iop/stdio.cpp)
ps2AddModule(loadfile MODULE Load_Module_By_EE VERSION 1.00
    SOURCES src/iop/loadfile.cpp)
ps2AddModule(cdvdman MODULE cdvd_driver VERSION 1.00
    SOURCES src/iop/cdvdman.cpp)
ps2AddModule(fileio MODULE FILEIO_service VERSION 1.00
    SOURCES src/iop/fileio.cpp)
ps2AddModule(cdvdfsv MODULE cdvd_EE_driver VERSION 1.00
    SOURCES src/iop/cdvdfsv.cpp)
ps2AddModule(reboot MODULE IOP_reboot VERSION 1.00
    SOURCES src/iop/reboot.cpp)
# Not on the boot list: the first module a program loads on request.
ps2AddModule(sio2man MODULE sio2man VERSION 1.00
    SOURCES src/iop/sio2man.cpp)
ps2AddModule(eesync MODULE EE_Synchroniser VERSION 1.00
    SOURCES src/iop/eesync.cpp)

# An EE program is stored as the ELF it is: the kernel's loader reads the
# program headers rather than being told a layout.
add_executable(osdsys.elf src/ee/osdsys.S src/ee/osdsys.cpp src/ee/sifclient.cpp)
# Without a small page size the linker aligns the segment's file offset to
# 64 KiB, which would store sixty-odd kilobytes of padding in the archive for
# a program of a few hundred bytes.
target_link_options(osdsys.elf PRIVATE
    "-T${CMAKE_CURRENT_SOURCE_DIR}/src/link/osdsys.ld"
    -z max-page-size=16)
set_target_properties(osdsys.elf PROPERTIES
    LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/link/osdsys.ld")

# EELOAD is raw code at the address the kernel stages it to (spec/04 EE-9a,
# docs/analysis/41), lifted out of its ELF like the other components.
ps2AddComponent(eeload SCRIPT eeload.ld SOURCES
    src/ee/eeload.S src/ee/eeload.cpp src/ee/sifclient.cpp)

ps2AddComponent(kernel SCRIPT kernel.ld SOURCES
    src/kernel/vectors.S
    src/kernel/entry.S
    src/kernel/entry.cpp
    src/kernel/syscall.S
    src/kernel/sif.cpp
    src/kernel/program.cpp
    src/kernel/cache.cpp
    src/kernel/gs.S
    src/kernel/gs.cpp
    src/kernel/osdconfig.cpp
    src/kernel/thread.cpp
    src/kernel/interrupt.cpp
    src/kernel/tlb.cpp
    src/kernel/tables.cpp)

# IOPBOOT is linked at the address its own archive offset gives it, so that
# address has to be known before it is linked -- but the offset depends only
# on what is stored before it (RESET, ROMDIR, EXTINFO, ROMVER, RDRAM, KERNEL,
# IOPBTCONF), never on IOPBOOT's own size, so a pre-pass over the manifest can
# compute it once those are built. `mkromdir.py --offset-of` is that pre-pass;
# it needs the *data* of the entries before IOPBOOT and the *extinfo* of every
# entry (EXTINFO concatenates all of them), which is why the built modules'
# `.extinfo` files are dependencies here despite the modules themselves coming
# after IOPBOOT in the archive.
#
# ROMVER and the pre-pass both live in their own directory (`cmake/iopboot-base`)
# rather than here: Ninja orders every plain custom command after every target
# defined in its *same* directory by default, which is fine everywhere else in
# this project but is exactly backwards for `iopboot.elf` below, which must
# link *after* the pre-pass runs -- and the pre-pass itself needs ROMVER's
# built size, so ROMVER's generation has to leave this directory with it, or
# the same ordering rule would put it after `iopboot.elf` too. A directory
# with no targets of its own never triggers that ordering, so `iopboot.elf`
# can depend on the pre-pass's output without Ninja seeing a cycle.
set(PS2_ROMVER "${CMAKE_CURRENT_BINARY_DIR}/romver.bin")
set(PS2_IOPBOOT_BASE 0xBFC00000)
set(PS2_IOPBOOT_BASE_RSP "${CMAKE_CURRENT_BINARY_DIR}/iopboot_base.rsp")
add_subdirectory(cmake/iopboot-base)

# The R3000A has neither MIPS III nor the `teq` a division check emits, the
# same reason `reset_iop.cpp` is pinned below.
set_source_files_properties(src/boot/iopboot.cpp PROPERTIES
    COMPILE_OPTIONS "-march=mips1;-mno-check-zero-division")
ps2AddComponent(iopboot SCRIPT iopboot.ld SOURCES src/boot/iopboot.cpp)
target_link_options(iopboot.elf PRIVATE "@${PS2_IOPBOOT_BASE_RSP}")
set_property(TARGET iopboot.elf APPEND PROPERTY
    LINK_DEPENDS "${PS2_IOPBOOT_BASE_RSP}")

set(PS2_IMAGE "${CMAKE_CURRENT_BINARY_DIR}/rom.bin")
add_custom_command(
    OUTPUT "${PS2_IMAGE}"
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/mkromdir.py"
            "${PS2_MANIFEST}" -o "${PS2_IMAGE}" --size ${PS2_IMAGE_SIZE}
            --check IOPBOOT ${PS2_IOPBOOT_BASE} "${PS2_IOPBOOT_BASE_RSP}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/mkromdir.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/rom/iopbtconf.txt"
            "${PS2_MANIFEST}" "${PS2_ROMVER}" "${PS2_OSDSYS}"
            "${PS2_IOPBOOT_BASE_RSP}"
            reset.elf rdram.elf iopboot.elf kernel.elf sysmem.elf
            loadcore.elf excepman.elf intrman.elf timrman.elf threadman.elf sysclib.elf
            dmacman.elf ioman.elf modload.elf romdrv.elf stdio.elf eesync.elf loadfile.elf
            cdvdman.elf fileio.elf cdvdfsv.elf reboot.elf sio2man.elf osdsys.elf eeload.elf
    COMMENT "Assembling the ROM archive")

add_custom_target(image ALL DEPENDS "${PS2_IMAGE}")

# The image is judged by the same simulators that judge the reference, at the
# depth this build has reached. `tools/imgcheck.py` says what that depth is.
add_custom_target(check
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/imgcheck.py" "${PS2_IMAGE}"
    # The R3000A's load delay slot (docs/analysis/31): IOP code written by
    # hand is the boot block's IOP half and the exception paths of EXCEPMAN
    # and INTRMAN, so those are where the check is needed -- one emulator
    # honours the slot and one does not, and the hazard only shows on the
    # first.
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/loaddelay.py"
            --objdump "${PS2_OBJDUMP}"
            "$<TARGET_FILE:reset.elf>" --between _iop_reset eeReset
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/loaddelay.py"
            --objdump "${PS2_OBJDUMP}"
            "$<TARGET_FILE:excepman.elf>" "$<TARGET_FILE:intrman.elf>"
    DEPENDS "${PS2_IMAGE}" reset.elf excepman.elf intrman.elf
    USES_TERMINAL
    COMMENT "Judging the image against docs/spec/")
