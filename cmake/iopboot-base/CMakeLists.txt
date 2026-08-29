# Split out from the top-level CMakeLists.txt for one reason only: Ninja
# orders every plain (non-TARGET) custom command in a directory after every
# target defined in that *same* directory, as a conservative default. That is
# harmless everywhere else in this project, but `iopboot.elf` needs exactly
# the opposite of it -- it must link *after* the pre-pass below runs, since
# that is what computes the address it links at. Putting the pre-pass in its
# own directory scope, with no targets of its own, keeps the ordering rule
# from ever applying to it. ROMVER's own generation moved in here with it,
# since the pre-pass needs ROMVER's built size and the same ordering rule
# would otherwise put ROMVER's command after `iopboot.elf` too, right back
# where this started.
set(PS2_ROMVER_PATH "${PS2_ROMVER}")
add_custom_command(
    OUTPUT "${PS2_ROMVER_PATH}"
    COMMAND "${Python3_EXECUTABLE}"
            "${PS2_SOURCE_DIR}/tools/mkromver.py"
            --version "${PS2_ROMVER_VERSION}"
            --region "${PS2_ROMVER_REGION}"
            --type "${PS2_ROMVER_TYPE}"
            --date "${PS2_ROMVER_DATE}"
            -o "${PS2_ROMVER_PATH}"
    DEPENDS "${PS2_SOURCE_DIR}/tools/mkromver.py"
    COMMENT "Generating ROMVER")
add_custom_target(romver ALL DEPENDS "${PS2_ROMVER_PATH}")

add_custom_command(
    OUTPUT "${PS2_IOPBOOT_BASE_RSP}"
    COMMAND "${Python3_EXECUTABLE}"
            "${PS2_SOURCE_DIR}/tools/mkromdir.py"
            "${PS2_MANIFEST}" --offset-of IOPBOOT --base ${PS2_IOPBOOT_BASE}
            --defsym IOPBOOT_BASE -o "${PS2_IOPBOOT_BASE_RSP}"
    DEPENDS "${PS2_SOURCE_DIR}/tools/mkromdir.py" "${PS2_MANIFEST}"
            "${PS2_SOURCE_DIR}/src/rom/iopbtconf.txt"
            "${PS2_ROMVER_PATH}" reset.elf rdram.elf kernel.elf
            sysmem.elf loadcore.elf excepman.elf intrman.elf timrman.elf
            threadman.elf dmacman.elf ioman.elf modload.elf romdrv.elf
            stdio.elf eesync.elf loadfile.elf cdvdman.elf fileio.elf cdvdfsv.elf
            sio2man.elf
    COMMENT "Computing IOPBOOT's archive-relative link address")

# A bare OUTPUT command with no consumer in its own directory is pruned from
# the build graph; `iopboot.elf`'s LINK_DEPENDS on it, from the parent
# directory, is not enough to anchor it. This target is.
add_custom_target(iopboot_base ALL DEPENDS "${PS2_IOPBOOT_BASE_RSP}")
