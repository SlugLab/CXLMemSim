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

find_package(cxxopts REQUIRED)
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
    src/policy.cpp
    src/helper.cpp
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND SOURCE_FILES
        src/incore.cpp
        src/uncore.cpp
        src/perf.cpp
        src/monitor.cpp
    )
endif()


# Create a static library of CXL core functionality
add_library(cxlmemsim STATIC ${SOURCE_FILES})
target_include_directories(cxlmemsim PUBLIC include ${cxxopts_INCLUDE_DIR} ${spdlog_INCLUDE_DIR})
target_link_libraries(cxlmemsim cxxopts::cxxopts spdlog::spdlog_header_only)

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
set(SERVER_LIB_SOURCES src/shared_memory_manager.cc src/shm_communication.cpp src/distributed_server.cpp src/tcp_communication.cpp src/rdma_communication.cpp src/hdm_decoder.cpp src/coherency_engine.cpp)
add_library(cxlmemsim_server_lib STATIC ${SERVER_LIB_SOURCES})
target_include_directories(cxlmemsim_server_lib PUBLIC include src ${cxxopts_INCLUDE_DIR} ${spdlog_INCLUDE_DIR})
target_link_libraries(cxlmemsim_server_lib cxlmemsim cxxopts::cxxopts spdlog::spdlog_header_only ${RT_LIB} ${RDMA_LIBS})

# Build server (uses server library + main)
add_executable(cxlmemsim_server src/main_server.cc)
target_include_directories(cxlmemsim_server PRIVATE include src ${cxxopts_INCLUDE_DIR} ${spdlog_INCLUDE_DIR})
target_link_libraries(cxlmemsim_server cxlmemsim_server_lib cxlmemsim cxxopts::cxxopts spdlog::spdlog_header_only ${RT_LIB} ${ATOMIC_LIB})

# Distributed SHM test
add_executable(test_distributed_shm tests/test_distributed_shm.cpp)
target_include_directories(test_distributed_shm PRIVATE include src ${cxxopts_INCLUDE_DIR} ${spdlog_INCLUDE_DIR})
target_link_libraries(test_distributed_shm cxlmemsim_server_lib cxlmemsim cxxopts::cxxopts spdlog::spdlog_header_only ${RT_LIB} ${ATOMIC_LIB})

# Installation targets
install(TARGETS cxlmemsim_server
        RUNTIME DESTINATION bin)

install(FILES include/tcp_communication.h include/rdma_communication.h
        DESTINATION include/cxlmemsim)
