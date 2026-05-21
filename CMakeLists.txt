cmake_minimum_required(VERSION 3.25.0)
project(CXLMemSim VERSION 0.1.0)

option(CXLMEMSIM_BUILD_MICROBENCHMARKS "Build microbenchmarks" ON)
option(CXLMEMSIM_ENABLE_RDMA "Enable RDMA transport when libraries are available" ON)

if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
    add_compile_options (-fdiagnostics-color=always)
elseif ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
    add_compile_options (-fcolor-diagnostics)
endif ()
execute_process(COMMAND uname -r OUTPUT_VARIABLE arch OUTPUT_STRIP_TRAILING_WHITESPACE)
set(LINUX_SOURCE /lib/modules/${arch}/build/)

set(CMAKE_CXX_STANDARD 20)

# Always build in SERVER_MODE (bpftime dependencies removed)
add_compile_definitions(SERVER_MODE)

# Use std::format instead of fmt for spdlog (C++20)
add_compile_definitions(SPDLOG_USE_STD_FORMAT)

# Use header-only spdlog to avoid library mismatch
add_compile_definitions(SPDLOG_HEADER_ONLY)

if(CXLMEMSIM_BUILD_MICROBENCHMARKS)
    add_subdirectory(microbench)
endif()

list(APPEND CMAKE_PREFIX_PATH ${CMAKE_BINARY_DIR})
find_package(spdlog REQUIRED)
execute_process(COMMAND uname -r OUTPUT_VARIABLE arch OUTPUT_STRIP_TRAILING_WHITESPACE)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -fPIC -pthread")

find_library(ATOMIC_LIB atomic)
find_library(RT_LIB rt)
if(NOT ATOMIC_LIB)
    set(ATOMIC_LIB "" CACHE STRING "libatomic path" FORCE)
endif()
if(NOT RT_LIB)
    set(RT_LIB "" CACHE STRING "librt path" FORCE)
endif()

set(SOURCE_FILES
    src/cxlcontroller.cpp
    src/cxlendpoint.cpp
    src/dcd_gfam.cpp
    src/hdm_decoder.cpp
    src/coherency_engine.cpp
    src/policy.cpp
    src/helper.cpp
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND SOURCE_FILES
        src/incore.cpp
        src/uncore.cpp
        src/perf.cpp
        src/monitor.cpp
        src/rob.cpp
        src/lbr.cpp
        src/pebs.cpp
        src/perf.cpp
    )
endif()


# Create a static library of CXL core functionality
add_library(cxlmemsim STATIC ${SOURCE_FILES})
target_include_directories(cxlmemsim PUBLIC include ${spdlog_INCLUDE_DIR})
target_link_libraries(cxlmemsim spdlog::spdlog_header_only)

enable_testing()

# Build latency calculator
add_executable(cxlmemsim_latency src/calculateLatency.cc)
set_target_properties(cxlmemsim_latency PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

set(RDMA_LIBS)
if(CXLMEMSIM_ENABLE_RDMA AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_library(RDMACM_LIB rdmacm)
    find_library(IBVERBS_LIB ibverbs)
    if(RDMACM_LIB AND IBVERBS_LIB)
        message(STATUS "RDMA support enabled: rdmacm=${RDMACM_LIB} ibverbs=${IBVERBS_LIB}")
        add_compile_definitions(HAS_RDMA)
        set(RDMA_LIBS ${RDMACM_LIB} ${IBVERBS_LIB})
    else()
        message(STATUS "RDMA support disabled: rdmacm or ibverbs not found")
    endif()
else()
    message(STATUS "RDMA support disabled")
endif()

# Server library (shared between server executable and tests)
set(SERVER_LIB_SOURCES src/shared_memory_manager.cc src/shm_communication.cpp src/distributed_server.cpp src/tcp_communication.cpp src/rdma_communication.cpp)
add_library(cxlmemsim_server_lib STATIC ${SERVER_LIB_SOURCES})
target_include_directories(cxlmemsim_server_lib PUBLIC include src ${spdlog_INCLUDE_DIR})
target_link_libraries(cxlmemsim_server_lib cxlmemsim spdlog::spdlog_header_only ${RT_LIB} ${RDMA_LIBS})

# Build server (uses server library + main)
add_executable(cxlmemsim_server src/main_server.cc)
target_include_directories(cxlmemsim_server PRIVATE include src ${spdlog_INCLUDE_DIR})
target_link_libraries(cxlmemsim_server cxlmemsim_server_lib cxlmemsim spdlog::spdlog_header_only ${RT_LIB} ${ATOMIC_LIB})

# Build legacy CXLMemSim
add_executable(cxlmemsim_legacy src/main.cc)
target_include_directories(cxlmemsim_legacy PRIVATE include src ${spdlog_INCLUDE_DIR})
target_link_libraries(cxlmemsim_legacy cxlmemsim_server_lib cxlmemsim spdlog::spdlog_header_only ${RT_LIB} ${ATOMIC_LIB})

# Distributed SHM integration test. It opens shared-memory transport state and
# may start TCP helpers, so build it but keep it out of always-on CTest.
add_executable(test_distributed_shm tests/test_distributed_shm.cpp)
target_include_directories(test_distributed_shm PRIVATE include src ${spdlog_INCLUDE_DIR})
target_link_libraries(test_distributed_shm cxlmemsim_server_lib cxlmemsim spdlog::spdlog_header_only ${RT_LIB} ${ATOMIC_LIB})

# DCD/GFAM unit test (standalone; no server dependencies)
add_executable(test_dcd_gfam tests/test_dcd_gfam.cpp src/dcd_gfam.cpp)
target_include_directories(test_dcd_gfam PRIVATE include)
add_test(NAME test_dcd_gfam COMMAND test_dcd_gfam)

# ROB/O3CPU trace tests
add_executable(test_rob tests/test_rob.cc src/rob.cpp)
target_include_directories(test_rob PRIVATE include src ${spdlog_INCLUDE_DIR})
target_link_libraries(test_rob cxlmemsim spdlog::spdlog_header_only ${RT_LIB} ${ATOMIC_LIB})
add_test(NAME test_rob COMMAND test_rob)

add_executable(test_mem_stall tests/test_mem_stall.cc src/rob.cpp)
target_include_directories(test_mem_stall PRIVATE include src ${spdlog_INCLUDE_DIR})
target_link_libraries(test_mem_stall cxlmemsim spdlog::spdlog_header_only ${RT_LIB} ${ATOMIC_LIB})
add_test(NAME test_mem_stall COMMAND test_mem_stall)

add_executable(test_bandwidth_model tests/test_bandwidth_model.cpp)
target_include_directories(test_bandwidth_model PRIVATE include src ${spdlog_INCLUDE_DIR})
target_link_libraries(test_bandwidth_model cxlmemsim spdlog::spdlog_header_only ${RT_LIB} ${ATOMIC_LIB})
add_test(NAME test_bandwidth_model COMMAND test_bandwidth_model)

# Integration-style test clients. They require an external server/DAX setup, so
# CMake builds them but does not register them as always-on CTest tests.
add_executable(test_back_invalidation tests/test_back_invalidation.cpp)
target_include_directories(test_back_invalidation PRIVATE include src)
target_link_libraries(test_back_invalidation ${RT_LIB} ${ATOMIC_LIB})

add_executable(test_dax_back_invalidation tests/test_dax_back_invalidation.cpp)
target_include_directories(test_dax_back_invalidation PRIVATE include src)
target_link_libraries(test_dax_back_invalidation ${RT_LIB} ${ATOMIC_LIB})

# Installation targets
install(TARGETS cxlmemsim_server
        RUNTIME DESTINATION bin)

install(FILES include/tcp_communication.h include/rdma_communication.h include/dcd_gfam.h
        DESTINATION include/cxlmemsim)
