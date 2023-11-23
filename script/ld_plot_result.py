#!/usr/bin/env python

import argparse
import subprocess
import time
from math import sqrt
import matplotlib.pyplot as plt
import pandas as pd

workloads = ["mlc", "ld", "st", "nt-ld", "nt-st", "ptr-chasing"]
pmus = [
    "mon0_tatal_stall_0_0",
    "mon0_all_dram_rds_0_0",
    "mon0_l2stall_0_0",
    "mon0_snoop_fw_wb_0_0",
    "mon0_llcl_hits_0_0",
    "mon0_llcl_miss_0_0",
    "mon0_null_0_0",
    "mon0_null_0_0",
]


def get_mean_and_ebars(df, groups, select):
    """returns df with error bars. gropus includes columns to groupby"""
    agg = df.groupby(groups)[select].agg(["mean", "count", "std"])
    error = []
    for i in agg.index:
        mean, count, std = agg.loc[i]
        error.append(1.95 * std / sqrt(count))

    agg["error"] = error

    return agg[["mean"]], agg[["error"]]


def print_pmu_csv():
    sizes = [2**x for x in range(0, 9)]
    for c in pmus:
        for i in sizes:
            df = pd.read_csv(f"ld_pmu{i}_results.csv")
            col = df[df[c]<1844674407][c]
            print(col)
            if col[11] == "0":
                print(col)
                continue
            # Plotting the data
            plt.plot(col, marker="o", linestyle="-", label=i)

        # Adding title and labels
        plt.title("PMU Plot for ld")
        plt.xlabel("PMU gathered per epoch")
        plt.ylabel(f"{c} Values")
        plt.legend()
        plt.savefig(f"ld_results_pmu_{c}.png")


def main():
    parser = argparse.ArgumentParser(description="plot results.")
    # parser.add_argument(
    #     "-f", "--file_name", nargs="?", default="ld_results.csv",
    #     help="csv containing results.")

    # args = parser.parse_args()

    # df = pd.read_csv(args.file_name)
    # means, error = get_mean_and_ebars(df, ["size"], "time")

    # fig,ax = plt.subplots()

    # ax.errorbar(means.index, means["mean"],yerr=error["error"], capsize=4)
    # #means.plot(ax=ax, yerr=error, grid=True, rot=0, capsize=4)
    # ax.set_xlabel("Size")
    # ax.set_ylabel("Execution Time (seconds)")
    # print(error)

    # fig.savefig("ld_results.png")
    print_pmu_csv()


if __name__ == "__main__":
    main()
