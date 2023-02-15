file(GLOB_RECURSE SOURCE_FILES *.cpp)
add_executable(CXL-Mem-Alloctor ${SOURCE_FILES})
add_subdirectory(jemalloc)