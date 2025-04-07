import matplotlib.pyplot as plt
import numpy as np

# 1) Data from the first (upper) table
categories = ["Load parallel", "Load serial", "Store parallel", "Store serial"]
cxl_data = [0.016399, 0.001579, 0.026713, 0.004425]
noncxl_data = [0.084274, 0.004895, 0.083735, 0.005381]

# 2) Data from the second (lower) table
#    We'll keep the same categories (rows), but different values
gem5_diff = [24454, 8177, 24117, 4395]
robsim_diff = [22309, 7283, 22249, 3956]

# Convert categories to an array of indices for plotting
x = np.arange(len(categories))  # [0, 1, 2, 3]
bar_width = 0.35

fig, axes = plt.subplots(1, 2, figsize=(12, 6))  # 1 row, 2 columns

##################################
# Left plot: CXL vs. NonCXL times
##################################
ax1 = axes[0]

# Plot bars side by side
rects1 = ax1.bar(x - bar_width/2, cxl_data, width=bar_width, label="CXL", color="#1f77b4")
rects2 = ax1.bar(x + bar_width/2, noncxl_data, width=bar_width, label="NonCXL", color="#ff7f0e")

ax1.set_xticks(x)
ax1.set_xticklabels(categories, rotation=15, ha='right')
ax1.set_ylabel("IPC (Instructions per Cycle)")
ax1.set_title("CXL vs. NonCXL")
ax1.legend()
ax1.grid(True, axis='y', linestyle='--', alpha=0.5)

########################################
# Right plot: Gem5 CXL diff vs. RoBSim diff
########################################
ax2 = axes[1]

rects3 = ax2.bar(x - bar_width/2, gem5_diff, width=bar_width, label="Gem5 CXL diff", color="#2ca02c")
rects4 = ax2.bar(x + bar_width/2, robsim_diff, width=bar_width, label="RoBSim diff", color="#d62728")

ax2.set_xticks(x)
ax2.set_xticklabels(categories, rotation=15, ha='right')
ax2.set_ylabel("Diff (Stall Cycles)")
ax2.set_title("Gem5 vs. RoBSim")
ax2.legend()
ax2.grid(True, axis='y', linestyle='--', alpha=0.5)

plt.tight_layout()
plt.savefig("ipc_rob_result.pdf")