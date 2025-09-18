cmake_minimum_required(VERSION 3.25.0)
project(CXLMemSim VERSION 0.1.0)

if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
    add_compile_options (-fdiagnostics-color=always)
elseif ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
    add_compile_options (-fcolor-diagnostics)
endif ()
execute_process(COMMAND uname -r OUTPUT_VARIABLE arch OUTPUT_STRIP_TRAILING_WHITESPACE)
set(LINUX_SOURCE /lib/modules/${arch}/build/)

set(CMAKE_CXX_STANDARD 26)

# Add option for server mode (disables bpftime dependencies)
option(SERVER_MODE "Build in server mode without bpftime dependencies" OFF)

if(NOT SERVER_MODE)
    # add_subdirectory(lib/bpftime)
    # add_subdirectory(workloads)
endif()

add_subdirectory(microbench)

list(APPEND CMAKE_PREFIX_PATH ${CMAKE_BINARY_DIR})

find_package(cxxopts REQUIRED)
find_package(spdlog REQUIRED)
file(GLOB_RECURSE SOURCE_FILES src/cxl*.cpp src/policy.cpp src/helper.cpp src/incore.cpp src/uncore.cpp src/perf.cpp)
set(SERVER_SOURCES src/main_server.cc src/shared_memory_manager.cc src/shm_communication.cpp)

execute_process(COMMAND uname -r OUTPUT_VARIABLE arch OUTPUT_STRIP_TRAILING_WHITESPACE)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -fPIC -pthread")
set(CMAKE_EXE_LINKER_FLAGS  "${CMAKE_EXE_LINKER_FLAGS} -latomic")

# Add compile definition for server mode
if(SERVER_MODE)
    add_compile_definitions(SERVER_MODE)
else()
    # add_subdirectory(qemu_integration)
endif()

# Create a static library of CXL core functionality
add_library(cxlmemsim STATIC ${SOURCE_FILES})
target_include_directories(cxlmemsim PUBLIC include ${cxxopts_INCLUDE_DIR} ${spdlog_INCLUDE_DIR})
target_link_libraries(cxlmemsim cxxopts::cxxopts spdlog::spdlog)

if(SERVER_MODE)
    # Build simple latency calculator for server mode
    add_executable(cxlmemsim_latency src/calculateLatency.cc)
    set_target_properties(cxlmemsim_latency PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    )
    add_executable(cxlmemsim_server ${SERVER_SOURCES})
    target_include_directories(cxlmemsim_server PRIVATE include ${cxxopts_INCLUDE_DIR} ${spdlog_INCLUDE_DIR})
    target_link_libraries(cxlmemsim_server cxlmemsim cxxopts::cxxopts spdlog::spdlog rt)
else()
    # Build full version with monitor support
    add_executable(cxlmemsim_server ${SERVER_SOURCES})
    target_include_directories(cxlmemsim_server PRIVATE include ${cxxopts_INCLUDE_DIR} ${spdlog_INCLUDE_DIR})
    target_link_libraries(cxlmemsim_server cxlmemsim cxxopts::cxxopts spdlog::spdlog rt)
endif()
# Find RDMA libraries
find_package(PkgConfig REQUIRED)
pkg_check_modules(RDMACM REQUIRED librdmacm)
pkg_check_modules(IBV REQUIRED libibverbs)

# Check if RDMA is available
option(ENABLE_RDMA "Enable RDMA support" ON)

if(ENABLE_RDMA)
    if(RDMACM_FOUND AND IBV_FOUND)
        message(STATUS "RDMA support enabled")
        add_definitions(-DHAVE_RDMA)

        # Add RDMA include directories
        include_directories(${RDMACM_INCLUDE_DIRS})
        include_directories(${IBV_INCLUDE_DIRS})

        # Add RDMA libraries
        set(RDMA_LIBRARIES ${RDMACM_LIBRARIES} ${IBV_LIBRARIES})

        # Add RDMA source files
        set(RDMA_SOURCES
            src/rdma_communication.cpp
            qemu_integration/src/cxlmemsim_server_rdma.cpp
        )

        # Create RDMA-enabled server executable
        add_executable(cxlmemsim_server_rdma ${RDMA_SOURCES})
        target_link_libraries(cxlmemsim_server_rdma
            ${RDMA_LIBRARIES}
            pthread
        )

        # Set RDMA-specific compile flags
        target_compile_options(cxlmemsim_server_rdma PRIVATE
            ${RDMACM_CFLAGS_OTHER}
            ${IBV_CFLAGS_OTHER}
        )

    else()
        message(WARNING "RDMA libraries not found. RDMA support disabled.")
        set(ENABLE_RDMA OFF)
    endif()
endif()

# Installation targets
if(ENABLE_RDMA)
    install(TARGETS cxlmemsim_server_rdma
            RUNTIME DESTINATION bin)

    install(FILES include/rdma_communication.h
            DESTINATION include/cxlmemsim)
endif()

# Export RDMA configuration
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/rdma_config.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/rdma_config.h"
)