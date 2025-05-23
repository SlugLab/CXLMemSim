import os
import re
import glob
import matplotlib.pyplot as plt

def parse_summary(filepath):
    """从 summary_*.txt 提取 mean、stddev"""
    text = open(filepath).read()
    runtimes_str = re.search(r"Runtimes \(s\):\s*([\d\.,\s]+)", text).group(1)
    mean = float(re.search(r"Average \(s\):\s*([\d\.]+)", text).group(1))
    stddev = float(re.search(r"StdDev \(s\):\s*([\d\.]+)", text).group(1))
    return mean, stddev

def collect_group_data(artifact_base, group, pci_values):
    """
    对于给定的 group ("ld" or "st") 和 pci_values 列表，
    返回字典 { pci_int: ([fence_counts], [means], [stddevs]) }
    """
    data = {pci: ([], [], []) for pci in pci_values}
    for i in range(0, 9):
        fence = 2**i
        prog_dir = os.path.join(artifact_base, "microbench", f"{group}{fence}")
        for pci in pci_values:
            hexstr = f"0x{pci:04x}"
            summary = os.path.join(prog_dir, f"summary_{hexstr}.txt")
            if not os.path.isfile(summary):
                continue
            mean, std = parse_summary(summary)
            lst_f, lst_m, lst_s = data[pci]
            lst_f.append(fence)
            if group == "ld" and pci == 0xffff:
                lst_m.append(mean/10)
            else:
                lst_m.append(mean/30)
            lst_s.append(std)
    return data

def plot_two_subplots(results_ld, results_st, pci_values):
    fig, axes = plt.subplots(1, 2, figsize=(14, 6), sharey=True)

    for ax, (group, results) in zip(axes, [("ld", results_ld), ("st", results_st)]):
        for pci in pci_values:
            fences, means, stds = results[pci]
            # 按 fence 排序
            pairs = sorted(zip(fences, means, stds), key=lambda x: x[0])
            fences_s, means_s, stds_s = zip(*pairs)
            ax.errorbar(
                fences_s,
                means_s,
                yerr=stds_s,
                fmt="o-",
                capsize=4,
                label=f"setpci=0x{pci:04x}"
            )
        ax.set_xticks([2**i for i in range(0,9)])
        ax.get_xaxis().set_major_formatter(plt.ScalarFormatter())
        ax.set_xlabel("Fence Count")
        ax.set_title(group)
        ax.grid(True)
        ax.legend()

    axes[0].set_ylabel("Latency (us)")
    plt.suptitle("ld vs st: 0x0000 and 0xffff")
    plt.tight_layout(rect=[0, 0, 1, 0.95])
    plt.savefig("ld_st_result.pdf")

if __name__ == "__main__":
    # 请根据实际路径调整 artifact_base
    artifact_base = "../artifact"
    pci_values = [0x0000, 0xffff]

    results_ld = collect_group_data(artifact_base, "ld", pci_values)
    results_st = collect_group_data(artifact_base, "st", pci_values)

    plot_two_subplots(results_ld, results_st, pci_values)
