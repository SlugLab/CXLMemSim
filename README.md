# CXL.mem Simulator
The CXL.mem simulator is to use the target latency for simulating the CPU perspective taking ROB and different cacheline state's into panelty from the application level.

## Prerequisite
```bash
$ uname -a
Linux banana 6.4.0+ #86 SMP PREEMPT_DYNAMIC Fri Jul 28 23:49:33 UTC 2023 x86_64 x86_64 x86_64 GNU/Linux
$ echo 0 | sudo tee /sys/devices/system/node/node1/cpu*/online >/dev/null 2>&1
```
## User input
```bash
LOGV=1 ./CXL-MEM-Simulator -t ./microbench/ld -i 5 -c 0,2 -d 85 -b 10,10 -l 100,100 -c 100,100 -w 85.5,86.5,87.5,85.5,86.5,87.5,88. -o "(1,(2,3))"
```
1. -t Target: The path to the executable
2. -i Interval: The epoch of the simulator, the parameter is in milisecond
3. -c CPUSet: The core id to run the executable and the rest will be `setaffinity` to one other core
4. -d Dram Latency: The current platform's DRAM latency, default is 85ns # mark that bw in the remote
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
9. env LOGV stands for logs level that you can see.
