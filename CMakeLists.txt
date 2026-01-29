cmake_minimum_required(VERSION 3.25.0)
project(CXLMemSim VERSION 0.1.0)

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

add_subdirectory(microbench)

list(APPEND CMAKE_PREFIX_PATH ${CMAKE_BINARY_DIR})

find_package(cxxopts REQUIRED)
find_package(spdlog REQUIRED)
file(GLOB_RECURSE SOURCE_FILES src/cxl*.cpp src/policy.cpp src/helper.cpp src/incore.cpp src/uncore.cpp src/perf.cpp src/monitor.cpp)

execute_process(COMMAND uname -r OUTPUT_VARIABLE arch OUTPUT_STRIP_TRAILING_WHITESPACE)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -fPIC -pthread")
set(CMAKE_EXE_LINKER_FLAGS  "${CMAKE_EXE_LINKER_FLAGS} -latomic")


# Create a static library of CXL core functionality
add_library(cxlmemsim STATIC ${SOURCE_FILES})
target_include_directories(cxlmemsim PUBLIC include ${cxxopts_INCLUDE_DIR} ${spdlog_INCLUDE_DIR})
target_link_libraries(cxlmemsim cxxopts::cxxopts spdlog::spdlog_header_only)

# Build latency calculator
add_executable(cxlmemsim_latency src/calculateLatency.cc)
set_target_properties(cxlmemsim_latency PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

# Server library (shared between server executable and tests)
set(SERVER_LIB_SOURCES src/shared_memory_manager.cc src/shm_communication.cpp src/distributed_server.cpp src/tcp_communication.cpp src/hdm_decoder.cpp src/coherency_engine.cpp)
add_library(cxlmemsim_server_lib STATIC ${SERVER_LIB_SOURCES})
target_include_directories(cxlmemsim_server_lib PUBLIC include src ${cxxopts_INCLUDE_DIR} ${spdlog_INCLUDE_DIR})
target_link_libraries(cxlmemsim_server_lib cxlmemsim cxxopts::cxxopts spdlog::spdlog_header_only rt)

# Build server (uses server library + main)
add_executable(cxlmemsim_server src/main_server.cc)
target_include_directories(cxlmemsim_server PRIVATE include src ${cxxopts_INCLUDE_DIR} ${spdlog_INCLUDE_DIR})
target_link_libraries(cxlmemsim_server cxlmemsim_server_lib cxlmemsim cxxopts::cxxopts spdlog::spdlog_header_only rt)

# Distributed SHM test
add_executable(test_distributed_shm tests/test_distributed_shm.cpp)
target_include_directories(test_distributed_shm PRIVATE include src ${cxxopts_INCLUDE_DIR} ${spdlog_INCLUDE_DIR})
target_link_libraries(test_distributed_shm cxlmemsim_server_lib cxlmemsim cxxopts::cxxopts spdlog::spdlog_header_only rt)

# Installation targets
install(TARGETS cxlmemsim_server
        RUNTIME DESTINATION bin)

install(FILES include/tcp_communication.h
        DESTINATION include/cxlmemsim)