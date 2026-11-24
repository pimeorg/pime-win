# WAMR runtime used by pime-core's WASM plugin host.
#
# The same option set as the Android build: fast interpreter only, no AOT/JIT,
# no WASI/pthreads. Keeping it in its own directory confines WAMR's global
# include paths and definitions.
cmake_minimum_required(VERSION 3.14)

set(WAMR_ROOT_DIR "${PIME_WAMR_DIR}")

set(WAMR_BUILD_PLATFORM "windows")
set(WAMR_BUILD_TARGET "X86_64")
set(WAMR_BUILD_INTERP 1)
set(WAMR_BUILD_FAST_INTERP 1)
set(WAMR_BUILD_AOT 0)
set(WAMR_BUILD_JIT 0)
set(WAMR_BUILD_FAST_JIT 0)
set(WAMR_BUILD_LIBC_BUILTIN 0)
set(WAMR_BUILD_LIBC_WASI 0)
set(WAMR_BUILD_MULTI_MODULE 0)
set(WAMR_BUILD_LIB_PTHREAD 0)
set(WAMR_BUILD_LIB_WASI_THREADS 0)
set(WAMR_BUILD_APP_FRAMEWORK 0)
set(WAMR_BUILD_SIMD 0)
set(WAMR_BUILD_TAIL_CALL 1)
set(WAMR_BUILD_BULK_MEMORY 1)
set(WAMR_BUILD_REF_TYPES 0)
set(WAMR_BUILD_MINI_LOADER 0)

include("${WAMR_ROOT_DIR}/build-scripts/runtime_lib.cmake")
include("${WAMR_ROOT_DIR}/core/shared/utils/uncommon/shared_uncommon.cmake")

add_library(vmlib STATIC ${WAMR_RUNTIME_LIB_SOURCE})
target_include_directories(vmlib PUBLIC
    "${WAMR_ROOT_DIR}/core/iwasm/include")
# wasm_export.h marks its API dllimport unless this is defined. The runtime is
# linked statically, so consumers of the header must see the export form.
target_compile_definitions(vmlib INTERFACE COMPILING_WASM_RUNTIME_API=1)
target_link_libraries(vmlib PRIVATE ntdll)
set_target_properties(vmlib PROPERTIES OUTPUT_NAME "pime_wamr")
