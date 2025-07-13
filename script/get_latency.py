import matplotlib.pyplot as plt
import numpy as np

# Data extracted from the table
benchmarks = [
    "VectorDB", "mcf", "wrf", "bc", "bfs", "sssp",
    "tc", "cc", "pr", "MLC", "Llama deepseek"
]

# CXLMemSim latency values
cxlsim = [
    17.37, 491.283525591, 1451.42, 1642.95, 829.53, 1324.64,
    70038.14, 1452.55, 2011.24, 501.4, 1455.345
]

cxlsim_std = [
    10.0, 100.0, 300.0, 340.0, 200.0, 100.0,
    10000.0, 100.0, 100.0, 100.0, 100.0
]

# Real latency values
real = [
     19.628, 573.134, 1138.34, 1773.22, 850.62, 1510.90,
     77340.14, 1333.78, 2347.19, 527.4, 1558.534
]
cxlsim_std = [x / y for x, y in zip(cxlsim_std, real)]
# Normalize: CXLMemSim / Real
normalized = [c / r for c, r in zip(cxlsim, real)]

x = np.arange(len(benchmarks))
width = 0.6

# Plotting normalized bar chart
fig, ax = plt.subplots(figsize=(14, 6))
bars = ax.bar(x, normalized, width, color='skyblue', yerr=cxlsim_std, capsize=5)

# Annotate values above bars
for i, val in enumerate(normalized):
    ax.text(i, val + 0.02, f"{val:.2f}", ha='center', va='bottom', fontsize=9)

# Labels and titles
ax.set_xlabel('Benchmark')
ax.set_ylabel('Normalized Latency (CXLMemSim / Real)')
ax.set_title('Normalized Latency Comparison (CXLMemSim vs Real)')
ax.set_xticks(x)
ax.set_xticklabels(benchmarks, rotation=45, ha="right")
ax.axhline(1.0, color='red', linestyle='--', linewidth=1, label='Parity Line')

ax.legend()
plt.tight_layout()

plt.savefig("latency.pdf")