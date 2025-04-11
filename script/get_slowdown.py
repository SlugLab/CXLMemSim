import matplotlib.pyplot as plt
import numpy as np

# Data from the table
topologies = ["Single Pool",
              "Two-level Hierarchy",
              "Shared Switch",
              "Complex Multi-pool"]
gem5_times = [34.56, 35.28, 35.42, 36.98]
xsim_times = [ 2.17,  2.35,  2.40,  2.41]
speedups   = [15.92, 15.01, 14.76, 15.34]

# Set up the figure
x = np.arange(len(topologies))
width = 0.35

fig, ax1 = plt.subplots(figsize=(7,4))

# Plot Gem5 and XSim bars on ax1
rects_gem5 = ax1.bar(x - width/2, gem5_times, width, label='Gem5 (s)', color='tab:blue')
rects_xsim = ax1.bar(x + width/2, xsim_times, width, label='XSim (s)', color='tab:orange')

# Set labels and ticks for the first axis
ax1.set_ylabel('Runtime (seconds)')
ax1.set_xticks(x)
ax1.set_xticklabels(topologies, rotation=30, ha='right')
ax1.legend(loc='upper left')

# Create a secondary axis for speedup
ax2 = ax1.twinx()
ax2.plot(x, speedups, marker='o', color='tab:green', label='Speedup')
ax2.set_ylabel('Speedup (×)')

# Add a legend entry for the speedup line
lines, labels = ax2.get_legend_handles_labels()
ax1.legend(lines + [rects_gem5, rects_xsim],
           labels + ['Gem5 (s)', 'XSim (s)'],
           loc='upper center')

fig.tight_layout()
plt.savefig('slowdown.pdf', dpi=300)