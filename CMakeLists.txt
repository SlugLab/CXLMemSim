cmake_minimum_required(VERSION 3.11.0)
project(CXL-MEM-Simulator VERSION 0.1.0)

find_package(cxxopts REQUIRED)
file(GLOB_RECURSE SOURCE_FILES src/*.cpp)

set(CMAKE_CXX_FLAGS "-Wall -g -pthread -lrt -rdynamic")
set(CMAKE_CXX_STANDARD 23)

add_executable(CXL-MEM-Simulator ${SOURCE_FILES})

include_directories(CXL-MEM-Simulator include)
target_link_libraries(CXL-MEM-Simulator cxxopts::cxxopts)
add_subdirectory(microbench)