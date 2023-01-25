# CXL.mem Simulator

## Prerequisite
```bash
$ uname -a
Linux gpu01 5.19.0-29-generic #30-Ubuntu SMP PREEMPT_DYNAMIC Wed Jan 4 12:14:09 UTC 2023 x86_64 x86_64 x86_64 GNU/Linux
```
```bash
sudo apt install llvm-dev clang libbpf-dev libclang-dev libcxxopts-dev libfmt-dev librange-v3-dev
```
## User input
Bandwidth, Latency: Both use 2 input in the vector, first for read, second for write
Weight: Use the heuristic to calculate the bandwidth
Topology: Construct the topology using newick tree syntax (1,(2,3)) stands for 
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
