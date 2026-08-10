cmake_minimum_required(VERSION 3.28)

project(PS2BiosRebuild
        VERSION 0.1.0
        DESCRIPTION "Reimplementation of the PlayStation 2 BIOS"
        LANGUAGES ASM)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Generic")
  message(FATAL_ERROR
      "This project only builds for the PS2 target. Configure with:\n"
      "  cmake -B build -G Ninja "
      "-DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mipsel-ps2.cmake")
endif()

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
              "$<TARGET_FILE:${name}.elf>"
              "${CMAKE_CURRENT_BINARY_DIR}/${name}.bin"
      BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/${name}.bin"
      COMMENT "Extracting ${name}.bin")
endfunction()

ps2AddComponent(reset  SCRIPT reset.ld  SOURCES src/boot/reset.S)
ps2AddComponent(rdram  SCRIPT rdram.ld  SOURCES src/boot/rdram.S)
ps2AddComponent(kernel SCRIPT kernel.ld SOURCES
    src/kernel/vectors.S
    src/kernel/entry.S
    src/kernel/syscall.S
    src/kernel/tables.S)

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
            reset.elf rdram.elf kernel.elf
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
