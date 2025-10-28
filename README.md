# CXLMemSim and MEMU

# CXLMemSim
The CXL.mem simulator uses the target latency for simulating the CPU perspective taking ROB and different cacheline states into penalty from the application level.

## Prerequisite
```bash
root@victoryang00-ASUS-Zenbook-S-14-UX5406SA-UX5406SA:/home/victoryang00/CLionProjects/CXLMemSim-dev/build# uname -a
Linux victoryang00-ASUS-Zenbook-S-14-UX5406SA-UX5406SA 6.13.0-rc4+ #12 SMP PREEMPT_DYNAMIC Fri Jan 24 07:08:46 CST 2025 x86_64 x86_64 x86_64 GNU/Linux
```
## User input
```bash
SPDLOG_LEVEL=debug ./CXLMemSim -t ./microbench/ld -i 5 -c 0,2 -d 85 -c 100,100 -w 85.5,86.5,87.5,85.5,86.5,87.5,88. -o "(1,(2,3))"
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
9. env SPDLOG_LEVEL stands for logs level that you can see.

## Cite
```bash
@article{yangyarch23,
  title={CXLMemSim: A pure software simulated CXL.mem for performance characterization},
  author={Yiwei Yang, Pooneh Safayenikoo, Jiacheng Ma, Tanvir Ahmed Khan, Andrew Quinn},
  journal={arXiv preprint arXiv:2303.06153},
  booktitle={The fifth Young Architect Workshop (YArch'23)},
  year={2023}
}
```

# MEMU

Compute Express Link (CXL) 3.0 introduces powerful memory pooling and promises to transform datacenter architectures. However, the lack of available CXL 3.0 hardware and the complexity of multi-host configurations pose significant challenges to the community. This paper presents MEMU, a comprehensive emulation framework that enables full CXL 3.0 functionality, including multi-host memory sharing and pooling support. MEMU provides emulation of CXL 3.0 features—such as fabric management, dynamic memory allocation, and coherent memory sharing across multiple hosts—in advance of real hardware availability. An evaluation of MEMU shows that it achieves performance within about 3x of projected native CXL 3.0 speeds having complete compatibility with existing CXL software stacks. We demonstrate the utility of MEMU through a case study on Genomics Pipeline, observing up to a 15% improvement in application performance compared to traditional RDMA-based approaches. MEMU is open-source and publicly available, aiming to accelerate CXL 3.0 research and development.

```bash
sudo ip link add br0 type bridge
sudo ip link set br0 up
sudo ip addr add 192.168.100.1/24 dev br0
for i in 0; do
    sudo ip tuntap add tap$i mode tap
    sudo ip link set tap$i up
    sudo ip link set tap$i master br0
done
mkdir build
cd build
wget https://asplos.dev/about/qemu.img
wget https://asplos.dev/about/bzImage
cp qemu.img qemu1.img
../qemu_integration/launch_qemu_cxl1.sh
# in qemu
vi /usr/local/bin/*.sh
# change 192.168.100.10 to 11
vi /etc/hostname
# change node0 to node1
exit
# out of qemu
../qemu_integration/launch_qemu_cxl.sh &
../qemu_integration/launch_qemu_cxl1.sh &
```

for multiple hosts, you'll need vxlan

```bash
#!/bin/bash
set -eux

DEV=enp23s0f0np0
BR=br0
VNI=100
MCAST=239.1.1.1
BR_IP_SUFFIX=$(hostname | grep -oE '[0-9]+$' || echo 1)   # optional auto-index
# Or set manually:
# BR_IP_SUFFIX=<1..4>

# Clean up
ip link del $BR 2>/dev/null || true
ip link del vxlan$VNI 2>/dev/null || true

# Create bridge
ip link add $BR type bridge
ip link set $BR up

# Create multicast VXLAN (no remote attribute!)
ip link add vxlan$VNI type vxlan id $VNI group $MCAST dev $DEV dstport 4789 ttl 10
ip link set vxlan$VNI up
ip link set vxlan$VNI master $BR

# Assign overlay IP
ip addr add 192.168.100.$BR_IP_SUFFIX/24 dev $BR

# Optional: add local TAPs for QEMU
for i in 0 1; do
    ip tuntap add tap$i mode tap
    ip link set tap$i up
    ip link set tap$i master $BR
done

echo "Bridge $BR ready on host $(hostname)"
```
for every host and edit the qemu's ip with `/usr/local/bin/setup*` and `/etc/hostname`.

