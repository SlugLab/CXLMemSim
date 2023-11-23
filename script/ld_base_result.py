#!/usr/bin/env python3

import subprocess
import time
import csv
import sys, os

workloads = ["mlc", "ld", "st", "nt-ld", "nt-st", "ptr-chasing"]


def run_command(size, mem_node):
    start_time = time.time()
    cmd = [
        f"/usr/bin/numactl -m {mem_node} ../cmake-build-debug/microbench/ld_base" + str(size),
    ]
    print(cmd)
    process = subprocess.Popen(
        cmd, shell=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )

    out, err = process.communicate()
    print(f"err: {err}, out: {out}")
    return int(out)


def run_cxlmemsim_command(size, mem_node):
    # start_time = time.time()
    cmd = [
        "LOGV=1",
        f"/usr/bin/numactl -m {mem_node}",
        "../cmake-build-debug/CXLMemSim",
        "-t",
        "../cmake-build-debug/microbench/ld" + str(size),
        "-i",
        "100",
    ]
    cmd = " ".join(cmd)
    print(cmd)
    os.system(cmd)
    # end_time = time.time()
    df = pd.read_csv("./output_pmu.csv")
    os.system(f"mv ./output_pmu.csv ./ld_pmu{size}_results.csv")
    return df

def execute(cmd):
    print(cmd)
    process = subprocess.Popen(
        cmd, shell=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )

    out, err = process.communicate()
    print(f"err: {err}, out: {out}")
    return out
    

def main():
    prefetching_off = [f"wrmsr -a 0x1a4 0xf"]
    prefetching_on = [f"wrmsr -a 0x1a4 0xf"]
    
    sizes = [2**x for x in range(0, 9)]

    mode = "local"
    mem_node = 0 if mode == "local" else 1


    execute(prefetching_off)
    
    f = open(f"ld_base_results_{mode}.csv", "a")
    writer = csv.writer(f, delimiter=",")
    writer.writerow(["size", "time"])
    for i in range(5):
        for size in sizes:
            exec_time = run_command(size, mem_node)
            writer.writerow([size, exec_time])

    execute(prefetching_on)
    # for size in sizes:
    #     df = run_cxlmemsim_command(size,1)


if __name__ == "__main__":
    main()
