# Copies a built binary into the dist directory.
#
#   cmake -DSOURCE=<file> -DDEST_DIR=<dir> -P cmake/StageBinary.cmake
#
# Windows refuses to overwrite a DLL/EXE that a process has loaded. Try to move
# the old copy to <name>.old first; if the rename or copy is denied, keep the
# existing file and warn instead of failing the whole build. The stale binary
# takes effect after the host that loaded it exits and the build is rerun.

if(NOT SOURCE OR NOT DEST_DIR)
    message(FATAL_ERROR "StageBinary: SOURCE and DEST_DIR are required")
endif()

get_filename_component(name "${SOURCE}" NAME)
set(dest "${DEST_DIR}/${name}")

if(EXISTS "${dest}")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E rm -f "${dest}.old"
                    OUTPUT_QUIET ERROR_QUIET)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E rename "${dest}" "${dest}.old"
                    RESULT_VARIABLE renamed
                    OUTPUT_QUIET ERROR_QUIET)
    if(NOT renamed EQUAL 0)
        message(STATUS "StageBinary: ${name} is loaded; staging a copy instead")
    endif()
endif()

execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                        "${SOURCE}" "${dest}"
                RESULT_VARIABLE copied
                OUTPUT_QUIET ERROR_QUIET)
if(copied EQUAL 0)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E rm -f "${dest}.old"
                    OUTPUT_QUIET ERROR_QUIET)
else()
    if(EXISTS "${dest}.old" AND NOT EXISTS "${dest}")
        execute_process(COMMAND "${CMAKE_COMMAND}" -E rename "${dest}.old"
                                "${dest}"
                        OUTPUT_QUIET ERROR_QUIET)
    endif()
    message(WARNING "StageBinary: could not replace ${name}; a host has it loaded. "
                    "Close that program and rebuild to update it.")
endif()
