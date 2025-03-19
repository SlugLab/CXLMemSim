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
add_subdirectory(lib/bpftime)
add_subdirectory(microbench)
#add_subdirectory(workloads)

list(APPEND CMAKE_PREFIX_PATH ${CMAKE_BINARY_DIR})

find_package(cxxopts REQUIRED)
#find_package(spdlog REQUIRED)
file(GLOB_RECURSE SOURCE_FILES src/*.cpp)

execute_process(COMMAND uname -r OUTPUT_VARIABLE arch OUTPUT_STRIP_TRAILING_WHITESPACE)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -fPIC -pthread")
set(CMAKE_EXE_LINKER_FLAGS  "${CMAKE_EXE_LINKER_FLAGS} -latomic")

add_executable(CXLMemSim ${SOURCE_FILES} src/main.cc)

include_directories(CXLMemSim include ${cxxopts_INCLUDE_DIR} ${spdlog_INCLUDE_DIR} ${runtime_SOURCE_DIR}/include)
target_link_libraries(CXLMemSim cxxopts::cxxopts bpftime_vm bpftime-object bpftime_base_attach_impl bpftime-agent )

add_executable(CXLMemSimRoB ${SOURCE_FILES} src/rob.cc)

include_directories(CXLMemSimRoB include ${cxxopts_INCLUDE_DIR} ${spdlog_INCLUDE_DIR} ${runtime_SOURCE_DIR}/include)
target_link_libraries(CXLMemSimRoB cxxopts::cxxopts bpftime_vm bpftime-object bpftime_base_attach_impl bpftime-agent)


function(bpf prefix)
    add_custom_target(${prefix}_bpf ALL
            COMMAND clang --target=bpf -nostdinc -S -I/usr/include/linux -I${CMAKE_SOURCE_DIR}/include -I${LINUX_SOURCE}/arch/x86/include -I/usr/include -I${LINUX_SOURCE}/arch/x86/include/uapi -I${LINUX_SOURCE}/arch/x86/include/generated -I${LINUX_SOURCE}/arch/x86/include/generated/uapi -I${LINUX_SOURCE}/include -I${LINUX_SOURCE}/include/uapi -I${LINUX_SOURCE}/include/generated/uapi -I${LINUX_SOURCE}/tools/testing/selftests/bpf -include ${LINUX_SOURCE}/include/linux/kconfig.h -D__KERNEL__ -D__ASM_SYSREG_H -D__BPF_TRACING__ -D__TARGET_ARCH_x86 -Wno-implicit-function-declaration -O3 -emit-llvm -g -c ${CMAKE_SOURCE_DIR}/src/${prefix}.bpf.c -o ${CMAKE_BINARY_DIR}/${prefix}.ll
            COMMAND llc -march=bpf -filetype=obj -o ${CMAKE_BINARY_DIR}/${prefix}.o ${CMAKE_BINARY_DIR}/${prefix}.ll
            )
    add_dependencies(CXLMemSim ${prefix}_bpf)
endfunction()

bpf(cxlmemsim)
