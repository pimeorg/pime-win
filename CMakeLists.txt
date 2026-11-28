cmake_minimum_required(VERSION 3.25)

project(pime_win VERSION 1.0.0 LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(NOT MSVC)
    message(FATAL_ERROR "pime-win targets MSVC (cl.exe). Use build.bat / vcvars64.")
endif()

# ---------------------------------------------------------------------------
# Sources of the reused pieces: pime-core (native kernel), pime-android's
# vendored WAMR runtime and the released plugin packages under assets/.
# ---------------------------------------------------------------------------
set(PIME_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../pime-core" CACHE PATH
    "pime-core checkout")
set(PIME_ANDROID_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../pime-android" CACHE PATH
    "pime-android checkout (vendored WAMR)")
set(PIME_WAMR_DIR "${PIME_ANDROID_DIR}/third_party/wamr" CACHE PATH
    "WAMR source tree")
set(PIME_DIST_DIR "${CMAKE_BINARY_DIR}/dist" CACHE PATH "Staging directory")

if(NOT EXISTS "${PIME_CORE_DIR}/include/pime/hal.h")
    message(FATAL_ERROR "pime-core not found at ${PIME_CORE_DIR}")
endif()
if(NOT EXISTS "${PIME_WAMR_DIR}/core/iwasm/include/wasm_export.h")
    message(FATAL_ERROR "WAMR not found at ${PIME_WAMR_DIR}")
endif()

# WAMR is built in an isolated directory so its global include paths and
# compile definitions do not leak into the rest of the project.
add_subdirectory(cmake/wamr)

# The kernel is linked statically: an in-proc TSF DLL is loaded into arbitrary
# applications, and DLL search order would not find pime_core.dll next to it.
set(PIME_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(PIME_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(PIME_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(PIME_SANITIZE_ADDRESS OFF CACHE BOOL "" FORCE)
set(PIME_SANITIZE_UNDEFINED OFF CACHE BOOL "" FORCE)
set(PIME_BUILD_WASM ON CACHE BOOL "" FORCE)
set(PIME_BUILD_LUA OFF CACHE BOOL "" FORCE)
set(PIME_WAMR_INCLUDE_DIR "${PIME_WAMR_DIR}/core/iwasm/include" CACHE PATH "" FORCE)
set(PIME_WAMR_LIBRARY vmlib CACHE STRING "" FORCE)
# Core's install/export rules assume a shared kernel; the Windows host stages
# a self-contained directory instead of installing pime_core.
set(CMAKE_SKIP_INSTALL_RULES ON)
add_subdirectory("${PIME_CORE_DIR}" "${CMAKE_BINARY_DIR}/pime-core")

# ---------------------------------------------------------------------------
# Shared host layers
# ---------------------------------------------------------------------------
add_library(pime_host STATIC
    src/common/win_util.cpp
    src/common/log.cpp
    src/common/settings.cpp
    src/common/chain_file.cpp
    src/common/store_key.cpp
    src/common/user_dict_format.cpp
    src/engine/core_engine.cpp
    src/tsf/ipc_client.cpp
)
target_include_directories(pime_host PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../pime-core/include"
)
target_compile_definitions(pime_host PUBLIC
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS
)
target_compile_options(pime_host PRIVATE /utf-8 /W4 /permissive-)
target_link_libraries(pime_host PUBLIC pime_core
    PRIVATE advapi32 shlwapi ole32 bcrypt crypt32)

# ---------------------------------------------------------------------------
# TSF text service
# ---------------------------------------------------------------------------
add_library(pime_tsf SHARED
    src/tsf/dllmain.cpp
    src/tsf/pime_tsf.def
    src/tsf/pime_tsf.rc
    src/tsf/register.cpp
    src/tsf/text_service.cpp
    src/tsf/display_attr.cpp
)
target_compile_definitions(pime_tsf PRIVATE PIME_TSF_BUILD)
target_compile_options(pime_tsf PRIVATE /utf-8 /W4 /permissive-)
target_link_libraries(pime_tsf PRIVATE
    pime_host
    advapi32 shlwapi ole32 oleaut32 uuid user32 gdi32 shell32
)
set_target_properties(pime_tsf PROPERTIES
    OUTPUT_NAME "pime_tsf"
    PREFIX ""
)

# ---------------------------------------------------------------------------
# Engine process: owns pime-core for every host, hot-updatable on its own.
# ---------------------------------------------------------------------------
add_executable(pime_engine WIN32
    src/engine/engine_main.cpp
    src/ui/candidate_window.cpp
)
target_compile_definitions(pime_engine PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(pime_engine PRIVATE /utf-8 /W4 /permissive-)
target_link_libraries(pime_engine PRIVATE
    pime_host ole32 user32 gdi32 d2d1 dwrite windowscodecs)
set_target_properties(pime_engine PROPERTIES
    OUTPUT_NAME "pime_engine"
    PREFIX ""
)

# ---------------------------------------------------------------------------
# Single front-end executable: settings UI by default, plus the installer,
# the headless kernel probe (--keys) and the test editor (--demo).
# ---------------------------------------------------------------------------
add_executable(pime WIN32
    src/app/main.cpp
    src/app/settings_window.cpp
    src/app/rime_dict.cpp
    src/app/pime.rc
    src/tsf/register.cpp
    src/demo/main.cpp
    src/cli/main.cpp
)
target_compile_definitions(pime PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(pime PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(pime PRIVATE /utf-8 /W4 /permissive-)
target_link_libraries(pime PRIVATE
    pime_host advapi32 shell32 comctl32 comdlg32 dwmapi uxtheme user32 gdi32
    ole32 oleaut32 uuid d2d1 dwrite)
set_target_properties(pime PROPERTIES OUTPUT_NAME "pime")

# ---------------------------------------------------------------------------
# Staging: dist/{pime.exe,pime_tsf.dll,pime_engine.exe} + dist/schemes/*
#
# Windows ships only the pinyin and english scheme packages. Each package is
# self-contained (chain + the plugin copies it needs), so the default install
# does not need the shared assets/plugins or assets/chains directories.
# ---------------------------------------------------------------------------
set(PIME_DEFAULT_SCHEMES pinyin english)
set(PIME_OPTIONAL_SCHEMES
    shuangpin-xiaohe shuangpin-ziranma shuangpin-microsoft shuangpin-sogou
    wubi86 cangjie)

add_custom_target(pime_dist ALL
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${PIME_DIST_DIR}"
    # The shared plugins/ and chains/ directories are gone; remove copies left
    # by an older build so the default install really only has two schemes.
    COMMAND "${CMAKE_COMMAND}" -E rm -rf
            "${PIME_DIST_DIR}/plugins" "${PIME_DIST_DIR}/chains"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/mode_en.ico"
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/mode_zh.ico"
            "${PIME_DIST_DIR}"
    COMMAND "${CMAKE_COMMAND}"
            "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
            "-DDIST_DIR=${PIME_DIST_DIR}"
            "-DSCHEMES=${PIME_DEFAULT_SCHEMES}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/StageSchemes.cmake"
    COMMAND "${CMAKE_COMMAND}"
            "-DSOURCE=$<TARGET_FILE:pime_tsf>"
            "-DDEST_DIR=${PIME_DIST_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/StageBinary.cmake"
    COMMAND "${CMAKE_COMMAND}"
            "-DSOURCE=$<TARGET_FILE:pime>"
            "-DDEST_DIR=${PIME_DIST_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/StageBinary.cmake"
    COMMAND "${CMAKE_COMMAND}"
            "-DSOURCE=$<TARGET_FILE:pime_engine>"
            "-DDEST_DIR=${PIME_DIST_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/StageBinary.cmake"
    DEPENDS pime_tsf pime pime_engine
    COMMENT "Staging PIME into ${PIME_DIST_DIR}"
)

# Optional schemes stay in the repo but are not part of the default install.
# Users copy a package into %LOCALAPPDATA%\PIME\schemes or import it from the
# settings window.
add_custom_target(pime_optional_schemes
    COMMAND "${CMAKE_COMMAND}"
            "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
            "-DDIST_DIR=${CMAKE_BINARY_DIR}/optional"
            "-DSCHEMES=${PIME_OPTIONAL_SCHEMES}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/StageSchemes.cmake"
    COMMENT "Staging optional scheme packages into ${CMAKE_BINARY_DIR}/optional"
)

add_custom_target(pime_run
    COMMAND "${PIME_DIST_DIR}/pime.exe" --keys nihao --res "${PIME_DIST_DIR}"
    DEPENDS pime_dist
    COMMENT "Probe: nihao -> candidates")
