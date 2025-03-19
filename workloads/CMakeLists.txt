add_subdirectory(gapbs)
set(GGML_AVX ON)
set(LLAMA_CURL ON)
add_subdirectory(llama.cpp)
#add_subdirectory(vsag)
#add_subdirectory(gromacs)

macro(memcached_build)
    set(memcached_src ${CMAKE_CURRENT_SOURCE_DIR}/memcached)
    add_custom_target(memcached_cfg ./autogen.sh
            COMMAND ./configure --enable-jobserver=no --enable-memaslap
            --enable-static --enable-shared=off
            WORKING_DIRECTORY ${memcached_src}
    )
    add_custom_target(memcached_make
            COMMAND make -j 4
            DEPENDS memcached_cfg
            WORKING_DIRECTORY ${memcached_src}
    )
    add_custom_target(memcached_copy
            COMMAND ${CMAKE_COMMAND} -E copy ${memcached_src}/memcached
            ${CMAKE_CURRENT_BINARY_DIR}/memcached
    )
    add_custom_target(memcached DEPENDS memcached_copy)
endmacro(memcached_build)

macro(memcached_ycsb_build)
    set(memcached_ycsb_src ${CMAKE_CURRENT_SOURCE_DIR}/YCSB)
    add_custom_target(memcached_ycsb_cfg
            COMMAND mvn -pl site.ycsb:memcached-binding -am clean package
            WORKING_DIRECTORY ${memcached_ycsb_src}
    )
    add_custom_target(memcached_ycsb_gen_workload
            COMMAND ./bin/ycsb load memcached -s -P workloads/workloada > outputLoad.txt
            && ./bin/ycsb run memcached -s -P workloads/workloada > outputRun.txt
            DEPENDS memcached_ycsb_cfg
            WORKING_DIRECTORY ${memcached_ycsb_src}
    )
    add_custom_target(memcached_ycsb_copy
            COMMAND ${CMAKE_COMMAND} -E copy ${memcached_ycsb_src}/outputLoad.txt
            ${CMAKE_CURRENT_BINARY_DIR}/outputLoad.txt && ${CMAKE_COMMAND} -E copy ${memcached_src}/outputRun.txt
            ${CMAKE_CURRENT_BINARY_DIR}/outputRun.txt
    )
endmacro(memcached_ycsb_build)

memcached_build()
memcached_ycsb_build()