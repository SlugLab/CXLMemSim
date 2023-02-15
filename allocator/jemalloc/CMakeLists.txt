if (SANITIZE OR NOT (
	((OS_LINUX OR OS_FREEBSD) AND (ARCH_AMD64 OR ARCH_ARM OR ARCH_PPC64LE OR ARCH_RISCV64)) OR
    (OS_DARWIN AND (CMAKE_BUILD_TYPE_UC STREQUAL "RELWITHDEBINFO" OR CMAKE_BUILD_TYPE_UC STREQUAL "DEBUG"))
))
    if (ENABLE_JEMALLOC)
        message (${RECONFIGURE_MESSAGE_LEVEL}
                 "jemalloc is disabled implicitly: it doesn't work with sanitizers and can only be used with x86_64, aarch64, or ppc64le Linux or FreeBSD builds and RelWithDebInfo macOS builds.")
    endif ()
    set (ENABLE_JEMALLOC OFF)
else ()
    option (ENABLE_JEMALLOC "Enable jemalloc allocator" ${ENABLE_LIBRARIES})
endif ()


# ThreadPool select job randomly, and there can be some threads that had been
# performed some memory heavy task before and will be inactive for some time,
# but until it will became active again, the memory will not be freed since by
# default each thread has it's own arena, but there should be not more then
# 4*CPU arenas (see opt.nareans description).
#
# By enabling percpu_arena number of arenas limited to number of CPUs and hence
# this problem should go away.
#
# muzzy_decay_ms -- use MADV_FREE when available on newer Linuxes, to
# avoid spurious latencies and additional work associated with
# MADV_DONTNEED. See
# https://github.com/ClickHouse/ClickHouse/issues/11121 for motivation.
set (JEMALLOC_CONFIG_MALLOC_CONF "percpu_arena:percpu,oversize_threshold:0,muzzy_decay_ms:5000,dirty_decay_ms:5000")

# CACHE variable is empty, to allow changing defaults without necessity
# to purge cache
set (JEMALLOC_CONFIG_MALLOC_CONF_OVERRIDE "" CACHE STRING "Change default configuration string of JEMalloc" )
if (JEMALLOC_CONFIG_MALLOC_CONF_OVERRIDE)
    set (JEMALLOC_CONFIG_MALLOC_CONF "${JEMALLOC_CONFIG_MALLOC_CONF_OVERRIDE}")
endif()
message (STATUS "jemalloc malloc_conf: ${JEMALLOC_CONFIG_MALLOC_CONF}")

file(GLOB_RECURSE SOURCE_FILES src/*.c)

add_library(_jemalloc ${SOURCE_FILES})
# First include jemalloc-cmake files, to override anything that jemalloc has.
# (for example if you were trying to build jemalloc inside contrib/jemalloc you
# will have some files that may be out of date)
target_include_directories(_jemalloc PUBLIC include)
target_include_directories(_jemalloc PRIVATE "${LIBRARY_DIR}/include")

set (JEMALLOC_INCLUDE_PREFIX)
# OS_
set (JEMALLOC_INCLUDE_PREFIX "include_linux")

# ARCH_
set(JEMALLOC_INCLUDE_PREFIX "${JEMALLOC_INCLUDE_PREFIX}_x86_64")

configure_file(jemalloc_internal_defs.h.in
    ${JEMALLOC_INCLUDE_PREFIX}/jemalloc/internal/jemalloc_internal_defs.h)
target_include_directories(_jemalloc SYSTEM PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/${JEMALLOC_INCLUDE_PREFIX}/jemalloc/internal")

target_compile_definitions(_jemalloc PRIVATE -DJEMALLOC_NO_PRIVATE_NAMESPACE)

if (CMAKE_BUILD_TYPE_UC STREQUAL "DEBUG")
    target_compile_definitions(_jemalloc PRIVATE
        -DJEMALLOC_DEBUG=1
        # Usage examples:
        # - MALLOC_CONF=log:.
        # - MALLOC_CONF='log:core.malloc.exit|core.sallocx.entry|core.sdallocx.entry'
        -DJEMALLOC_LOG=1)
endif ()

target_compile_definitions(_jemalloc PRIVATE -DJEMALLOC_PROF=1)

target_compile_options(_jemalloc PRIVATE -Wno-redundant-decls)
# for RTLD_NEXT
target_compile_options(_jemalloc PRIVATE -D_GNU_SOURCE)

add_library(jemalloc ALIAS _jemalloc)
