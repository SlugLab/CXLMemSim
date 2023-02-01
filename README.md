# CXL.mem Simulator
The epoch design of this project is mostly refering to [mes](https://github.com/takahiro-hirofuchi/mesmeric-emulator), the novelty is use pebs to construct the topology and calculate the hierachy latency based on this. See the [talk](https://docs.google.com/file/d/1bZi2rbB-u5xMw_YET726gb2s9QuxMZJE/edit?usp=docslist_api&filetype=mspresentation)

## Prerequisite
```bash
$ uname -a
Linux gpu01 5.19.0-29-generic #30-Ubuntu SMP PREEMPT_DYNAMIC Wed Jan 4 12:14:09 UTC 2023 x86_64 x86_64 x86_64 GNU/Linux
$ sudo apt install llvm-dev clang libbpf-dev libclang-dev libcxxopts-dev libfmt-dev librange-v3-dev
```
## User input
```bash
./CXL-MEM-Simulator -t ./microbench/many_calloc -i 5 -c 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14
```
1. -t Target: The path to the executable
2. -i Interval: The epoch of the simulator, the parameter is in milisecond
3. -c CPUSet: The core id to run the executable and the rest will be `setaffinity` to one other core
4. -d Dram Latency: The current platform's DRAM latency, default is 85ns
5. -b, -l Bandwidth, Latency: Both use 2 input in the vector, first for read, second for write
6. -c Capacity: The capacity of the memory with first be local, remaining accordingly to the input vector.
7. -w Weight: Use the heuristic to calculate the bandwidth
8. -o Topology: Construct the topology using newick tree syntax (1,(2,3)) stands for 
```bash
            1
          /
0 - local
          \
                   2
         switch  / 
                 \ 
                  3
```
## Limitation
The pebs requires no larger than 5 `perf_open_event` attached to certain PID, so I limit the bpf program to munmap(kprobe) and sbrk(kprobe/kretprobe), you can configure them. For multiple process application, I need to first SIGSTOP the process and `send/recv` back the PID information. For client and server application, I need to SIGSTOP/SIGCONT on both client and server simultaneously, which is not implemented yet.