# Assembles scheme packages for the dist directory.
#
#   cmake -DSOURCE_DIR=<repo> -DDIST_DIR=<out> -DSCHEMES="pinyin;english" \
#         -P cmake/StageSchemes.cmake
#
# A scheme package is a self-contained directory:
#
#   <DIST_DIR>/schemes/<id>/
#     scheme.yaml
#     <chain>.cbor
#     plugins/pime-plugin-<plugin-id>-<version>/...
#
# The plugin copies come from assets/plugins; a package never reaches outside
# its own directory at runtime, so updating one scheme cannot break another.
# SCHEMES entries are ids; each is looked up in schemes/<id>.yaml first and
# schemes/optional/<id>.yaml second.

if(NOT SOURCE_DIR OR NOT DIST_DIR OR NOT SCHEMES)
    message(FATAL_ERROR "StageSchemes: SOURCE_DIR, DIST_DIR and SCHEMES are required")
endif()

foreach(scheme_id IN LISTS SCHEMES)
    set(scheme_yaml "${SOURCE_DIR}/schemes/${scheme_id}.yaml")
    if(NOT EXISTS "${scheme_yaml}")
        set(scheme_yaml "${SOURCE_DIR}/schemes/optional/${scheme_id}.yaml")
    endif()
    if(NOT EXISTS "${scheme_yaml}")
        message(FATAL_ERROR "StageSchemes: no definition for '${scheme_id}'")
    endif()

    file(STRINGS "${scheme_yaml}" lines)
    set(id "")
    set(chain "")
    set(plugins "")
    foreach(line IN LISTS lines)
        if(line MATCHES "^id:[ \t]*([A-Za-z0-9_-]+)[ \t]*$")
            set(id "${CMAKE_MATCH_1}")
        elseif(line MATCHES "^chain:[ \t]*([^ \t#]+)")
            set(chain "${CMAKE_MATCH_1}")
        elseif(line MATCHES "id:[ \t]*([A-Za-z0-9_-]+),[ \t]*version:[ \t]*([0-9A-Za-z.+-]+)")
            list(APPEND plugins "${CMAKE_MATCH_1}-${CMAKE_MATCH_2}")
        endif()
    endforeach()

    if(NOT id OR NOT chain)
        message(FATAL_ERROR "StageSchemes: ${scheme_yaml} lacks id or chain")
    endif()
    if(NOT id STREQUAL scheme_id)
        message(FATAL_ERROR "StageSchemes: ${scheme_yaml} declares id '${id}', expected '${scheme_id}'")
    endif()

    set(out "${DIST_DIR}/schemes/${id}")
    file(REMOVE_RECURSE "${out}")
    file(MAKE_DIRECTORY "${out}/plugins")

    file(READ "${scheme_yaml}" scheme_text)
    file(WRITE "${out}/scheme.yaml" "${scheme_text}")

    set(chain_path "${SOURCE_DIR}/assets/chains/${chain}")
    if(NOT EXISTS "${chain_path}")
        message(FATAL_ERROR "StageSchemes: ${id} chain ${chain_path} missing")
    endif()
    file(COPY "${chain_path}" DESTINATION "${out}")

    foreach(package IN LISTS plugins)
        set(package_dir "${SOURCE_DIR}/assets/plugins/pime-plugin-${package}")
        if(NOT IS_DIRECTORY "${package_dir}")
            message(FATAL_ERROR "StageSchemes: ${id} needs missing package ${package_dir}")
        endif()
        file(COPY "${package_dir}" DESTINATION "${out}/plugins")
    endforeach()

    message(STATUS "scheme ${id}: ${chain} + ${plugins}")
endforeach()
