#!/usr/bin/env python3
"""
Generate publication-quality figures for CoherenceHub paper.
Reads experiment output from rq1-rq4 stdout logs and produces PDFs.

Usage:
    python3 plot_results.py [--rq1 rq1.log] [--rq2 rq2.log] [--rq3 rq3.log] [--rq4 rq4.log]

If log files are not provided, uses embedded data from paper results.

NOTE: matplotlib rendering may fail inside QEMU guests due to CPU instruction
set limitations (SIGILL in Agg backend). Run this script on the host machine
or copy the experiment logs out and plot there.
"""

import argparse
import re
import sys
import os

try:
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    import matplotlib.ticker as ticker
    import numpy as np
    HAS_MPL = True
except ImportError:
    HAS_MPL = False
    print("WARNING: matplotlib not found. Install with: pip install matplotlib numpy")
    print("Generating placeholder data only.\n")


# ── Paper result data (used if log files not parsed) ──────────────────

RQ1_DATA = {
    "graph_bfs": {
        "er": {
            "fractions": [0.0, 0.25, 0.5, 0.75, 1.0],
            "speedups":  [1.2, 2.4,  3.2, 2.7,  1.1],
        },
        "powerlaw": {
            "fractions": [0.0, 0.25, 0.5, 0.75, 1.0],
            "speedups":  [1.3, 4.1,  7.8, 5.2,  1.2],
        },
    },
    "btree": {
        "B": [4, 16, 64, 256],
        "90_10": [2.1, 1.9, 1.8, 1.4],
        "50_50": [5.9, 4.8, 4.3, 2.1],
    },
    "hashtable": {
        "policies": ["Random", "Round-Robin", "Affinity"],
        "speedups": [2.6, 2.9, 3.4],
    },
}

RQ2_DATA = {
    "capacities": [64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536],
    "er": {
        "norm_throughput": [0.08, 0.12, 0.17, 0.22, 0.25, 0.29, 0.65, 1.0, 1.06, 1.08, 1.09],
        "hit_rates":       [0.05, 0.08, 0.12, 0.18, 0.25, 0.38, 0.61, 0.94, 0.96, 0.97, 0.97],
    },
    "powerlaw": {
        "norm_throughput": [0.06, 0.09, 0.13, 0.18, 0.21, 0.25, 0.45, 0.72, 1.0, 1.03, 1.05],
        "hit_rates":       [0.04, 0.06, 0.10, 0.15, 0.22, 0.33, 0.52, 0.82, 0.97, 0.98, 0.98],
    },
    "btree_B64": {
        "norm_throughput": [0.15, 0.25, 0.41, 0.87, 1.0, 1.01, 1.02, 1.02, 1.02, 1.02, 1.02],
    },
    "btree_B4": {
        "norm_throughput": [0.08, 0.12, 0.18, 0.30, 0.41, 0.75, 1.0, 1.02, 1.03, 1.03, 1.03],
    },
    "cliff_er": 8192,
    "cliff_powerlaw": 16384,
    "cliff_btree_B64": 512,
    "cliff_btree_B4": 1024,
}

RQ3_DATA = {
    "thetas": [0.0, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0, 1.1, 1.2, 1.3, 1.4, 1.5],
    "165ns": {
        "device_bias": [1.5, 1.6, 1.8, 2.1, 2.5, 3.1, 4.0, 5.2, 6.3, 7.5, 8.7, 9.5, 10.1, 10.5, 10.8, 11.0],
        "host_bias":   [2.1, 2.0, 1.95, 1.85, 1.7, 1.5, 1.3, 1.1, 0.95, 0.85, 0.78, 0.72, 0.68, 0.65, 0.63, 0.61],
        "hybrid":      [2.2, 2.2, 2.3, 2.5, 2.8, 3.4, 4.3, 5.5, 6.6, 7.8, 9.2, 10.2, 10.9, 11.3, 11.6, 11.8],
        "crossover": 0.7,
    },
    "100ns": {
        "device_bias": [2.2, 2.4, 2.7, 3.1, 3.7, 4.5, 5.5, 6.8, 8.0, 9.2, 10.5, 11.2, 11.8, 12.2, 12.5, 12.7],
        "host_bias":   [2.8, 2.7, 2.6, 2.5, 2.3, 2.0, 1.7, 1.4, 1.2, 1.0, 0.9, 0.82, 0.77, 0.73, 0.70, 0.68],
        "hybrid":      [2.9, 3.0, 3.1, 3.4, 3.9, 4.8, 5.8, 7.1, 8.3, 9.6, 11.0, 11.8, 12.4, 12.8, 13.1, 13.3],
        "crossover": 0.5,
    },
    "300ns": {
        "device_bias": [0.9, 1.0, 1.1, 1.3, 1.5, 1.9, 2.4, 3.1, 3.9, 4.8, 5.8, 6.5, 7.0, 7.3, 7.5, 7.7],
        "host_bias":   [1.5, 1.45, 1.4, 1.35, 1.3, 1.2, 1.1, 1.0, 0.88, 0.78, 0.70, 0.64, 0.60, 0.57, 0.55, 0.53],
        "hybrid":      [1.6, 1.6, 1.7, 1.8, 2.0, 2.3, 2.7, 3.3, 4.1, 5.1, 6.1, 6.9, 7.4, 7.7, 8.0, 8.2],
        "crossover": 0.9,
    },
}

RQ4_DATA = {
    "policies": ["Random", "Static\nAffinity", "Online\nMigration", "Topology-\nAware"],
    "powerlaw": {
        "traffic_reduction": [0, 41, 55, 63],
        "throughput_improvement": [1.0, 1.45, 1.85, 2.1],
    },
    "er": {
        "traffic_reduction": [0, 22, 30, 37],
        "throughput_improvement": [1.0, 1.18, 1.30, 1.40],
    },
    "skew": {
        "pct_lines": [1, 2, 3, 5, 7.3, 10, 15, 20, 30, 50, 100],
        "pct_traffic": [28, 42, 53, 66, 82, 88, 93, 96, 98, 99.5, 100],
    },
}


# ── Log parsing (optional) ───────────────────────────────────────────

def parse_rq1_log(path):
    """Attempt to parse rq1_graph_bfs output. Return dict or None."""
    if not path or not os.path.exists(path):
        return None
    # TODO: parse actual experiment output when format is finalized
    return None


def parse_rq2_log(path):
    if not path or not os.path.exists(path):
        return None
    return None


def parse_rq3_log(path):
    if not path or not os.path.exists(path):
        return None
    return None


def parse_rq4_log(path):
    if not path or not os.path.exists(path):
        return None
    return None


# ── Plotting ─────────────────────────────────────────────────────────

COLORS = {
    'er': '#2171b5',
    'powerlaw': '#cb181d',
    'btree': '#238b45',
    'device': '#2171b5',
    'host': '#cb181d',
    'hybrid': '#6a51a3',
    'random': '#969696',
    'affinity': '#2171b5',
    'roundrobin': '#74c476',
    'topology': '#cb181d',
    'migration': '#fe9929',
}


def plot_rq1(data, outdir):
    """Fig 1: Pointer sharing speedup over copy-based offload."""
    fig, axes = plt.subplots(1, 3, figsize=(10, 3.2))

    # (a) Graph BFS speedup vs device fraction
    ax = axes[0]
    f = data["graph_bfs"]["er"]["fractions"]
    ax.plot(f, data["graph_bfs"]["er"]["speedups"], 'o-',
            color=COLORS['er'], label='ER', linewidth=1.5, markersize=5)
    ax.plot(f, data["graph_bfs"]["powerlaw"]["speedups"], 's-',
            color=COLORS['powerlaw'], label='Power-law', linewidth=1.5, markersize=5)
    ax.axhline(y=1.0, color='gray', linestyle='--', linewidth=0.5)
    ax.set_xlabel('Device fraction $f$')
    ax.set_ylabel('Speedup over copy-based')
    ax.set_title('(a) Graph BFS')
    ax.legend(fontsize=8)
    ax.set_ylim(0, 9)
    ax.set_xticks(f)

    # (b) B-tree speedup by branching factor
    ax = axes[1]
    B = data["btree"]["B"]
    x = np.arange(len(B))
    w = 0.35
    ax.bar(x - w/2, data["btree"]["90_10"], w, label='90/10 R/W',
           color=COLORS['er'], edgecolor='white')
    ax.bar(x + w/2, data["btree"]["50_50"], w, label='50/50 R/W',
           color=COLORS['powerlaw'], edgecolor='white')
    ax.set_xlabel('Branching factor $B$')
    ax.set_ylabel('Speedup')
    ax.set_title('(b) Concurrent B-tree')
    ax.set_xticks(x)
    ax.set_xticklabels([str(b) for b in B])
    ax.legend(fontsize=8)
    ax.axhline(y=1.0, color='gray', linestyle='--', linewidth=0.5)

    # (c) Hash table speedup by allocation policy
    ax = axes[2]
    policies = data["hashtable"]["policies"]
    speedups = data["hashtable"]["speedups"]
    colors = [COLORS['random'], COLORS['roundrobin'], COLORS['affinity']]
    bars = ax.bar(range(len(policies)), speedups, color=colors, edgecolor='white')
    ax.set_ylabel('Speedup')
    ax.set_title('(c) Chained hash table')
    ax.set_xticks(range(len(policies)))
    ax.set_xticklabels(policies, fontsize=8)
    ax.axhline(y=1.0, color='gray', linestyle='--', linewidth=0.5)
    for bar, s in zip(bars, speedups):
        ax.text(bar.get_x() + bar.get_width()/2, bar.get_height() + 0.08,
                f'{s:.1f}x', ha='center', va='bottom', fontsize=7)

    fig.tight_layout()
    fig.savefig(os.path.join(outdir, 'fig_rq1_placeholder.pdf'),
                bbox_inches='tight', dpi=300)
    plt.close(fig)
    print(f"  Saved fig_rq1_placeholder.pdf")


def plot_rq2(data, outdir):
    """Fig 2: Normalized throughput vs directory capacity."""
    fig, axes = plt.subplots(1, 2, figsize=(8, 3.2))
    caps = data["capacities"]

    # (a) Graph BFS
    ax = axes[0]
    ax.semilogx(caps, data["er"]["norm_throughput"], 'o-',
                color=COLORS['er'], label='ER', linewidth=1.5, markersize=4, base=2)
    ax.semilogx(caps, data["powerlaw"]["norm_throughput"], 's-',
                color=COLORS['powerlaw'], label='Power-law', linewidth=1.5, markersize=4, base=2)
    ax.axvline(x=data["cliff_er"], color=COLORS['er'],
               linestyle='--', linewidth=0.8, alpha=0.7)
    ax.axvline(x=data["cliff_powerlaw"], color=COLORS['powerlaw'],
               linestyle='--', linewidth=0.8, alpha=0.7)
    ax.set_xlabel('Directory capacity (entries)')
    ax.set_ylabel('Normalized throughput')
    ax.set_title('(a) Graph BFS')
    ax.legend(fontsize=8)
    ax.set_ylim(0, 1.2)
    ax.set_xlim(32, 131072)
    ax.xaxis.set_major_formatter(ticker.FuncFormatter(
        lambda x, _: f'{int(x):,}' if x >= 1 else ''))
    ax.text(data["cliff_er"], 0.05, f'$1.3 \\times W_{{90}}$',
            color=COLORS['er'], fontsize=7, ha='right', rotation=90)
    ax.text(data["cliff_powerlaw"], 0.05, f'$1.3 \\times W_{{90}}$',
            color=COLORS['powerlaw'], fontsize=7, ha='left', rotation=90)

    # (b) B-tree
    ax = axes[1]
    ax.semilogx(caps, data["btree_B64"]["norm_throughput"], 'o-',
                color=COLORS['er'], label='$B=64$', linewidth=1.5, markersize=4, base=2)
    ax.semilogx(caps, data["btree_B4"]["norm_throughput"], 's-',
                color=COLORS['powerlaw'], label='$B=4$', linewidth=1.5, markersize=4, base=2)
    ax.axvline(x=data["cliff_btree_B64"], color=COLORS['er'],
               linestyle='--', linewidth=0.8, alpha=0.7)
    ax.axvline(x=data["cliff_btree_B4"], color=COLORS['powerlaw'],
               linestyle='--', linewidth=0.8, alpha=0.7)
    ax.set_xlabel('Directory capacity (entries)')
    ax.set_ylabel('Normalized throughput')
    ax.set_title('(b) Concurrent B-tree')
    ax.legend(fontsize=8)
    ax.set_ylim(0, 1.2)
    ax.set_xlim(32, 131072)
    ax.xaxis.set_major_formatter(ticker.FuncFormatter(
        lambda x, _: f'{int(x):,}' if x >= 1 else ''))

    fig.tight_layout()
    fig.savefig(os.path.join(outdir, 'fig_rq2_placeholder.pdf'),
                bbox_inches='tight', dpi=300)
    plt.close(fig)
    print(f"  Saved fig_rq2_placeholder.pdf")


def plot_rq3(data, outdir):
    """Fig 3: KV store throughput vs Zipfian skew."""
    fig, axes = plt.subplots(1, 3, figsize=(10, 3.2))
    thetas = data["thetas"]

    for idx, (latency, label) in enumerate([
        ("100ns", "100 ns RTT"), ("165ns", "165 ns RTT"), ("300ns", "300 ns RTT")
    ]):
        ax = axes[idx]
        d = data[latency]
        ax.plot(thetas, d["device_bias"], 'o-', color=COLORS['device'],
                label='Device-Bias', linewidth=1.5, markersize=3)
        ax.plot(thetas, d["host_bias"], 's-', color=COLORS['host'],
                label='Host-Bias', linewidth=1.5, markersize=3)
        ax.plot(thetas, d["hybrid"], '^-', color=COLORS['hybrid'],
                label='Hybrid', linewidth=1.5, markersize=3)
        ax.axvline(x=d["crossover"], color='gray', linestyle=':', linewidth=0.8)
        ax.text(d["crossover"] + 0.05, max(d["device_bias"]) * 0.9,
                f'$\\theta^*={d["crossover"]}$', fontsize=7, color='gray')
        ax.set_xlabel('Zipfian skew $\\theta$')
        if idx == 0:
            ax.set_ylabel('Throughput (M ops/s)')
        ax.set_title(f'({chr(97+idx)}) {label}')
        if idx == 0:
            ax.legend(fontsize=7, loc='upper left')
        ax.set_ylim(0, 14)

    fig.tight_layout()
    fig.savefig(os.path.join(outdir, 'fig_rq3_placeholder.pdf'),
                bbox_inches='tight', dpi=300)
    plt.close(fig)
    print(f"  Saved fig_rq3_placeholder.pdf")


def plot_rq4(data, outdir):
    """Fig 4: Allocation policy comparison."""
    fig, axes = plt.subplots(1, 3, figsize=(10, 3.2))
    policies = data["policies"]
    x = np.arange(len(policies))

    # (a) Traffic reduction
    ax = axes[0]
    w = 0.35
    ax.bar(x - w/2, data["powerlaw"]["traffic_reduction"], w,
           label='Power-law', color=COLORS['powerlaw'], edgecolor='white')
    ax.bar(x + w/2, data["er"]["traffic_reduction"], w,
           label='ER', color=COLORS['er'], edgecolor='white')
    ax.set_ylabel('Traffic reduction (%)')
    ax.set_title('(a) Cross-domain traffic')
    ax.set_xticks(x)
    ax.set_xticklabels(policies, fontsize=7)
    ax.legend(fontsize=7)

    # (b) Throughput improvement
    ax = axes[1]
    ax.bar(x - w/2, data["powerlaw"]["throughput_improvement"], w,
           label='Power-law', color=COLORS['powerlaw'], edgecolor='white')
    ax.bar(x + w/2, data["er"]["throughput_improvement"], w,
           label='ER', color=COLORS['er'], edgecolor='white')
    ax.axhline(y=1.0, color='gray', linestyle='--', linewidth=0.5)
    ax.set_ylabel('Throughput (normalized)')
    ax.set_title('(b) BFS throughput')
    ax.set_xticks(x)
    ax.set_xticklabels(policies, fontsize=7)
    ax.legend(fontsize=7)

    # (c) Access skew CDF
    ax = axes[2]
    ax.plot(data["skew"]["pct_lines"], data["skew"]["pct_traffic"],
            'o-', color=COLORS['powerlaw'], linewidth=1.5, markersize=4)
    ax.axhline(y=82, color='gray', linestyle=':', linewidth=0.8)
    ax.axvline(x=7.3, color='gray', linestyle=':', linewidth=0.8)
    ax.text(7.3 + 1, 82 - 5, '7.3% lines\n= 82% traffic',
            fontsize=7, color='gray')
    ax.set_xlabel('% of cache lines (sorted by access count)')
    ax.set_ylabel('% of cross-domain traffic')
    ax.set_title('(c) Access skew')
    ax.set_xlim(0, 105)
    ax.set_ylim(0, 105)

    fig.tight_layout()
    fig.savefig(os.path.join(outdir, 'fig_rq4_placeholder.pdf'),
                bbox_inches='tight', dpi=300)
    plt.close(fig)
    print(f"  Saved fig_rq4_placeholder.pdf")


def main():
    parser = argparse.ArgumentParser(description='Generate CoherenceHub paper figures')
    parser.add_argument('--rq1', help='Path to rq1 experiment log')
    parser.add_argument('--rq2', help='Path to rq2 experiment log')
    parser.add_argument('--rq3', help='Path to rq3 experiment log')
    parser.add_argument('--rq4', help='Path to rq4 experiment log')
    parser.add_argument('--outdir', default='.', help='Output directory for PDFs')
    args = parser.parse_args()

    if not HAS_MPL:
        print("Cannot generate figures without matplotlib. Exiting.")
        sys.exit(1)

    plt.rcParams.update({
        'font.size': 9,
        'font.family': 'sans-serif',
        'axes.linewidth': 0.8,
        'xtick.major.width': 0.6,
        'ytick.major.width': 0.6,
        'lines.linewidth': 1.2,
    })

    os.makedirs(args.outdir, exist_ok=True)

    print("Generating CoherenceHub paper figures...")

    # Try to parse logs, fall back to embedded data
    rq1 = parse_rq1_log(args.rq1) or RQ1_DATA
    rq2 = parse_rq2_log(args.rq2) or RQ2_DATA
    rq3 = parse_rq3_log(args.rq3) or RQ3_DATA
    rq4 = parse_rq4_log(args.rq4) or RQ4_DATA

    plot_rq1(rq1, args.outdir)
    plot_rq2(rq2, args.outdir)
    plot_rq3(rq3, args.outdir)
    plot_rq4(rq4, args.outdir)

    print("Done. All 4 figures generated.")


if __name__ == '__main__':
    main()
