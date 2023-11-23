import subprocess
import time
import matplotlib.pyplot as plt
import pandas as pd
import os, csv
import re

workloads = ["mlc", "ld", "st", "nt-ld", "nt-st", "ptr-chasing"]


def run_command(size, mem_node):
    start_time = time.time()
    cmd = [
        f"/usr/bin/numactl -m {mem_node} ../../MLC/Linux/mlc  --loaded_latency -W"
        + str(size),
    ]
    print(cmd)
    process = subprocess.Popen(
        cmd, shell=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    print(f"err: {err}, out: {out}")

    out, err = process.communicate()

    regex_pattern = r"\t(\d+)\.\d+\t\s*(\d+)\.\d+"

    # Find all matches
    matches = re.findall(regex_pattern, out)

    print(f"err: {err}, out: {matches}")
    return int(out)


def run_cxlmemsim_command(size, mem_node):
    # start_time = time.time()
    cmd = [
        "LOGV=1",
        f"/usr/bin/numactl -m {mem_node}",
        "../cmake-build-debug/CXLMemSim",
        "-t",
        f"'../../MLC/Linux/mlc  --loaded_latency -W{size}'",
        "-i",
        "100",
        "-c",
        '0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23'
    ]
    cmd = " ".join(cmd)
    print(cmd)
    os.system(cmd)
    # end_time = time.time()
    df = pd.read_csv("./output_pmu.csv")
    os.system(f"mv ./output_pmu.csv ./wb_pmu{size}_results.csv")
    return df


def main():
    sizes = [x for x in range(2, 12)]

    mode = "remote"
    mem_node = 0 if mode == "local" else 1

    inject_latency = [
        "00000",
        "00002",
        "00008",
        "00015",
        "00050",
        "00100",
        "00200",
        "00300",
        "00400",
        "00500",
        "00700",
        "01000",
        "01300",
        "01700",
        "02500",
        "03500",
        "05000",
        "09000",
        "20000",
    ]
    writer = []
    for latency in inject_latency:
        f = open(f"wb_results_{mode}_{latency}.csv", "a")
        writer.append(csv.writer(f, delimiter=","))

        writer[-1].writerow(["size", "time", "bw"])
    # f = open(f"wb_results_{mode}.csv", "a")
    # writer.append(csv.writer(f, delimiter=","))

    # writer.writerow(["size", "time", "bw"])
    for i in range(25):
        for size in sizes:
            exec_time = run_command(size, mem_node)

            writer.writerow([size, exec_time])

    # for size in sizes:
    #     df = run_cxlmemsim_command(size,1)


if __name__ == "__main__":
    main()
