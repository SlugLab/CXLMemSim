cmake_minimum_required(VERSION 3.11.0)
project(CXL-MEM-Simulator VERSION 0.1.0)

find_package(cxxopts REQUIRED)
find_package(fmt REQUIRED)
find_package(range-v3 REQUIRED)
find_package(Clang REQUIRED)
file(GLOB_RECURSE SOURCE_FILES src/*.cpp)
message(${CLANG_INCLUDE_DIRS})
execute_process(COMMAND uname -r OUTPUT_VARIABLE arch OUTPUT_STRIP_TRAILING_WHITESPACE)
set(LINUX_SOURCE /lib/modules/${arch}/build/)
set(CMAKE_CXX_FLAGS "-Wall -g -pthread -lrt -rdynamic")
set(CMAKE_CXX_STANDARD 23)

add_executable(CXL-MEM-Simulator ${SOURCE_FILES})

include_directories(CXL-MEM-Simulator include)
target_link_libraries(CXL-MEM-Simulator cxxopts::cxxopts fmt::fmt range-v3::range-v3 elf bpf)

function(bpf prefix)
    add_custom_target(${prefix}_bpf ALL
            COMMAND ${CLANG_INCLUDE_DIRS}/../bin/clang -S -nostdinc -I${LINUX_SOURCE}/arch/x86/include -I${LINUX_SOURCE}/arch/x86/include/uapi -I${LINUX_SOURCE}/arch/x86/include/generated -I${LINUX_SOURCE}/arch/x86/include/generated/uapi -I${LINUX_SOURCE}/include -I${LINUX_SOURCE}/include/uapi -I${LINUX_SOURCE}/include/generated/uapi -I${LINUX_SOURCE}/tools/testing/selftests/bpf -include ${LINUX_SOURCE}/include/linux/kconfig.h -Werror -D__KERNEL__ -D__ASM_SYSREG_H -D__BPF_TRACING__ -D__TARGET_ARCH_x86 -Wno-unused-value -Wno-pointer-sign -Wno-compare-distinct-pointer-types -Wno-gnu-variable-sized-type-not-at-end -Wno-tautological-compare -Wno-unknown-warning-option -Wno-address-of-packed-member -O2 -g -emit-llvm -c ${CMAKE_SOURCE_DIR}/src/${prefix}.c -o ${CMAKE_BINARY_DIR}/${prefix}.ll
            COMMAND ${CLANG_INCLUDE_DIRS}/../bin/llc -march=bpf -filetype=obj -o ${CMAKE_BINARY_DIR}/${prefix}.o ${CMAKE_BINARY_DIR}/${prefix}.ll
            )
    add_dependencies(CXL-MEM-Simulator ${prefix}_bpf)
endfunction()

bpf(collectmmap)

add_subdirectory(microbench)
add_subdirectory(allocator)