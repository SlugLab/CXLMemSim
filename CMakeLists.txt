cmake_minimum_required(VERSION 3.11.0)
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
file(GLOB_RECURSE SOURCE_FILES src/cxl*.cpp src/shared_memory_manager.cc src/policy.cpp src/helper.cpp src/incore.cpp src/uncore.cpp src/perf.cpp)

execute_process(COMMAND uname -r OUTPUT_VARIABLE arch OUTPUT_STRIP_TRAILING_WHITESPACE)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -fPIC -pthread")
set(CMAKE_EXE_LINKER_FLAGS  "${CMAKE_EXE_LINKER_FLAGS} -latomic")

# Add compile definition for server mode
if(SERVER_MODE)
    add_compile_definitions(SERVER_MODE)
else()
    add_subdirectory(qemu_integration)
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
    add_executable(CXLMemSim ${SOURCE_FILES} src/main_server.cc)
    target_include_directories(CXLMemSim PRIVATE include ${cxxopts_INCLUDE_DIR} ${spdlog_INCLUDE_DIR})
    target_link_libraries(CXLMemSim cxxopts::cxxopts spdlog::spdlog)
else()
    # Build full version with monitor support
    add_executable(CXLMemSimServer ${SOURCE_FILES} src/main_server.cc)
    include_directories(CXLMemSimServer include ${cxxopts_INCLUDE_DIR} ${spdlog_INCLUDE_DIR} ${runtime_SOURCE_DIR}/include)
    target_link_libraries(CXLMemSimServer cxxopts::cxxopts spdlog::spdlog)
endif()
