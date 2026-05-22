include_guard(GLOBAL)

set(_CXLMEMSIM_SLUG_DEFAULT_ROOT "")
get_filename_component(_CXLMEMSIM_SLUG_SIBLING_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../../cxltime" ABSOLUTE)
if(EXISTS "${_CXLMEMSIM_SLUG_SIBLING_ROOT}/CMakeLists.txt")
    set(_CXLMEMSIM_SLUG_DEFAULT_ROOT "${_CXLMEMSIM_SLUG_SIBLING_ROOT}")
endif()

set(CXLMEMSIM_CXLTIME_ROOT "${_CXLMEMSIM_SLUG_DEFAULT_ROOT}" CACHE PATH
    "Path to the cxltime checkout that contains tools/slug_allocator")

if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(_CXLMEMSIM_SLUG_BUILD_NAME build-mac-slug)
    set(_CXLMEMSIM_SLUG_DYLIB_SUFFIX dylib)
else()
    set(_CXLMEMSIM_SLUG_BUILD_NAME build-slug)
    set(_CXLMEMSIM_SLUG_DYLIB_SUFFIX so)
endif()

if(CXLMEMSIM_CXLTIME_ROOT)
    get_filename_component(_CXLMEMSIM_SLUG_DEFAULT_BUILD
        "${CXLMEMSIM_CXLTIME_ROOT}/${_CXLMEMSIM_SLUG_BUILD_NAME}" ABSOLUTE)
else()
    set(_CXLMEMSIM_SLUG_DEFAULT_BUILD "")
endif()

set(CXLMEMSIM_SLUG_BUILD_DIR "${_CXLMEMSIM_SLUG_DEFAULT_BUILD}" CACHE PATH
    "Build directory for cxltime SlugAllocator artifacts")

set(_CXLMEMSIM_SLUG_DEFAULT_LLVM_DIR "")
foreach(_CXLMEMSIM_LLVM_CANDIDATE
        /opt/homebrew/opt/llvm@21/lib/cmake/llvm
        /usr/local/opt/llvm@21/lib/cmake/llvm
        /opt/homebrew/opt/llvm/lib/cmake/llvm
        /usr/local/opt/llvm/lib/cmake/llvm)
    if(EXISTS "${_CXLMEMSIM_LLVM_CANDIDATE}/LLVMConfig.cmake")
        set(_CXLMEMSIM_SLUG_DEFAULT_LLVM_DIR "${_CXLMEMSIM_LLVM_CANDIDATE}")
        break()
    endif()
endforeach()

set(CXLMEMSIM_SLUG_LLVM_DIR "${_CXLMEMSIM_SLUG_DEFAULT_LLVM_DIR}" CACHE PATH
    "LLVM CMake package directory used to build cxltime SlugAllocator")

set(CXLMEMSIM_SLUG_PASS
    "${CXLMEMSIM_SLUG_BUILD_DIR}/tools/slug_allocator/SlugAllocatorPass.${_CXLMEMSIM_SLUG_DYLIB_SUFFIX}"
    CACHE FILEPATH "Path to the built SlugAllocator LLVM pass plugin")
set(CXLMEMSIM_SLUG_RUNTIME
    "${CXLMEMSIM_SLUG_BUILD_DIR}/tools/slug_allocator/libslug_allocator_runtime.${_CXLMEMSIM_SLUG_DYLIB_SUFFIX}"
    CACHE FILEPATH "Path to the built SlugAllocator runtime library")

if(NOT CXLMEMSIM_CXLTIME_ROOT OR NOT EXISTS "${CXLMEMSIM_CXLTIME_ROOT}/CMakeLists.txt")
    message(WARNING
        "CXLMemSim SlugAllocator integration is enabled, but cxltime was not found. "
        "Set CXLMEMSIM_CXLTIME_ROOT to a checkout that contains tools/slug_allocator.")
    return()
endif()

if(NOT EXISTS "${CXLMEMSIM_CXLTIME_ROOT}/tools/slug_allocator/CMakeLists.txt")
    message(WARNING
        "CXLMEMSIM_CXLTIME_ROOT=${CXLMEMSIM_CXLTIME_ROOT} does not contain "
        "tools/slug_allocator; skipping SlugAllocator targets.")
    return()
endif()

set(_CXLMEMSIM_SLUG_CONFIGURE_ARGS
    -S "${CXLMEMSIM_CXLTIME_ROOT}"
    -B "${CXLMEMSIM_SLUG_BUILD_DIR}"
    -DCMAKE_BUILD_TYPE=RelWithDebInfo
    -DBPFTIME_BUILD_WITH_LIBBPF=OFF
    -DBPFTIME_BUILD_KERNEL_BPF=OFF
    -DBUILD_BPFTIME_DAEMON=OFF
    -DBPFTIME_LLVM_JIT=OFF
    -DBPFTIME_UBPF_JIT=OFF
    -DBPFTIME_BUILD_RUNTIME=OFF
    -DBPFTIME_BUILD_ATTACH=OFF
    -DBPFTIME_BUILD_BPFTIME_TOOLS=OFF
    -DBPFTIME_BUILD_SLUG_ALLOCATOR=ON)

if(CXLMEMSIM_SLUG_LLVM_DIR)
    list(APPEND _CXLMEMSIM_SLUG_CONFIGURE_ARGS
        "-DLLVM_DIR=${CXLMEMSIM_SLUG_LLVM_DIR}")
endif()

add_custom_target(slugallocator_tools
    COMMAND "${CMAKE_COMMAND}" ${_CXLMEMSIM_SLUG_CONFIGURE_ARGS}
    COMMAND "${CMAKE_COMMAND}" --build "${CXLMEMSIM_SLUG_BUILD_DIR}"
            --target SlugAllocatorPass slug_allocator_runtime --parallel
    BYPRODUCTS "${CXLMEMSIM_SLUG_PASS}" "${CXLMEMSIM_SLUG_RUNTIME}"
    USES_TERMINAL
    COMMENT "Building cxltime SlugAllocator pass and runtime")

add_custom_target(slugallocator_paths
    COMMAND "${CMAKE_COMMAND}" -E echo "cxltime root: ${CXLMEMSIM_CXLTIME_ROOT}"
    COMMAND "${CMAKE_COMMAND}" -E echo "build dir: ${CXLMEMSIM_SLUG_BUILD_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E echo "LLVM_DIR: ${CXLMEMSIM_SLUG_LLVM_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E echo "pass: ${CXLMEMSIM_SLUG_PASS}"
    COMMAND "${CMAKE_COMMAND}" -E echo "runtime: ${CXLMEMSIM_SLUG_RUNTIME}")

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_custom_target(slugallocator_smoke
        COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/script/cxlmemsim_slug.py"
                smoke
                --cxltime-root "${CXLMEMSIM_CXLTIME_ROOT}"
                --build-dir "${CXLMEMSIM_SLUG_BUILD_DIR}"
                --llvm-dir "${CXLMEMSIM_SLUG_LLVM_DIR}"
                --pass "${CXLMEMSIM_SLUG_PASS}"
                --runtime "${CXLMEMSIM_SLUG_RUNTIME}"
        DEPENDS slugallocator_tools
        USES_TERMINAL
        COMMENT "Running SlugAllocator instrumentation smoke test")
else()
    message(WARNING "Python3 interpreter not found; slugallocator_smoke target is unavailable")
endif()
