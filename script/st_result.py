#!/usr/bin/env python

import subprocess
import time
import csv
import sys, os
import pandas as pd

workloads = ["mlc", "ld", "st", "nt-ld", "nt-st", "ptr-chasing"]


def run_command(size, mem_node):
    start_time = time.time()
    cmd = [
        f"/usr/bin/numactl -m {mem_node} ../cmake-build-debug/microbench/st" + str(size),
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
        "../cmake-build-debug/microbench/st" + str(size),
        "-i",
        "100",
    ]
    cmd = " ".join(cmd)
    print(cmd)
    os.system(cmd)
    # end_time = time.time()
    df = pd.read_csv("./output_pmu.csv")
    os.system(f"mv ./output_pmu.csv ./st_pmu{size}_results.csv")
    return df


def main():
    sizes = [2**x for x in range(0, 9)]

    mode = "remote"
    mem_node = 0 if mode == "local" else 1

        
    f = open(f"st_results_{mode}.csv", "a")

    writer = csv.writer(f, delimiter=",")

    writer.writerow(["size", "time"])
    for i in range(25):
        for size in sizes:
            exec_time = run_command(size, mem_node)
            writer.writerow([size, exec_time])

    # for size in sizes:
    #     df = run_cxlmemsim_command(size,1)


if __name__ == "__main__":
    main()
