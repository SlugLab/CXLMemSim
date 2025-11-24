#!/usr/bin/env python3
"""
Plot switch accuracy experiment results.
"""

import json
import re
import sys
import matplotlib.pyplot as plt
import numpy as np
from pathlib import Path
from collections import defaultdict

def parse_stdout_metrics(stdout):
    """Extract metrics from stdout."""
    metrics = {
        'emulated_time': 0.0,
        'total_delay': 0.0,
        'pebs_samples': 0,
        'pebs_llcmiss': 0,
        'lbr_samples': 0,
        'bpftime_samples': 0,
        'hitm': 0,
        'local': 0,
        'remote': 0,
    }

    if not stdout:
        return metrics

    # Parse each line
    for line in stdout.split('\n'):
        if 'emulated time' in line:
            match = re.search(r'emulated time\s*=\s*([\d.]+)', line)
            if match:
                metrics['emulated_time'] = float(match.group(1))
        elif 'total delay' in line:
            match = re.search(r'total delay\s*=\s*([\d.]+)', line)
            if match:
                metrics['total_delay'] = float(match.group(1))
        elif 'PEBS sample total' in line:
            match = re.search(r'PEBS sample total\s+(\d+)\s+(\d+)', line)
            if match:
                metrics['pebs_samples'] = int(match.group(1))
                metrics['pebs_llcmiss'] = int(match.group(2))
        elif 'LBR sample total' in line:
            match = re.search(r'LBR sample total\s+(\d+)', line)
            if match:
                metrics['lbr_samples'] = int(match.group(1))
        elif 'bpftime sample total' in line:
            match = re.search(r'bpftime sample total\s+(\d+)', line)
            if match:
                metrics['bpftime_samples'] = int(match.group(1))
        elif 'HITM:' in line:
            match = re.search(r'HITM:\s*(\d+)', line)
            if match:
                metrics['hitm'] = int(match.group(1))
        elif 'Local:' in line:
            match = re.search(r'Local:\s*(\d+)', line)
            if match:
                metrics['local'] = int(match.group(1))
        elif 'Remote:' in line:
            match = re.search(r'Remote:\s*(\d+)', line)
            if match:
                metrics['remote'] = int(match.group(1))

    return metrics

def load_results(filepath):
    """Load and parse results JSON."""
    with open(filepath) as f:
        data = json.load(f)

    parsed_results = []
    for result in data['results']:
        if not result.get('success', False):
            continue

        config = result['config']
        metrics = parse_stdout_metrics(result.get('stdout', ''))

        parsed_results.append({
            'topology': config['topology_name'],
            'num_switches': config['num_switches'],
            'workload': config['workload_name'],
            'workload_type': config['workload_type'],
            'bandwidth': config['bandwidth_name'],
            'switch_latency': config['switch_latency_name'],
            'duration': result.get('duration', 0),
            **metrics
        })

    return parsed_results

def plot_by_topology(results, output_dir):
    """Plot metrics grouped by topology."""

    # Group by topology
    by_topo = defaultdict(list)
    for r in results:
        by_topo[r['topology']].append(r)

    # Order topologies
    topo_order = ['no_switch', 'one_level', 'two_level', 'three_level', 'four_level']
    topos = [t for t in topo_order if t in by_topo]

    # Plot 1: Emulated time by topology and workload
    fig, axes = plt.subplots(2, 2, figsize=(14, 10))

    # Emulated time
    ax = axes[0, 0]
    workloads = list(set(r['workload'] for r in results))
    x = np.arange(len(topos))
    width = 0.8 / len(workloads)

    for i, wl in enumerate(workloads):
        times = []
        for topo in topos:
            vals = [r['emulated_time'] for r in by_topo[topo] if r['workload'] == wl]
            times.append(np.mean(vals) if vals else 0)
        ax.bar(x + i * width, times, width, label=wl)

    ax.set_xlabel('Topology')
    ax.set_ylabel('Emulated Time (s)')
    ax.set_title('Emulated Time by Topology and Workload')
    ax.set_xticks(x + width * (len(workloads) - 1) / 2)
    ax.set_xticklabels(topos, rotation=45)
    ax.legend()

    # PEBS samples (log scale)
    ax = axes[0, 1]
    for i, wl in enumerate(workloads):
        samples = []
        for topo in topos:
            vals = [r['pebs_samples'] for r in by_topo[topo] if r['workload'] == wl]
            avg = np.mean(vals) if vals else 0
            samples.append(max(avg, 1))  # Avoid log(0)
        ax.bar(x + i * width, samples, width, label=wl)

    ax.set_xlabel('Topology')
    ax.set_ylabel('PEBS Samples (log scale)')
    ax.set_title('PEBS Samples by Topology and Workload')
    ax.set_yscale('log')
    ax.set_xticks(x + width * (len(workloads) - 1) / 2)
    ax.set_xticklabels(topos, rotation=45)
    ax.legend()

    # HITM (cache hits) - log scale
    ax = axes[1, 0]
    for i, wl in enumerate(workloads):
        hitm = []
        for topo in topos:
            vals = [r['hitm'] for r in by_topo[topo] if r['workload'] == wl]
            avg = np.mean(vals) if vals else 0
            hitm.append(max(avg, 1))  # Avoid log(0)
        ax.bar(x + i * width, hitm, width, label=wl)

    ax.set_xlabel('Topology')
    ax.set_ylabel('HITM Count (log scale)')
    ax.set_title('Cache HITM by Topology and Workload')
    ax.set_yscale('log')
    ax.set_xticks(x + width * (len(workloads) - 1) / 2)
    ax.set_xticklabels(topos, rotation=45)
    ax.legend()

    # Total delay
    ax = axes[1, 1]
    for i, wl in enumerate(workloads):
        delays = []
        for topo in topos:
            vals = [r['total_delay'] for r in by_topo[topo] if r['workload'] == wl]
            avg = np.mean(vals) if vals else 0
            delays.append(max(avg, 0.001))  # Small minimum for visibility
        ax.bar(x + i * width, delays, width, label=wl)

    ax.set_xlabel('Topology')
    ax.set_ylabel('Total Delay (s)')
    ax.set_title('Total Delay by Topology and Workload')
    # Use log scale if values vary significantly
    if any(delays) and max(max(delays) for delays in [delays] if delays) > 10 * min(d for d in [delays] if d for d in delays if d > 0):
        ax.set_yscale('log')
    ax.set_xticks(x + width * (len(workloads) - 1) / 2)
    ax.set_xticklabels(topos, rotation=45)
    ax.legend()

    plt.tight_layout()
    plt.savefig(output_dir / 'topology_comparison.pdf', dpi=150)
    plt.close()
    print(f"Saved: {output_dir / 'topology_comparison.pdf'}")

def plot_by_switch_latency(results, output_dir):
    """Plot metrics grouped by switch latency."""

    # Group by switch latency
    by_lat = defaultdict(list)
    for r in results:
        by_lat[r['switch_latency']].append(r)

    lat_order = ['40ns', '50ns', '60ns', '70ns']
    lats = [l for l in lat_order if l in by_lat]

    if len(lats) < 2:
        return

    fig, ax = plt.subplots(figsize=(10, 6))

    topos = list(set(r['topology'] for r in results))
    topo_order = ['no_switch', 'one_level', 'two_level', 'three_level', 'four_level']
    topos = [t for t in topo_order if t in topos]

    x = np.arange(len(lats))
    width = 0.8 / len(topos)

    for i, topo in enumerate(topos):
        times = []
        for lat in lats:
            vals = [r['emulated_time'] for r in by_lat[lat] if r['topology'] == topo]
            times.append(np.mean(vals) if vals else 0)
        ax.bar(x + i * width, times, width, label=topo)

    ax.set_xlabel('Switch Latency')
    ax.set_ylabel('Emulated Time (s)')
    ax.set_title('Impact of Switch Latency on Emulated Time')
    ax.set_xticks(x + width * (len(topos) - 1) / 2)
    ax.set_xticklabels(lats)
    ax.legend()

    plt.tight_layout()
    plt.savefig(output_dir / 'switch_latency_impact.pdf', dpi=150)
    plt.close()
    print(f"Saved: {output_dir / 'switch_latency_impact.pdf'}")

def plot_by_bandwidth(results, output_dir):
    """Plot metrics grouped by bandwidth."""

    by_bw = defaultdict(list)
    for r in results:
        by_bw[r['bandwidth']].append(r)

    bws = sorted(by_bw.keys())

    if len(bws) < 2:
        return

    fig, ax = plt.subplots(figsize=(10, 6))

    topos = list(set(r['topology'] for r in results))
    topo_order = ['no_switch', 'one_level', 'two_level', 'three_level', 'four_level']
    topos = [t for t in topo_order if t in topos]

    x = np.arange(len(bws))
    width = 0.8 / len(topos)

    for i, topo in enumerate(topos):
        times = []
        for bw in bws:
            vals = [r['emulated_time'] for r in by_bw[bw] if r['topology'] == topo]
            times.append(np.mean(vals) if vals else 0)
        ax.bar(x + i * width, times, width, label=topo)

    ax.set_xlabel('Bandwidth')
    ax.set_ylabel('Emulated Time (s)')
    ax.set_title('Impact of Bandwidth on Emulated Time')
    ax.set_xticks(x + width * (len(topos) - 1) / 2)
    ax.set_xticklabels(bws)
    ax.legend()

    plt.tight_layout()
    plt.savefig(output_dir / 'bandwidth_impact.pdf', dpi=150)
    plt.close()
    print(f"Saved: {output_dir / 'bandwidth_impact.pdf'}")

def plot_summary_table(results, output_dir):
    """Create a summary table of results."""

    # Group and summarize
    summary = defaultdict(lambda: defaultdict(list))
    for r in results:
        key = (r['topology'], r['workload'])
        summary[key]['emulated_time'].append(r['emulated_time'])
        summary[key]['total_delay'].append(r['total_delay'])
        summary[key]['pebs_samples'].append(r['pebs_samples'])

    # Write summary
    with open(output_dir / 'summary.txt', 'w') as f:
        f.write("Switch Accuracy Experiment Summary\n")
        f.write("=" * 70 + "\n\n")

        for (topo, wl), data in sorted(summary.items()):
            f.write(f"{topo} + {wl}:\n")
            f.write(f"  Avg Emulated Time: {np.mean(data['emulated_time']):.3f}s\n")
            f.write(f"  Avg Total Delay: {np.mean(data['total_delay']):.3f}\n")
            f.write(f"  Avg PEBS Samples: {np.mean(data['pebs_samples']):.0f}\n")
            f.write("\n")

    print(f"Saved: {output_dir / 'summary.txt'}")

def main():
    if len(sys.argv) < 2:
        print("Usage: python plot_switch_accuracy.py <results.json>")
        sys.exit(1)

    results_path = Path(sys.argv[1])
    output_dir = results_path.parent

    print(f"Loading results from: {results_path}")
    results = load_results(results_path)
    print(f"Loaded {len(results)} successful experiments")

    if not results:
        print("No successful results to plot")
        sys.exit(1)

    # Generate plots
    plot_by_topology(results, output_dir)
    plot_by_switch_latency(results, output_dir)
    plot_by_bandwidth(results, output_dir)
    plot_summary_table(results, output_dir)

    print("\nDone! Plots saved to:", output_dir)

if __name__ == "__main__":
    main()
