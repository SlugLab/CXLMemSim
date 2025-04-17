import matplotlib.pyplot as plt
import pandas as pd
import numpy as np
import seaborn as sns

# Construct the DataFrame from the image data
data = {
    "Caching": ["none", "fifo", "frequency"],
    "VectorDB": [17.37, 16.2, 18.14],
    "mcf": [491.28, 484.56, 499.51],
    "wrf": [1451.42, 1421.43, 1489.14],
    "bc": [1642.95, 1567.66, 1655.24],
    "bfs": [829.53, 852.14, 831.34],
    "sssp": [1324.64, 1345.15, 1356.59],
    "tc": [70038.14, 70435.15, 70050.31],
    "cc": [1452.55, 1432.57, 1469.14],
    "pr": [2011.24, 2125.63, 2013.42],
    "llama": [1455.345, 1435.15, 1489.45]
}

df = pd.DataFrame(data)
df.set_index("Caching", inplace=True)

# Transpose for easier plotting
df_t = df.transpose()
df_normalized = df.divide(df.loc["none"])
# Transpose the normalized DataFrame to switch axes
df_normalized_t = df_normalized.transpose()

# Create a grouped bar chart where x-axis is benchmark
x = np.arange(len(df_normalized_t.index))  # benchmarks
bar_width = 0.25

# Plot each caching policy
plt.figure(figsize=(14, 3))
for i, policy in enumerate(df_normalized_t.columns):
    plt.bar(x + i * bar_width, df_normalized_t[policy], width=bar_width, label=policy)

plt.xticks(x + bar_width, df_normalized_t.index)
plt.xlabel("Benchmark")
plt.ylabel("Normalized Latency (to 'none')")
plt.title("Normalized Latency per Benchmark Across Caching Policies")
plt.ylim(0.9, 1.1)
plt.legend(title="Caching Policy")
plt.tight_layout()
plt.grid(axis='y')
plt.savefig("policy1.pdf")

# Re-import necessary packages after environment reset
import matplotlib.pyplot as plt
import pandas as pd
import numpy as np

# Recreate the paging policy latency data
paging_data = {
    "Paging": ["none", "hugepage", "pagetableaware"],
    "VectorDB": [17.37, 16.53, 15.53],
    "mcf": [491.28, 502.42, 467.31],
    "wrf": [1451.42, 1459.41, 1345.15],
    "bc": [1642.95, 1598.41, 1523.45],
    "bfs": [829.53, 810.1, 765.51],
    "sssp": [1324.64, 1205.35, 1264.53],
    "tc": [70038.14, 69710.15, 69755.43],
    "cc": [1452.55, 1400.41, 1401.41],
    "pr": [2011.24, 1941.45, 1931.67],
    "llama": [1455.345, 1402.41, 1405.53]
}

df_paging = pd.DataFrame(paging_data)
df_paging.set_index("Paging", inplace=True)

# Normalize to 'none'
df_paging_normalized = df_paging.divide(df_paging.loc["none"])
df_paging_normalized_t = df_paging_normalized.transpose()

# Plotting
plt.figure(figsize=(14, 3))
bar_width = 0.25
x = np.arange(len(df_paging_normalized_t.index))  # benchmarks

for i, policy in enumerate(df_paging_normalized_t.columns):
    plt.bar(x + i * bar_width, df_paging_normalized_t[policy], width=bar_width, label=policy)

plt.xticks(x + bar_width, df_paging_normalized_t.index)
plt.xlabel("Benchmark")
plt.ylabel("Normalized Latency (to 'none')")
plt.title("Normalized Latency per Benchmark Across Paging Policies")
plt.legend(title="Paging Policy")
plt.tight_layout()
plt.ylim(0.85, 1.05)
plt.grid(axis='y')
plt.savefig("policy2.pdf")

# Re-import after environment reset
import matplotlib.pyplot as plt
import pandas as pd
import numpy as np

# Define the migration policy latency data
migration_data = {
    "Migration": ["none", "frequency", "locality", "lifetime"],
    "VectorDB": [17.37, 16.15, 13.14, 15.15],
    "mcf": [491.28, 401.51, 405.14, 389.15],
    "wrf": [1451.42, 1415.10, 1413.23, 1359.51],
    "bc": [1642.95, 1818.36, 1534.45, 1443.34],
    "bfs": [829.53, 986.51, 845.15, 865.43],
    "sssp": [1324.64, 1451.62, 1325.15, 1345.15],
    "tc": [70038.14, 71044.51, 70046.41, 70053.34],
    "cc": [1452.55, 1569.31, 1421.51, 1487.41],
    "pr": [2011.24, 2105.14, 2000.34, 2056.15],
    "llama": [1455.345, 1561.15, 1245.61, 1345.16]
}

df_migration = pd.DataFrame(migration_data)
df_migration.set_index("Migration", inplace=True)

# Normalize to 'none'
df_migration_normalized = df_migration.divide(df_migration.loc["none"])
df_migration_normalized_t = df_migration_normalized.transpose()

# Plotting
plt.figure(figsize=(14, 3))
bar_width = 0.2
x = np.arange(len(df_migration_normalized_t.index))  # benchmarks

for i, policy in enumerate(df_migration_normalized_t.columns):
    plt.bar(x + i * bar_width, df_migration_normalized_t[policy], width=bar_width, label=policy)

plt.xticks(x + bar_width * 1.5, df_migration_normalized_t.index)
plt.xlabel("Benchmark")
plt.ylabel("Normalized Latency (to 'none')")
plt.title("Normalized Latency per Benchmark Across Migration Policies")
plt.legend(title="Migration Policy")
plt.ylim(0.7, 1.1)
plt.tight_layout()
plt.grid(axis='y')
plt.savefig("policy3.pdf")
