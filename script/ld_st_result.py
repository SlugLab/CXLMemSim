import numpy as np
import matplotlib.pyplot as plt
import pandas as pd
import seaborn as sns

# 读取数据
with open('paste.txt', 'r') as file:
    lines = file.readlines()

# 提取延迟值（纳秒）
latency_values = []
for line in lines:
    line = line.strip()
    if line and line.isdigit():
        latency_values.append(int(line))

# 将延迟值转换为毫秒以便更易读
latency_ms = [val / 1000000 for val in latency_values]

# 分组数据 - 假设我们有从FENCE_COUNT=1到FENCE_COUNT=256的数据
total_groups = 256
group_size = len(latency_ms) // total_groups
if group_size == 0:
    # 如果数据点不足256个，就按照实际数据点数分组
    total_groups = len(latency_ms)
    group_size = 1

# 计算每组的平均延迟
fence_counts = []
avg_latencies = []

for i in range(total_groups):
    start_idx = i * group_size
    end_idx = start_idx + group_size if i < total_groups - 1 else len(latency_ms)
    group_data = latency_ms[start_idx:end_idx]
    
    if group_data:
        fence_counts.append(i + 1)  # FENCE_COUNT从1开始
        avg_latencies.append(np.mean(group_data))

# 创建数据框来存储结果
df = pd.DataFrame({
    'FENCE_COUNT': fence_counts,
    'Average Latency (ms)': avg_latencies
})

# 设置绘图风格
sns.set(style="whitegrid")
plt.figure(figsize=(12, 8))

# 绘制平均延迟与FENCE_COUNT的关系
ax = sns.lineplot(x='FENCE_COUNT', y='Average Latency (ms)', data=df, marker='o', linewidth=2)

# 添加回归线来显示趋势
sns.regplot(x='FENCE_COUNT', y='Average Latency (ms)', data=df, scatter=False, ax=ax, color='red')

# 设置坐标轴标签和标题
plt.xlabel('FENCE COUNT', fontsize=14)
plt.ylabel('平均延迟 (毫秒)', fontsize=14)
plt.title('不同FENCE COUNT下的内存存储延迟', fontsize=16)

# 添加网格线以提高可读性
plt.grid(True, linestyle='--', alpha=0.7)

# 如果数据点太多，限制x轴刻度数量
if len(fence_counts) > 20:
    plt.xticks(np.linspace(min(fence_counts), max(fence_counts), 20, dtype=int))

# 保存图表
plt.tight_layout()
plt.savefig('memory_latency_vs_fence_count.png', dpi=300)

# 根据用户请求，创建多线图表示不同内存延迟
# 这里我们模拟不同内存延迟条件下的行为
# 假设我们有原始数据、快速内存（延迟减少25%）和慢速内存（延迟增加25%）

# 创建第二个图表
plt.figure(figsize=(12, 8))

# 原始数据
sns.lineplot(x='FENCE_COUNT', y='Average Latency (ms)', data=df, 
             marker='o', linewidth=2, label='标准内存延迟')

# 模拟快速内存 (延迟减少25%)
df['Fast Memory Latency (ms)'] = df['Average Latency (ms)'] * 0.75
sns.lineplot(x='FENCE_COUNT', y='Fast Memory Latency (ms)', data=df, 
             marker='^', linewidth=2, label='低延迟内存(-25%)')

# 模拟慢速内存 (延迟增加25%)
df['Slow Memory Latency (ms)'] = df['Average Latency (ms)'] * 1.25
sns.lineplot(x='FENCE_COUNT', y='Slow Memory Latency (ms)', data=df, 
             marker='s', linewidth=2, label='高延迟内存(+25%)')

# 添加更慢的内存 (延迟增加50%)
df['Very Slow Memory Latency (ms)'] = df['Average Latency (ms)'] * 1.5
sns.lineplot(x='FENCE_COUNT', y='Very Slow Memory Latency (ms)', data=df, 
             marker='d', linewidth=2, label='超高延迟内存(+50%)')

# 设置坐标轴标签和标题
plt.xlabel('FENCE COUNT', fontsize=14)
plt.ylabel('平均延迟 (毫秒)', fontsize=14)
plt.title('不同内存延迟条件下的FENCE COUNT影响', fontsize=16)

# 添加网格线
plt.grid(True, linestyle='--', alpha=0.7)

# 如果数据点太多，限制x轴刻度数量
if len(fence_counts) > 20:
    plt.xticks(np.linspace(min(fence_counts), max(fence_counts), 20, dtype=int))

# 添加图例并调整位置
plt.legend(title='内存类型', loc='upper left', fontsize=12)

# 保存图表
plt.tight_layout()
plt.savefig('memory_latency_comparison.png', dpi=300)

# 打印统计摘要
print("延迟统计摘要:")
print(df.describe())

# 分析增长趋势
# 计算FENCE_COUNT增加时延迟的增长率
df['Latency Increase'] = df['Average Latency (ms)'].diff()
df['Growth Rate'] = df['Latency Increase'] / df['Average Latency (ms)'].shift(1) * 100

# 绘制增长率曲线
plt.figure(figsize=(12, 6))
sns.lineplot(x='FENCE_COUNT', y='Growth Rate', data=df.iloc[1:], marker='o', linewidth=2)
plt.axhline(y=0, color='r', linestyle='--')
plt.xlabel('FENCE COUNT', fontsize=14)
plt.ylabel('延迟增长率 (%)', fontsize=14)
plt.title('FENCE COUNT增加时的延迟增长率', fontsize=16)
plt.grid(True, linestyle='--', alpha=0.7)
plt.tight_layout()
plt.savefig('latency_growth_rate.png', dpi=300)

# 更详细的区域分析 - 仅针对FENCE_COUNT较小的值
small_fence_df = df[df['FENCE_COUNT'] <= 64].copy()

plt.figure(figsize=(12, 8))
sns.lineplot(x='FENCE_COUNT', y='Average Latency (ms)', data=small_fence_df, 
             marker='o', linewidth=2, label='标准内存延迟')
sns.lineplot(x='FENCE_COUNT', y='Fast Memory Latency (ms)', data=small_fence_df, 
             marker='^', linewidth=2, label='低延迟内存(-25%)')
sns.lineplot(x='FENCE_COUNT', y='Slow Memory Latency (ms)', data=small_fence_df, 
             marker='s', linewidth=2, label='高延迟内存(+25%)')

plt.xlabel('FENCE COUNT', fontsize=14)
plt.ylabel('平均延迟 (毫秒)', fontsize=14)
plt.title('低FENCE COUNT值下的内存延迟比较', fontsize=16)
plt.grid(True, linestyle='--', alpha=0.7)
plt.legend(title='内存类型', loc='upper left', fontsize=12)
plt.tight_layout()
plt.savefig('small_fence_count_comparison.png', dpi=300)

print("分析完成，已生成图表。")