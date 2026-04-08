import matplotlib.pyplot as plt
import numpy as np

ranks = [1, 2, 4, 8, 16]

native_cxl = [2.3, 1.3, 0.7, 0.4, 0.2]
cxlmemsim_tcp = [1.083, 0.585, 0.330, 0.199, 0.123]
cxlmemsim_rdma = [0.996, 0.524, 0.283, 0.160, 0.09]

ideal = [cxlmemsim_tcp[0] / r for r in ranks]

fig, ax = plt.subplots(figsize=(8, 6))

ax.plot(ranks, native_cxl, 's-', color='#4CAF50', markersize=9, linewidth=2, label='Native CXL (VTune)', markerfacecolor='#4CAF50')
ax.plot(ranks, cxlmemsim_tcp, 'o-', color='#2196F3', markersize=8, linewidth=2, label='CXLMemSim + TCP')
ax.plot(ranks, cxlmemsim_rdma, 's-', color='#E91E63', markersize=8, linewidth=2, label='CXLMemSim + RDMA (100G)')
ax.plot(ranks, ideal, '--', color='grey', linewidth=1.5, label='Ideal')

# Annotate TCP points
for x, y in zip(ranks, cxlmemsim_tcp):
    ax.annotate(f'{y:.3f}', (x, y), textcoords='offset points', xytext=(-20, -15), fontsize=9, color='#2196F3')

# Annotate RDMA points
for x, y in zip(ranks, cxlmemsim_rdma):
    ax.annotate(f'{y:.3f}' if y >= 0.1 else f'{y:.2f}', (x, y), textcoords='offset points', xytext=(5, 8), fontsize=9, color='#E91E63')

ax.set_xscale('log', base=2)
ax.set_xticks(ranks)
ax.set_xticklabels(ranks)
ax.set_xlabel('MPI Ranks', fontsize=12)
ax.set_ylabel('Wall Time (s)', fontsize=12)
ax.set_title('Strong Scaling: Execution Time', fontsize=14)
ax.legend(loc='upper right', fontsize=10)
ax.grid(False)

plt.tight_layout()
plt.savefig('./strong_scaling.png', dpi=200)
plt.savefig('./strong_scaling.pdf')
print("Done")