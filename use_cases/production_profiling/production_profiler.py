#!/usr/bin/env python3
"""
Production Workload Profiling Framework for CXLMemSim
Enables performance profiling of production applications with various CXL configurations
"""

import argparse
import json
import os
import subprocess
import time
import multiprocessing
from pathlib import Path
from typing import Dict, List, Tuple
import pandas as pd
import matplotlib.pyplot as plt
import yaml

class ProductionProfiler:
    def __init__(self, cxlmemsim_path: str, output_dir: str):
        self.cxlmemsim_path = Path(cxlmemsim_path)
        self.output_dir = Path(output_dir)
        self.output_dir.mkdir(parents=True, exist_ok=True)
        self.results = []
        
    def profile_workload(self, workload_config: Dict) -> Dict:
        """Profile a single workload with given CXL configuration"""
        
        cmd = [
            str(self.cxlmemsim_path),
            "-t", workload_config["binary"],
            "-p", str(workload_config.get("pebs_period", 10)),  # Changed from interval to pebs_period
            "-c", workload_config.get("cpuset", "0,2"),
            "-d", str(workload_config.get("dram_latency", 85))
        ]
        
        # Add CXL-specific parameters
        if "bandwidth" in workload_config:
            cmd.extend(["-b", ",".join(map(str, workload_config["bandwidth"]))])
        if "latency" in workload_config:
            cmd.extend(["-l", ",".join(map(str, workload_config["latency"]))])
        if "capacity" in workload_config:
            cmd.extend(["-q", ",".join(map(str, workload_config["capacity"]))])  # Changed from -c to -q
        if "topology" in workload_config:
            cmd.extend(["-o", workload_config["topology"]])
            
        # Add workload arguments
        if "args" in workload_config:
            cmd.extend(["--"] + workload_config["args"])
            
        # Run profiling
        start_time = time.time()
        env = os.environ.copy()
        env["SPDLOG_LEVEL"] = workload_config.get("log_level", "info")
        
        try:
            result = subprocess.run(
                cmd, 
                capture_output=True, 
                text=True, 
                env=env,
                timeout=workload_config.get("timeout", 3600)
            )
            
            execution_time = time.time() - start_time
            
            profile_result = {
                "workload": workload_config["name"],
                "config": workload_config,
                "execution_time": execution_time,
                "stdout": result.stdout,
                "stderr": result.stderr,
                "returncode": result.returncode,
                "timestamp": time.strftime("%Y-%m-%d %H:%M:%S")
            }
            
            # Parse CXLMemSim output for metrics
            metrics = self._parse_output(result.stdout)
            profile_result["metrics"] = metrics
            
            return profile_result
            
        except subprocess.TimeoutExpired:
            return {
                "workload": workload_config["name"],
                "config": workload_config,
                "error": "Timeout expired",
                "timestamp": time.strftime("%Y-%m-%d %H:%M:%S")
            }
    
    def _parse_output(self, output: str) -> Dict:
        """Parse CXLMemSim output for performance metrics"""
        metrics = {
            "local_accesses": 0,
            "remote_accesses": 0,
            "average_latency": 0,
            "bandwidth_utilization": 0,
            "page_faults": 0
        }
        
        # Parse output lines for metrics
        for line in output.split('\n'):
            if "Local memory accesses:" in line:
                metrics["local_accesses"] = int(line.split(":")[-1].strip())
            elif "Remote memory accesses:" in line:
                metrics["remote_accesses"] = int(line.split(":")[-1].strip())
            elif "Average latency:" in line:
                metrics["average_latency"] = float(line.split(":")[-1].strip().replace("ns", ""))
            elif "Bandwidth utilization:" in line:
                metrics["bandwidth_utilization"] = float(line.split(":")[-1].strip().replace("%", ""))
                
        return metrics
    
    def run_production_suite(self, suite_config_file: str):
        """Run a complete production profiling suite"""
        
        with open(suite_config_file, 'r') as f:
            suite_config = yaml.safe_load(f)
            
        total_configs = len(suite_config["workloads"]) * len(suite_config["cxl_configurations"])
        print(f"Running {total_configs} profiling configurations...")
        
        # Use multiprocessing for parallel execution
        with multiprocessing.Pool(processes=suite_config.get("parallel_jobs", 4)) as pool:
            tasks = []
            
            for workload in suite_config["workloads"]:
                for cxl_config in suite_config["cxl_configurations"]:
                    # Merge workload and CXL configuration
                    combined_config = {**workload, **cxl_config}
                    combined_config["name"] = f"{workload['name']}_{cxl_config['name']}"
                    tasks.append(combined_config)
                    
            results = pool.map(self.profile_workload, tasks)
            
        self.results.extend(results)
        self._save_results()
        self._generate_report()
        
    def _save_results(self):
        """Save profiling results to JSON"""
        output_file = self.output_dir / f"profiling_results_{int(time.time())}.json"
        with open(output_file, 'w') as f:
            json.dump(self.results, f, indent=2)
        print(f"Results saved to {output_file}")
        
    def _generate_report(self):
        """Generate performance comparison report"""
        
        # Convert results to DataFrame for analysis
        data = []
        for result in self.results:
            if "metrics" in result:
                row = {
                    "workload": result["workload"],
                    "execution_time": result["execution_time"],
                    **result["metrics"]
                }
                data.append(row)
                
        df = pd.DataFrame(data)
        
        # Generate visualizations
        fig, axes = plt.subplots(2, 2, figsize=(12, 10))
        
        # Execution time comparison
        df.groupby("workload")["execution_time"].mean().plot(kind="bar", ax=axes[0, 0])
        axes[0, 0].set_title("Average Execution Time by Workload")
        axes[0, 0].set_ylabel("Time (seconds)")
        
        # Remote vs Local access ratio
        df["remote_ratio"] = df["remote_accesses"] / (df["local_accesses"] + df["remote_accesses"])
        df.groupby("workload")["remote_ratio"].mean().plot(kind="bar", ax=axes[0, 1])
        axes[0, 1].set_title("Remote Memory Access Ratio")
        axes[0, 1].set_ylabel("Ratio")
        
        # Average latency
        df.groupby("workload")["average_latency"].mean().plot(kind="bar", ax=axes[1, 0])
        axes[1, 0].set_title("Average Memory Latency")
        axes[1, 0].set_ylabel("Latency (ns)")
        
        # Bandwidth utilization
        df.groupby("workload")["bandwidth_utilization"].mean().plot(kind="bar", ax=axes[1, 1])
        axes[1, 1].set_title("Bandwidth Utilization")
        axes[1, 1].set_ylabel("Utilization (%)")
        
        plt.tight_layout()
        report_file = self.output_dir / f"profiling_report_{int(time.time())}.png"
        plt.savefig(report_file)
        print(f"Report saved to {report_file}")
        
        # Generate summary statistics
        summary_file = self.output_dir / f"profiling_summary_{int(time.time())}.txt"
        with open(summary_file, 'w') as f:
            f.write("Production Workload Profiling Summary\n")
            f.write("=" * 50 + "\n\n")
            f.write(f"Total configurations tested: {len(self.results)}\n")
            f.write(f"Average execution time: {df['execution_time'].mean():.2f} seconds\n")
            f.write(f"Average remote access ratio: {df['remote_ratio'].mean():.2%}\n")
            f.write(f"Average memory latency: {df['average_latency'].mean():.2f} ns\n")
            f.write(f"Average bandwidth utilization: {df['bandwidth_utilization'].mean():.2f}%\n")
            
        print(f"Summary saved to {summary_file}")


def main():
    parser = argparse.ArgumentParser(description="Production Workload Profiler for CXLMemSim")
    parser.add_argument("--cxlmemsim", required=True, help="Path to CXLMemSim binary")
    parser.add_argument("--config", required=True, help="Path to profiling suite configuration")
    parser.add_argument("--output", default="./profiling_results", help="Output directory")
    
    args = parser.parse_args()
    
    profiler = ProductionProfiler(args.cxlmemsim, args.output)
    profiler.run_production_suite(args.config)


if __name__ == "__main__":
    main()