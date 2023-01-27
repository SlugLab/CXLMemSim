#!/bin/bash

sudo apt install llvm-dev clang libbpf-dev libclang-dev libcxxopts-dev libfmt-dev librange-v3-dev ninja-build

mkdir build
cd build
cmake -GNinja ..
ninja

sudo bash -c "LOGV=0 ./CXL-MEM-Simulator -t './microbench/many_calloc'" > many_calloc_cxlmemsim.txt
time ./microbench/many_calloc > many_calloc_time.txt
sudo bash -c "LOGV=0 ./CXL-MEM-Simulator -t './microbench/many_mmap_write'" > many_mmap_write_cxlmemsim.txt
time ./microbench/many_mmap_write > many_mmap_write_time.txt
sudo bash -c "LOGV=0 ./CXL-MEM-Simulator -t './microbench/many_mmap_read'" > many_mmap_read_cxlmemsim.txt
time ./microbench/many_mmap_read > many_mmap_read_time.txt
sudo bash -c "LOGV=0 ./CXL-MEM-Simulator -t './microbench/many_malloc'" > many_malloc_cxlmemsim.txt
time ./microbench/many_malloc > many_malloc_time.txt
sudo bash -c "LOGV=0 ./CXL-MEM-Simulator -t './microbench/many_sbrk'" > many_sbrk_cxlmemsim.txt
time ./microbench/many_sbrk > many_sbrk_time.txt