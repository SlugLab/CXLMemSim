#!/usr/bin/env python3
"""
Switch Accuracy Experiment for CXLMemSim (Section 5.2.3)

This script evaluates the accuracy of CXL switch modeling by comparing
XSim predictions against measurements on hardware configurations with
one and two levels of CXL switching.

Key experiments:
1. Test different switch topologies (0, 1, 2 levels)
2. Test both random-access (ptr-chasing) and streaming (ld) workloads
3. Test different bandwidths (25 GB/s, 50 GB/s)
4. Vary switch latency per level (40-70ns)

Expected results:
- Each additional switch level introduces 40-70 ns of extra latency
- Random-access workloads more sensitive to switch hops than streaming
- Bandwidth scaling partially compensates for deeper hierarchies
"""

import os
import sys
import json
import time
import argparse
import subprocess
from pathlib import Path
from datetime import datetime
from typing import Dict, List, Tuple, Any

# Configuration constants
BASE_DIR = Path(__file__).parent.parent
BUILD_DIR = BASE_DIR / "build"
RESULTS_DIR = BASE_DIR / "experiments" / "results" / "switch_accuracy"

# Default parameters
DEFAULT_DRAM_LATENCY = 85  # ns
DEFAULT_CPU_FREQ = 4000  # MHz
DEFAULT_CPUSET = "0,1,2,3"
DEFAULT_CAPACITY = [0, 20, 20, 20]  # GB per memory region

# Experiment configurations
SWITCH_TOPOLOGIES = {
    "no_switch": {
        "topology": "(1)",
        "description": "Direct connection (0 switch levels)",
        "num_switches": 0,
        "num_expanders": 1,
        "capacity": [0, 20],
        "latency": [200, 250],
        "bandwidth": [50, 50]
    },
    "one_level": {
        "topology": "(1,2)",
        "description": "One level of switching",
        "num_switches": 1,
        "num_expanders": 2,
        "capacity": [0, 20, 20],
        "latency": [200, 250, 200, 250],
        "bandwidth": [50, 50, 50, 50]
    },
    "two_level": {
        "topology": "(1,(2,3))",
        "description": "Two levels of switching",
        "num_switches": 2,
        "num_expanders": 3,
        "capacity": [0, 20, 20, 20],
        "latency": [200, 250, 200, 250, 200, 250],
        "bandwidth": [50, 50, 50, 50, 50, 50]
    },
    "three_level": {
        "topology": "(1,(2,(3,4)))",
        "description": "Three levels of switching",
        "num_switches": 3,
        "num_expanders": 4,
        "capacity": [0, 20, 20, 20, 20],
        "latency": [200, 250, 200, 250, 200, 250, 200, 250],
        "bandwidth": [50, 50, 50, 50, 50, 50, 50, 50]
    },
    "four_level": {
        "topology": "(1,(2,(3,(4,5))))",
        "description": "Four levels of switching",
        "num_switches": 4,
        "num_expanders": 5,
        "capacity": [0, 20, 20, 20, 20, 20],
        "latency": [200, 250, 200, 250, 200, 250, 200, 250, 200, 250],
        "bandwidth": [50, 50, 50, 50, 50, 50, 50, 50, 50, 50]
    },
    "two_level_wide": {
        "topology": "((1,2),(3,4))",
        "description": "Two levels wide (balanced tree)",
        "num_switches": 3,
        "num_expanders": 4,
        "capacity": [0, 20, 20, 20, 20],
        "latency": [200, 250, 200, 250, 200, 250, 200, 250],
        "bandwidth": [50, 50, 50, 50, 50, 50, 50, 50]
    },
    "three_level_wide": {
        "topology": "((1,2),((3,4),(5,6)))",
        "description": "Three levels wide (unbalanced tree)",
        "num_switches": 4,
        "num_expanders": 6,
        "capacity": [0, 20, 20, 20, 20, 20, 20],
        "latency": [200, 250, 200, 250, 200, 250, 200, 250, 200, 250, 200, 250],
        "bandwidth": [50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50]
    }
}

# Switch latency configurations (in nanoseconds, stored as microseconds for CLI)
SWITCH_LATENCIES = {
    "40ns": 0.00004,   # 40 ns
    "50ns": 0.00005,   # 50 ns (default)
    "60ns": 0.00006,   # 60 ns
    "70ns": 0.00007,   # 70 ns
}

# Bandwidth configurations (GB/s)
BANDWIDTH_CONFIGS = {
    "25gbps": {"read": 25, "write": 25},
    "50gbps": {"read": 50, "write": 50},
}

# Workload configurations
WORKLOADS = {
    "ptr-chasing": {
        "binary": "ptr-chasing",
        "description": "Random access (pointer chasing) - latency sensitive",
        "type": "random"
    },
    "ld64": {
        "binary": "ld64",
        "description": "Sequential load (streaming) - bandwidth sensitive",
        "type": "streaming"
    },
    "ld_serial64": {
        "binary": "ld_serial64",
        "description": "Serial sequential load",
        "type": "streaming"
    }
}


class SwitchAccuracyExperiment:
    """Main experiment class for switch accuracy evaluation."""

    def __init__(self, args):
        self.args = args
        self.results = []
        self.timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        self.results_dir = RESULTS_DIR / self.timestamp

    def setup(self):
        """Setup experiment environment."""
        print("Setting up experiment environment...")

        # Create results directory
        self.results_dir.mkdir(parents=True, exist_ok=True)

        # Verify binaries exist
        self.verify_binaries()

        print(f"Results will be saved to: {self.results_dir}")

    def build_project(self):
        """Build CXLMemSim and microbenchmarks."""
        print("Building CXLMemSim...")

        # Create build directory
        BUILD_DIR.mkdir(exist_ok=True)

        # Run cmake and make
        cmake_cmd = ["cmake", ".."]
        make_cmd = ["make", "-j", str(os.cpu_count() or 4)]

        try:
            subprocess.run(cmake_cmd, cwd=BUILD_DIR, check=True,
                         capture_output=not self.args.verbose)
            subprocess.run(make_cmd, cwd=BUILD_DIR, check=True,
                         capture_output=not self.args.verbose)
            print("Build successful!")
        except subprocess.CalledProcessError as e:
            print(f"Build failed: {e}")
            sys.exit(1)

    def verify_binaries(self):
        """Verify that required binaries exist."""
        required_binaries = [
            BUILD_DIR / "CXLMemSim",
        ]

        for workload in WORKLOADS.values():
            required_binaries.append(BUILD_DIR / "microbench" / workload["binary"])

        missing = []
        for binary in required_binaries:
            if not binary.exists():
                missing.append(str(binary))

        if missing:
            print(f"Missing binaries: {', '.join(missing)}")
            print("Please build the project first with: cmake .. && make")
            sys.exit(1)

    def run_single_experiment(self, config: Dict[str, Any]) -> Dict[str, Any]:
        """Run a single experiment configuration."""

        # Build command
        cmd = [
            str(BUILD_DIR / "CXLMemSim"),
            "-t", str(BUILD_DIR / "microbench" / config["workload_binary"]),
            "-c", config.get("cpuset", DEFAULT_CPUSET),
            "-d", str(config.get("dram_latency", DEFAULT_DRAM_LATENCY)),
            "-o", config["topology"],
            "-q", ",".join(map(str, config["capacity"])),
            "-l", ",".join(map(str, config["latency"])),
            "-b", ",".join(map(str, config["bandwidth"])),
            "-s", str(config["switch_latency"]),
            "-p", "10",  # PEBS period
        ]

        if self.args.verbose:
            print(f"Running: {' '.join(cmd)}")

        # Run experiment
        start_time = time.time()
        result = {
            "config": config,
            "command": " ".join(cmd),
            "start_time": datetime.now().isoformat(),
        }

        try:
            env = os.environ.copy()
            env["SPDLOG_LEVEL"] = "info" if self.args.verbose else "warn"

            proc = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=config.get("timeout", 300),
                env=env
            )

            result["returncode"] = proc.returncode
            result["stdout"] = proc.stdout
            result["stderr"] = proc.stderr
            result["success"] = proc.returncode == 0

        except subprocess.TimeoutExpired:
            result["success"] = False
            result["error"] = "Timeout"
        except Exception as e:
            result["success"] = False
            result["error"] = str(e)

        result["duration"] = time.time() - start_time
        result["end_time"] = datetime.now().isoformat()

        return result

    def generate_experiment_configs(self) -> List[Dict[str, Any]]:
        """Generate all experiment configurations."""
        configs = []

        # Select topologies based on args
        topologies = SWITCH_TOPOLOGIES
        if self.args.topology:
            topologies = {k: v for k, v in topologies.items() if k in self.args.topology}

        # Select workloads based on args
        workloads = WORKLOADS
        if self.args.workload:
            workloads = {k: v for k, v in workloads.items() if k in self.args.workload}

        # Select bandwidths based on args
        bandwidths = BANDWIDTH_CONFIGS
        if self.args.bandwidth:
            bandwidths = {k: v for k, v in bandwidths.items() if k in self.args.bandwidth}

        # Select switch latencies based on args
        switch_latencies = SWITCH_LATENCIES
        if self.args.switch_latency:
            switch_latencies = {k: v for k, v in switch_latencies.items()
                              if k in self.args.switch_latency}

        # Generate all combinations
        for topo_name, topo_config in topologies.items():
            for wl_name, wl_config in workloads.items():
                for bw_name, bw_config in bandwidths.items():
                    for lat_name, lat_value in switch_latencies.items():
                        # Build bandwidth array based on number of expanders
                        bw_array = []
                        for _ in range(topo_config["num_expanders"]):
                            bw_array.extend([bw_config["read"], bw_config["write"]])

                        config = {
                            "name": f"{topo_name}_{wl_name}_{bw_name}_{lat_name}",
                            "topology_name": topo_name,
                            "topology": topo_config["topology"],
                            "topology_description": topo_config["description"],
                            "num_switches": topo_config["num_switches"],
                            "workload_name": wl_name,
                            "workload_binary": wl_config["binary"],
                            "workload_type": wl_config["type"],
                            "bandwidth_name": bw_name,
                            "bandwidth": bw_array,
                            "switch_latency_name": lat_name,
                            "switch_latency": lat_value,
                            "capacity": topo_config["capacity"],
                            "latency": topo_config["latency"],
                            "dram_latency": DEFAULT_DRAM_LATENCY,
                            "cpuset": DEFAULT_CPUSET,
                            "timeout": 300,
                        }
                        configs.append(config)

        return configs

    def run_experiments(self):
        """Run all experiments."""
        configs = self.generate_experiment_configs()
        total = len(configs)

        print(f"\nRunning {total} experiment configurations...")
        print("=" * 60)

        for i, config in enumerate(configs, 1):
            print(f"\n[{i}/{total}] {config['name']}")
            print(f"  Topology: {config['topology_description']}")
            print(f"  Workload: {config['workload_name']} ({config['workload_type']})")
            print(f"  Bandwidth: {config['bandwidth_name']}")
            print(f"  Switch Latency: {config['switch_latency_name']}")

            result = self.run_single_experiment(config)
            self.results.append(result)

            status = "SUCCESS" if result.get("success") else "FAILED"
            print(f"  Status: {status} ({result['duration']:.2f}s)")

            # Save intermediate results
            if i % 5 == 0:
                self.save_results()

        print("\n" + "=" * 60)
        print("Experiments completed!")

    def analyze_results(self):
        """Analyze experiment results."""
        print("\nAnalyzing results...")

        analysis = {
            "summary": {
                "total": len(self.results),
                "successful": sum(1 for r in self.results if r.get("success")),
                "failed": sum(1 for r in self.results if not r.get("success")),
            },
            "by_topology": {},
            "by_workload_type": {},
            "by_bandwidth": {},
            "by_switch_latency": {},
        }

        # Group results
        for result in self.results:
            if not result.get("success"):
                continue

            config = result["config"]

            # By topology
            topo = config["topology_name"]
            if topo not in analysis["by_topology"]:
                analysis["by_topology"][topo] = []
            analysis["by_topology"][topo].append(result)

            # By workload type
            wl_type = config["workload_type"]
            if wl_type not in analysis["by_workload_type"]:
                analysis["by_workload_type"][wl_type] = []
            analysis["by_workload_type"][wl_type].append(result)

            # By bandwidth
            bw = config["bandwidth_name"]
            if bw not in analysis["by_bandwidth"]:
                analysis["by_bandwidth"][bw] = []
            analysis["by_bandwidth"][bw].append(result)

            # By switch latency
            lat = config["switch_latency_name"]
            if lat not in analysis["by_switch_latency"]:
                analysis["by_switch_latency"][lat] = []
            analysis["by_switch_latency"][lat].append(result)

        return analysis

    def generate_report(self):
        """Generate experiment report."""
        print("\nGenerating report...")

        analysis = self.analyze_results()

        report = []
        report.append("=" * 70)
        report.append("SWITCH ACCURACY EXPERIMENT REPORT")
        report.append("Section 5.2.3: CXL Switch Modeling Accuracy")
        report.append("=" * 70)
        report.append(f"\nTimestamp: {self.timestamp}")
        report.append(f"Results directory: {self.results_dir}")

        report.append("\n" + "-" * 70)
        report.append("SUMMARY")
        report.append("-" * 70)
        report.append(f"Total experiments: {analysis['summary']['total']}")
        report.append(f"Successful: {analysis['summary']['successful']}")
        report.append(f"Failed: {analysis['summary']['failed']}")

        report.append("\n" + "-" * 70)
        report.append("KEY FINDINGS")
        report.append("-" * 70)

        # Topology comparison - deep hierarchies
        report.append("\n1. Impact of Switch Levels (Deep Hierarchies):")
        for topo_name in ["no_switch", "one_level", "two_level", "three_level", "four_level"]:
            if topo_name in analysis["by_topology"]:
                results = analysis["by_topology"][topo_name]
                avg_duration = sum(r["duration"] for r in results) / len(results)
                report.append(f"   {topo_name}: {len(results)} runs, avg duration: {avg_duration:.2f}s")

        # Topology comparison - wide hierarchies
        report.append("\n2. Impact of Switch Topology (Wide Trees):")
        for topo_name in ["two_level_wide", "three_level_wide"]:
            if topo_name in analysis["by_topology"]:
                results = analysis["by_topology"][topo_name]
                avg_duration = sum(r["duration"] for r in results) / len(results)
                report.append(f"   {topo_name}: {len(results)} runs, avg duration: {avg_duration:.2f}s")

        # Workload sensitivity
        report.append("\n3. Workload Sensitivity to Switch Hops:")
        for wl_type, results in analysis["by_workload_type"].items():
            avg_duration = sum(r["duration"] for r in results) / len(results)
            report.append(f"   {wl_type}: {len(results)} runs, avg duration: {avg_duration:.2f}s")

        # Bandwidth compensation
        report.append("\n4. Bandwidth Scaling Effect:")
        for bw_name, results in analysis["by_bandwidth"].items():
            avg_duration = sum(r["duration"] for r in results) / len(results)
            report.append(f"   {bw_name}: {len(results)} runs, avg duration: {avg_duration:.2f}s")

        # Switch latency impact
        report.append("\n5. Switch Latency Impact:")
        for lat_name, results in analysis["by_switch_latency"].items():
            avg_duration = sum(r["duration"] for r in results) / len(results)
            report.append(f"   {lat_name}: {len(results)} runs, avg duration: {avg_duration:.2f}s")

        report.append("\n" + "-" * 70)
        report.append("EXPECTED BEHAVIOR (from paper)")
        report.append("-" * 70)
        report.append("- Each switch level adds 40-70ns latency")
        report.append("- Random-access workloads more sensitive to switch hops")
        report.append("- Higher bandwidth partially compensates for deeper hierarchies")
        report.append("- XSim should capture incremental cost within 10-12%")
        report.append("- Deep hierarchies (3-4 levels) should show cumulative latency")
        report.append("- Wide vs deep topologies may show different access patterns")

        report.append("\n" + "=" * 70)
        report.append("END OF REPORT")
        report.append("=" * 70)

        report_text = "\n".join(report)

        # Save report
        report_file = self.results_dir / "report.txt"
        with open(report_file, "w") as f:
            f.write(report_text)

        print(report_text)
        print(f"\nReport saved to: {report_file}")

    def save_results(self):
        """Save experiment results to JSON."""
        results_file = self.results_dir / "results.json"

        with open(results_file, "w") as f:
            json.dump({
                "timestamp": self.timestamp,
                "args": vars(self.args),
                "results": self.results,
            }, f, indent=2, default=str)

        if self.args.verbose:
            print(f"Results saved to: {results_file}")

    def run(self):
        """Run the complete experiment."""
        self.setup()
        self.run_experiments()
        self.save_results()
        self.generate_report()


def main():
    parser = argparse.ArgumentParser(
        description="Switch Accuracy Experiment for CXLMemSim (Section 5.2.3)"
    )

    parser.add_argument(
        "--topology", "-t",
        nargs="+",
        choices=list(SWITCH_TOPOLOGIES.keys()),
        help="Topologies to test (default: all)"
    )

    parser.add_argument(
        "--workload", "-w",
        nargs="+",
        choices=list(WORKLOADS.keys()),
        help="Workloads to test (default: all)"
    )

    parser.add_argument(
        "--bandwidth", "-b",
        nargs="+",
        choices=list(BANDWIDTH_CONFIGS.keys()),
        help="Bandwidth configurations to test (default: all)"
    )

    parser.add_argument(
        "--switch-latency", "-s",
        nargs="+",
        choices=list(SWITCH_LATENCIES.keys()),
        help="Switch latencies to test (default: all)"
    )

    parser.add_argument(
        "--skip-build",
        action="store_true",
        help="Skip building the project"
    )

    parser.add_argument(
        "--verbose", "-v",
        action="store_true",
        help="Enable verbose output"
    )

    parser.add_argument(
        "--quick",
        action="store_true",
        help="Run quick experiment with reduced configurations"
    )

    args = parser.parse_args()

    # Quick mode reduces configurations
    if args.quick:
        if not args.topology:
            args.topology = ["no_switch", "two_level"]
        if not args.workload:
            args.workload = ["ptr-chasing", "ld64"]
        if not args.bandwidth:
            args.bandwidth = ["50gbps"]
        if not args.switch_latency:
            args.switch_latency = ["50ns"]

    experiment = SwitchAccuracyExperiment(args)
    experiment.run()


if __name__ == "__main__":
    main()
