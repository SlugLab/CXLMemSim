import pandas as pd
import matplotlib.pyplot as plt

# Define the data manually extracted from the image
data = {
    "Test": [
        "ld_cxl", "ld_serial_cxl",
        "st_cxl", "st_serial_cxl"
    ],
    "gem5_time": [1251.51, 1468.79,  1494.13,  1496.15 ],
    "gem5_non_cxl_time": [105.09, 191.78, 103.04, 101.90],
    "cxlmemsim_time": [10.531,  11.452,  9.415,  9.869]
}

# Create a DataFrame
df = pd.DataFrame(data)

# Plotting
plt.figure(figsize=(12, 6))
bar_width = 0.25
x = range(len(df))

# Bar positions
r1 = [i - bar_width for i in x]
r2 = x
r3 = [i + bar_width for i in x]

plt.bar(r1, df['gem5_time'], width=bar_width, label='gem5 time (CXL)', color='tab:blue')
plt.bar(r2, df['gem5_non_cxl_time'], width=bar_width, label='gem5 time (non-CXL)', color='tab:orange')
plt.bar(r3, df['cxlmemsim_time'], width=bar_width, label='CXLMemSim time', color='tab:green')

plt.xlabel('Test Case')
plt.ylabel('Time (s)')
plt.title('Comparison of gem5 and CXLMemSim Time')
plt.xticks(ticks=x, labels=df['Test'], rotation=45)
plt.legend()
plt.tight_layout()
plt.savefig('gem5_slowdown.pdf')