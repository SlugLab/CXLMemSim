import os
import glob
import matplotlib.pyplot as plt

def read_data(filepath):
    """
    读取文件中每行的数值，返回浮点数列表。
    """
    data = []
    with open(filepath, 'r') as f:
        for line in f:
            try:
                data.append(float(line.strip()))
            except ValueError:
                continue
    return data

def compute_average_diff(data):
    """
    计算相邻数据的差值平均值。
    如果数据点不足两个则返回 None。
    """
    if len(data) < 2:
        return None
    diffs = [data[i+1] - data[i] for i in range(len(data)-1)]
    return sum(diffs) / len(diffs)

def process_directory(directory):
    """
    对于给定目录（如 "microbench/st" 或 "microbench/ld"），
    遍历后缀为 2^i（i=0,1,...,8，即 1,2,4,...,256）的子目录，
    在每个子目录下查找 remote_setpci_*.txt 文件，
    并计算每个文件（即每个 setpci 参数）的差值平均值。
    
    返回一个字典：
      key: setpci 参数（如 "0x0001"）
      value: 包含两个列表的字典 { "fence_counts": [...], "avg_diff": [...] }
             其中 fence_counts 存放对应的 2^i 值，
             avg_diff 存放对应计算出的平均差值。
    """
    data_dict = {}
    # 遍历 i=0 到 8，对应 fence count 为 2**i
    for i in range(0, 9):
        current_fence = 2**i
        # 假设子目录命名为如 "st2", "st4", ...（传入的 directory 是 "microbench/st" 或 "microbench/ld"）
        current_dir = f"{directory}{current_fence}"
        pattern = os.path.join(current_dir, "remote_setpci_*.txt")
        files = glob.glob(pattern)
        for file in files:
            data = read_data(file)
            avg_diff = compute_average_diff(data)
            if avg_diff is None:
                continue
            # 从文件名中提取 setpci 参数，如 "remote_setpci_0x0001.txt" 提取 "0x0001"
            filename = os.path.basename(file)
            setpci = int(filename.split('_')[-1].split('.')[0], 0)
            if setpci not in data_dict:
                data_dict[setpci] = {"fence_counts": [], "avg_diff": []}
            data_dict[setpci]["fence_counts"].append(current_fence)
            data_dict[setpci]["avg_diff"].append(avg_diff)
    return data_dict

def main():
    # 基目录设为 "microbench"，下面有 st 和 ld 两个分组
    base_dir = "microbench"
    groups = ["st", "ld"]
    
    # 创建两个子图，分别对应 st 和 ld
    fig, axes = plt.subplots(1, 2, figsize=(14, 8))
    
    for ax, group in zip(axes, groups):
        # 例如 group_dir 为 "microbench/st"
        group_dir = os.path.join(base_dir, group)
        print("Processing:", group_dir)
        group_data = process_directory(group_dir)
        if not group_data:
            ax.text(0.5, 0.5, f"没有找到 {group} 数据", ha="center", va="center")
            ax.set_title(group)
            continue
        
        for setpci, values in group_data.items():
            fence_counts = values["fence_counts"]
            avg_diffs = values["avg_diff"]
            # 按 fence_counts 排序（确保点顺序正确）
            sorted_pairs = sorted(zip(fence_counts, avg_diffs), key=lambda x: x[0])
            fence_counts_sorted, avg_diffs_sorted = zip(*sorted_pairs)
            # 对调后：x 轴为 fence count，y 轴为平均延迟差值
            ax.plot(fence_counts_sorted, avg_diffs_sorted, linewidth=0.8, label=setpci)
        
        ax.set_xlabel("Fence Count")
        ax.set_ylabel("Average delay difference (ns)")
        ax.set_title(group)
        ax.legend(title="setpci", loc="upper right")
        ax.grid(True)
    
    plt.tight_layout()
    plt.savefig("ld_st_result.pdf")
    plt.show()

if __name__ == "__main__":
    main()
