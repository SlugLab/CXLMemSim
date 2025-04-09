#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
PEBS周期对比图生成脚本
将不同PEBS周期的时间戳-索引关系绘制在同一张图上
"""

import re
import subprocess
import os
import pandas as pd
import matplotlib.pyplot as plt
import numpy as np
import time
import argparse
from pathlib import Path
import glob

def run_test(target, pebs_period, output_dir, run_id=None):
    """
    使用指定的PEBS周期运行目标程序

    参数:
        target (str): 目标程序路径
        pebs_period (int): PEBS采样周期
        output_dir (str): 输出目录
        run_id (str, optional): 运行ID，用于区分不同的运行

    返回:
        str或None: 程序输出或None（如果失败）
    """
    timestamp = time.strftime("%Y%m%d-%H%M%S")
    run_id = run_id or timestamp

    # 确保输出目录存在
    os.makedirs(output_dir, exist_ok=True)

    # 创建输出文件路径
    output_file = os.path.join(output_dir, f"{os.path.basename(target)}_pebs{pebs_period}_{run_id}.log")

    # 构建命令
    cmd = [
        "./CXLMemSim",
        "-t", target,
        "-p", str(pebs_period)
    ]

    # 运行命令并捕获输出
    try:
        print(f"执行命令: {' '.join(cmd)}")
        with open(output_file, 'w') as f:
            result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    text=True, check=True, timeout=300)  # 5分钟超时
            f.write(result.stdout)
        return result.stdout, output_file

    except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as e:
        print(f"命令执行失败: {e}")
        with open(output_file, 'w') as f:
            f.write(f"错误: {e}\n")
            if hasattr(e, 'stdout') and e.stdout:
                f.write(e.stdout)
        return None, output_file

def parse_timestamps(content):
    """
    从程序输出文本中解析时间戳和索引

    参数:
        content (str): 程序输出内容

    返回:
        pandas.DataFrame: 包含时间戳和索引的DataFrame，如果没有有效数据则为None
    """
    timestamps = []
    indices = []

    if not content:
        return None

    for line in content.split('\n'):
        # 使用正则表达式匹配时间戳和索引
        match = re.match(r'^\s*(\d+)\s+(\d+)\s*$', line)
        if match:
            timestamp = int(match.group(1))
            index = int(match.group(2))
            timestamps.append(timestamp)
            indices.append(index)

    if not timestamps:  # 如果没有找到有效数据
        return None

    # 创建DataFrame
    df = pd.DataFrame({'timestamp': timestamps, 'index': indices})
    return df

def find_log_files(output_dir, pattern="*_pebs*.log"):
    """
    查找给定目录中的所有日志文件

    参数:
        output_dir (str): 输出目录
        pattern (str): 文件匹配模式

    返回:
        list: 匹配的文件路径列表
    """
    path_pattern = os.path.join(output_dir, pattern)
    return glob.glob(path_pattern)

def extract_pebs_period(filename):
    """
    从文件名中提取PEBS周期值

    参数:
        filename (str): 文件名或路径

    返回:
        int: PEBS周期值，如果无法提取则返回None
    """
    basename = os.path.basename(filename)
    match = re.search(r'_pebs(\d+)_', basename)
    if match:
        return int(match.group(1))
    return None

def normalize_timestamps(df):
    """
    归一化时间戳，使第一个时间戳为0

    参数:
        df (pandas.DataFrame): 包含时间戳的DataFrame

    返回:
        pandas.DataFrame: 包含归一化时间戳的DataFrame
    """
    if df is None or df.empty:
        return None

    # 复制DataFrame以避免修改原始数据
    normalized_df = df.copy()

    # 获取第一个时间戳作为基准
    base_timestamp = df['timestamp'].iloc[0]

    # 归一化时间戳
    normalized_df['normalized_timestamp'] = (df['timestamp'] - base_timestamp) / 1000000000  # 转换为秒

    return normalized_df

def plot_combined_graph(dataframes_dict, output_file='combined_pebs_analysis.pdf'):
    """
    绘制不同PEBS周期的数据在同一张图上

    参数:
        dataframes_dict (dict): 键为PEBS周期，值为DataFrame的字典
        output_file (str): 输出文件路径
    """
    if not dataframes_dict:
        print("没有数据可以绘制图表")
        return

    plt.figure(figsize=(12, 8))

    # 设置颜色循环
    colors = ['blue', 'red', 'green', 'purple', 'orange', 'brown', 'pink', 'gray', 'olive', 'cyan']
    markers = ['o', 's', '^', 'd', 'v', '<', '>', 'p', '*', 'h']

    # 绘制每个PEBS周期的数据
    for i, (pebs_period, df) in enumerate(sorted(dataframes_dict.items())):
        color_idx = i % len(colors)
        marker_idx = i % len(markers)

        # 为了使图表不太拥挤，根据数据点数量调整marker的大小和频率
        if len(df) > 1000:
            # 数据点较多时，每隔100个点标记一个，并减小marker尺寸
            plt.plot(df['normalized_timestamp'], df['index'], '-', color=colors[color_idx],
                     label=f'PEBS={pebs_period}', alpha=0.7)
            # 添加少量的标记点
            marker_indices = np.linspace(0, len(df)-1, 20, dtype=int)
            plt.plot(df['normalized_timestamp'].iloc[marker_indices],
                     df['index'].iloc[marker_indices],
                     markers[marker_idx], color=colors[color_idx], markersize=4, alpha=0.8)
        else:
            # 数据点较少时，标记所有点
            plt.plot(df['normalized_timestamp'], df['index'], '-' + markers[marker_idx],
                     color=colors[color_idx], label=f'PEBS={pebs_period}',
                     markersize=5, alpha=0.8, markevery=max(1, len(df)//50))

    plt.xlabel('Time (seconds)', fontsize=12)
    plt.ylabel('Index', fontsize=12)
    plt.title('Timestamp vs Index for Different PEBS Periods', fontsize=14)
    plt.grid(True, alpha=0.3)
    plt.legend(loc='best')

    # 保存图表
    plt.xlim(0, 3.5)
    plt.ylim(0, 150000)
    plt.tight_layout()
    plt.savefig(output_file, dpi=300)
    print(f"组合图表已保存到: {output_file}")


def main():
    parser = argparse.ArgumentParser(description='为不同PEBS周期创建时间戳vs索引的组合图')
    parser.add_argument('--targets', nargs='+', default=['./microbench/ld1'],
                        help='要测试的目标程序列表')
    parser.add_argument('--pebs-periods', nargs='+', type=int, default=[1, 10, 100, 1000, 10000],
                        help='要测试的PEBS周期列表')
    parser.add_argument('--output-dir', default='./results',
                        help='结果输出目录')
    parser.add_argument('--output-file', default='combined_pebs_analysis.pdf',
                        help='输出图表文件路径')
    parser.add_argument('--runs', type=int, default=1,
                        help='每个组合运行的次数')
    parser.add_argument('--only-plot', action='store_true',
                        help='只从现有日志文件中绘制图表，不运行新测试')
    parser.add_argument('--log-files', nargs='+',
                        help='指定要处理的日志文件，而不是自动查找')
    args = parser.parse_args()

    # 确保输出目录存在
    os.makedirs(args.output_dir, exist_ok=True)

    # 将存储不同PEBS周期的数据
    pebs_data = {}

    # 如果只绘制图表，不运行测试
    if args.only_plot:
        # 如果指定了日志文件，使用这些文件
        if args.log_files:
            log_files = args.log_files
        else:
            # 否则查找目录中的所有日志文件
            log_files = find_log_files(args.output_dir)

        if not log_files:
            print(f"在 {args.output_dir} 中没有找到匹配的日志文件")
            return

        print(f"找到以下日志文件：")
        for log_file in log_files:
            print(f"  {log_file}")

        # 处理每个日志文件
        for log_file in log_files:
            pebs_period = extract_pebs_period(log_file)
            if pebs_period is None:
                print(f"无法从文件名 {log_file} 中提取PEBS周期，跳过")
                continue

            try:
                with open(log_file, 'r') as f:
                    content = f.read()

                df = parse_timestamps(content)
                if df is not None and not df.empty:
                    print(f"从文件 {log_file} 中提取了 {len(df)} 条时间戳记录 (PEBS={pebs_period})")
                    # 归一化时间戳
                    normalized_df = normalize_timestamps(df)
                    pebs_data[pebs_period] = normalized_df
                else:
                    print(f"无法从文件 {log_file} 中提取有效的时间戳数据")
            except Exception as e:
                print(f"处理文件 {log_file} 时出错: {e}")
    else:
        # 运行测试
        for target in args.targets:
            for pebs_period in args.pebs_periods:
                for run in range(args.runs):
                    print(f"\n运行目标: {target}, PEBS周期: {pebs_period}, 运行 #{run+1}/{args.runs}")
                    run_id = f"run{run+1}"

                    output, log_file = run_test(target, pebs_period, args.output_dir, run_id)
                    if output:
                        df = parse_timestamps(output)
                        if df is not None and not df.empty:
                            print(f"成功提取了 {len(df)} 条时间戳记录")
                            # 归一化时间戳
                            normalized_df = normalize_timestamps(df)
                            pebs_data[pebs_period] = normalized_df
                            # 找到有效数据后继续下一个PEBS周期
                            break
                        else:
                            print(f"无法从输出中提取有效的时间戳数据")

    # 绘制组合图表
    if pebs_data:
        output_file = os.path.join(args.output_dir, args.output_file)
        plot_combined_graph(pebs_data, output_file)
    else:
        print("没有找到任何有效的数据来绘制图表")

if __name__ == "__main__":
    main()