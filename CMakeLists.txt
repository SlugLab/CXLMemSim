cmake_minimum_required(VERSION 3.25.0)
project(CXLMemSim VERSION 0.1.0)

option(CXLMEMSIM_BUILD_MICROBENCHMARKS "Build microbenchmarks" ON)
option(CXLMEMSIM_ENABLE_RDMA "Enable RDMA transport when libraries are available" ON)
option(CXLMEMSIM_ENABLE_SLUGALLOCATOR "Enable cxltime SlugAllocator compiler instrumentation integration" ON)

if(CXLMEMSIM_ENABLE_SLUGALLOCATOR)
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
    include(SlugAllocator)
endif()

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
    src/mesi_directory.cpp
    src/mesi_transaction_engine.cpp
    src/coherence_protocol_v2.cpp
    src/coherence_endpoint_cache.cpp
    src/coherence_server_v2.cpp
    src/coherence_tcp_transport_v2.cpp
    src/coherence_shm_transport_v2.cpp
    src/coherence_rdma_transport_v2.cpp
    src/endpoint_session_registry.cpp
    src/policy.cpp
    src/helper.cpp
    src/monitor.cpp
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND SOURCE_FILES
        src/incore.cpp
        src/uncore.cpp
        src/perf.cpp
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

find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME test_qemuless_mig_summary
        COMMAND ${Python3_EXECUTABLE} -m unittest discover -s tests -p test_qemuless_mig_summary.py -v)
    set_tests_properties(test_qemuless_mig_summary PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

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
set(SERVER_LIB_SOURCES src/shared_memory_manager.cc src/shm_communication.cpp src/distributed_server.cpp src/tcp_communication.cpp src/rdma_communication.cpp
                       src/coherence_server_runtime_v2.cpp)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/include/ssd_streaming_backend.h" AND
   EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/ssd_streaming_backend.cpp")
    message(STATUS "SSD streaming backend enabled")
    list(APPEND SERVER_LIB_SOURCES src/ssd_streaming_backend.cpp)
    set(CXLMEMSIM_HAS_SSD_STREAMING_BACKEND ON)
else()
    message(STATUS "SSD streaming backend disabled: backend source/header not found")
endif()
add_library(cxlmemsim_server_lib STATIC ${SERVER_LIB_SOURCES})
target_include_directories(cxlmemsim_server_lib PUBLIC include src ${spdlog_INCLUDE_DIR})
if(CXLMEMSIM_HAS_SSD_STREAMING_BACKEND)
    target_compile_definitions(cxlmemsim_server_lib PUBLIC CXLMEMSIM_HAS_SSD_STREAMING_BACKEND)
endif()
target_link_libraries(cxlmemsim_server_lib cxlmemsim spdlog::spdlog_header_only ${RT_LIB} ${RDMA_LIBS})

# Build server (uses server library + main)
add_executable(cxlmemsim_server src/main_server.cc)
target_include_directories(cxlmemsim_server PRIVATE include src ${spdlog_INCLUDE_DIR})
target_link_libraries(cxlmemsim_server cxlmemsim_server_lib cxlmemsim spdlog::spdlog_header_only ${RT_LIB} ${ATOMIC_LIB})

# Build legacy CXLMemSim. Linux uses perf/ptrace monitoring; other platforms
# build a no-PMU compatibility mode.
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

add_executable(test_pgas_controller_counters tests/test_pgas_controller_counters.cpp)
target_include_directories(test_pgas_controller_counters PRIVATE include src ${spdlog_INCLUDE_DIR})
target_link_libraries(test_pgas_controller_counters cxlmemsim spdlog::spdlog_header_only ${RT_LIB} ${ATOMIC_LIB})
add_test(NAME test_pgas_controller_counters COMMAND test_pgas_controller_counters)

add_executable(test_pgas_poll_policy tests/test_pgas_poll_policy.cpp)
target_include_directories(test_pgas_poll_policy PRIVATE include)
add_test(NAME test_pgas_poll_policy COMMAND test_pgas_poll_policy)

add_executable(test_cxl_memsim_wait_policy tests/test_cxl_memsim_wait_policy.c)
target_include_directories(test_cxl_memsim_wait_policy PRIVATE lib/qemu/include)
add_test(NAME test_cxl_memsim_wait_policy COMMAND test_cxl_memsim_wait_policy)

add_executable(test_coherence_protocol_v2 tests/test_coherence_protocol_v2.cpp)
target_include_directories(test_coherence_protocol_v2 PRIVATE include)
target_link_libraries(test_coherence_protocol_v2 cxlmemsim)
set(COHERENCE_PROTOCOL_V2_FIXTURE "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/coherence_protocol_v2_frames.tsv")
target_compile_definitions(test_coherence_protocol_v2
                           PRIVATE "COHERENCE_PROTOCOL_V2_FIXTURE_PATH=\"${COHERENCE_PROTOCOL_V2_FIXTURE}\"")
add_test(NAME test_coherence_protocol_v2 COMMAND test_coherence_protocol_v2)

add_executable(test_endpoint_session_registry
               tests/test_endpoint_session_registry.cpp
               src/coherence_protocol_v2.cpp
               src/endpoint_session_registry.cpp)
target_include_directories(test_endpoint_session_registry PRIVATE include)
target_compile_definitions(test_endpoint_session_registry PRIVATE CXLMEMSIM_ENDPOINT_SESSION_REGISTRY_TESTING)
add_test(NAME test_endpoint_session_registry COMMAND test_endpoint_session_registry)
set_tests_properties(test_endpoint_session_registry PROPERTIES TIMEOUT 30)

add_executable(test_mesi_directory tests/test_mesi_directory.cpp src/mesi_directory.cpp)
target_include_directories(test_mesi_directory PRIVATE include)
target_compile_definitions(
    test_mesi_directory PRIVATE CXLMEMSIM_MESI_DIRECTORY_TESTING CXLMEMSIM_MESI_DIRECTORY_STANDALONE)
add_test(NAME test_mesi_directory COMMAND test_mesi_directory)
set_tests_properties(test_mesi_directory PROPERTIES TIMEOUT 30)

add_executable(test_mesi_directory_engine tests/test_mesi_directory.cpp)
target_include_directories(test_mesi_directory_engine PRIVATE include)
target_link_libraries(test_mesi_directory_engine cxlmemsim)
add_test(NAME test_mesi_directory_engine COMMAND test_mesi_directory_engine)
set_tests_properties(test_mesi_directory_engine PROPERTIES TIMEOUT 30)

add_executable(test_mesi_transaction_engine tests/test_mesi_transaction_engine.cpp)
target_include_directories(test_mesi_transaction_engine PRIVATE include)
target_link_libraries(test_mesi_transaction_engine cxlmemsim)
add_test(NAME test_mesi_transaction_engine COMMAND test_mesi_transaction_engine)
set_tests_properties(test_mesi_transaction_engine PROPERTIES TIMEOUT 30)

add_executable(test_mesi_writeback_and_atomics tests/test_mesi_writeback_and_atomics.cpp)
target_include_directories(test_mesi_writeback_and_atomics PRIVATE include)
target_link_libraries(test_mesi_writeback_and_atomics cxlmemsim)
add_test(NAME test_mesi_writeback_and_atomics COMMAND test_mesi_writeback_and_atomics)
set_tests_properties(test_mesi_writeback_and_atomics PROPERTIES TIMEOUT 30)

add_executable(test_type2_coherence_litmus tests/test_type2_coherence_litmus.cpp)
target_include_directories(test_type2_coherence_litmus PRIVATE include)
target_link_libraries(test_type2_coherence_litmus cxlmemsim)
add_test(NAME test_type2_coherence_litmus COMMAND test_type2_coherence_litmus)
set_tests_properties(test_type2_coherence_litmus PROPERTIES TIMEOUT 30)

add_executable(test_type2_device_litmus_contract tests/test_type2_device_litmus_contract.c)
target_compile_options(test_type2_device_litmus_contract PRIVATE -std=gnu11)
add_test(NAME test_type2_device_litmus_contract COMMAND test_type2_device_litmus_contract)

add_executable(test_endpoint_eviction_race tests/test_endpoint_eviction_race.cpp)
target_include_directories(test_endpoint_eviction_race PRIVATE include)
target_link_libraries(test_endpoint_eviction_race cxlmemsim)
add_test(NAME test_endpoint_eviction_race COMMAND test_endpoint_eviction_race)
set_tests_properties(test_endpoint_eviction_race PROPERTIES TIMEOUT 15)

add_executable(test_endpoint_completion_semantics tests/test_endpoint_completion_semantics.cpp)
target_include_directories(test_endpoint_completion_semantics PRIVATE include)
target_link_libraries(test_endpoint_completion_semantics cxlmemsim)
add_test(NAME test_endpoint_completion_semantics COMMAND test_endpoint_completion_semantics)
set_tests_properties(test_endpoint_completion_semantics PROPERTIES TIMEOUT 15)

add_executable(test_coherence_server_v2 tests/test_coherence_server_v2.cpp)
target_include_directories(test_coherence_server_v2 PRIVATE include)
target_link_libraries(test_coherence_server_v2 cxlmemsim)
add_test(NAME test_coherence_server_v2 COMMAND test_coherence_server_v2)
set_tests_properties(test_coherence_server_v2 PROPERTIES TIMEOUT 30)

add_executable(test_coherence_tcp_transport_v2 tests/test_coherence_tcp_transport_v2.cpp)
target_include_directories(test_coherence_tcp_transport_v2 PRIVATE include)
target_link_libraries(test_coherence_tcp_transport_v2 cxlmemsim)
add_test(NAME test_coherence_tcp_transport_v2 COMMAND test_coherence_tcp_transport_v2)
set_tests_properties(test_coherence_tcp_transport_v2 PROPERTIES TIMEOUT 10)

add_executable(test_coherence_server_runtime_v2 tests/test_coherence_server_runtime_v2.cpp)
target_include_directories(test_coherence_server_runtime_v2 PRIVATE include src)
target_link_libraries(test_coherence_server_runtime_v2 cxlmemsim_server_lib cxlmemsim ${RT_LIB} ${ATOMIC_LIB})
add_test(NAME test_coherence_server_runtime_v2 COMMAND test_coherence_server_runtime_v2)
set_tests_properties(test_coherence_server_runtime_v2 PROPERTIES TIMEOUT 10)

add_executable(test_coherence_shm_transport_v2 tests/test_coherence_shm_transport_v2.cpp)
target_include_directories(test_coherence_shm_transport_v2 PRIVATE include)
target_link_libraries(test_coherence_shm_transport_v2 cxlmemsim ${RT_LIB})
add_test(NAME test_coherence_shm_transport_v2 COMMAND test_coherence_shm_transport_v2)
set_tests_properties(test_coherence_shm_transport_v2 PROPERTIES TIMEOUT 15)

add_executable(test_coherence_rdma_transport_v2 tests/test_coherence_rdma_transport_v2.cpp)
target_include_directories(test_coherence_rdma_transport_v2 PRIVATE include)
target_link_libraries(test_coherence_rdma_transport_v2 cxlmemsim)
add_test(NAME test_coherence_rdma_transport_v2 COMMAND test_coherence_rdma_transport_v2)
set_tests_properties(test_coherence_rdma_transport_v2 PROPERTIES TIMEOUT 15)

add_executable(test_mesi_reported_state_recovery tests/test_mesi_reported_state_recovery.cpp)
target_include_directories(test_mesi_reported_state_recovery PRIVATE include)
target_link_libraries(test_mesi_reported_state_recovery cxlmemsim)
add_test(NAME test_mesi_reported_state_recovery COMMAND test_mesi_reported_state_recovery)
set_tests_properties(test_mesi_reported_state_recovery PROPERTIES TIMEOUT 15)

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
