#!/bin/bash
cd build
git clone https://github.com/fadedzipper/gem5-cxl -b cxl.mem-dev
cd gem5-cxl
scons build/ARM/gem5.opt -j 16
time build/X86/gem5.opt configs/example/se.py -c ../microbench/many_calloc > many_calloc_gem5.txt
time build/X86/gem5.opt configs/example/se.py -c ../microbench/many_mmap_write > many_mmap_write_gem5.txt
time build/X86/gem5.opt configs/example/se.py -c ../microbench/many_mmap_read > many_mmap_read_gem5.txt
time build/X86/gem5.opt configs/example/se.py -c ../microbench/many_malloc > many_malloc_gem5.txt
time build/X86/gem5.opt configs/example/se.py -c ../microbench/many_sbrk > many_sbrk_gem5.txt