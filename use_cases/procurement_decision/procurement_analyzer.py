#!/usr/bin/env python3
"""
Hardware Procurement Decision Support Tool for CXL Memory
Helps evaluate cost/performance tradeoffs for different CXL configurations
"""

import argparse
import json
import numpy as np
import pandas as pd
import matplotlib.pyplot as plt
from pathlib import Path
from typing import Dict, List, Tuple
import subprocess
import concurrent.futures
import yaml

class ProcurementAnalyzer:
    def __init__(self, cxlmemsim_path: str, gem5_calibration_file: str = None):
        self.cxlmemsim_path = Path(cxlmemsim_path)
        self.calibration_data = None
        
        if gem5_calibration_file:
            with open(gem5_calibration_file, 'r') as f:
                self.calibration_data = json.load(f)
                
    def evaluate_hardware_config(self, hw_config: Dict, workload: Dict) -> Dict:
        """Evaluate a specific hardware configuration with given workload"""
        
        # Build CXLMemSim command based on hardware specifications
        cmd = [
            str(self.cxlmemsim_path),
            "-t", workload["binary"],
            "-p", str(workload.get("pebs_period", 10)),  # Changed from interval to pebs_period
            "-c", workload.get("cpuset", "0,2"),
            "-d", str(hw_config.get("local_dram_latency", 85))
        ]
        
        # Convert hardware specs to CXLMemSim parameters
        if "cxl_latency_ns" in hw_config:
            read_lat = hw_config["cxl_latency_ns"]
            write_lat = hw_config.get("cxl_write_latency_ns", read_lat * 1.2)
            cmd.extend(["-l", f"{read_lat},{write_lat}"])
            
        if "cxl_bandwidth_gbps" in hw_config:
            read_bw = hw_config["cxl_bandwidth_gbps"] * 1000  # Convert to MB/s
            write_bw = hw_config.get("cxl_write_bandwidth_gbps", hw_config["cxl_bandwidth_gbps"]) * 1000
            cmd.extend(["-b", f"{read_bw},{write_bw}"])
            
        if "memory_distribution" in hw_config:
            cmd.extend(["-q", ",".join(map(str, hw_config["memory_distribution"]))])  # Changed from -c to -q
            
        if "topology" in hw_config:
            cmd.extend(["-o", hw_config["topology"]])
            
        # Run simulation
        try:
            result = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
            metrics = self._parse_metrics(result.stdout)
            
            # Calculate performance score
            perf_score = self._calculate_performance_score(metrics, workload)
            
            # Calculate cost
            total_cost = self._calculate_hardware_cost(hw_config)
            
            # Calculate power consumption estimate
            power_estimate = self._estimate_power_consumption(hw_config, metrics)
            
            return {
                "config": hw_config,
                "metrics": metrics,
                "performance_score": perf_score,
                "total_cost": total_cost,
                "power_estimate": power_estimate,
                "cost_per_performance": total_cost / perf_score if perf_score > 0 else float('inf')
            }
            
        except subprocess.TimeoutExpired:
            return {
                "config": hw_config,
                "error": "Simulation timeout",
                "total_cost": self._calculate_hardware_cost(hw_config)
            }
    
    def _parse_metrics(self, output: str) -> Dict:
        """Parse CXLMemSim output for metrics"""
        metrics = {}
        for line in output.split('\n'):
            if "Execution time:" in line:
                metrics["execution_time"] = float(line.split(":")[-1].strip().replace("s", ""))
            elif "Average latency:" in line:
                metrics["avg_latency"] = float(line.split(":")[-1].strip().replace("ns", ""))
            elif "Throughput:" in line:
                metrics["throughput"] = float(line.split(":")[-1].strip().split()[0])
            elif "Remote accesses:" in line:
                metrics["remote_accesses"] = int(line.split(":")[-1].strip())
                
        return metrics
    
    def _calculate_performance_score(self, metrics: Dict, workload: Dict) -> float:
        """Calculate normalized performance score"""
        # Weighted combination of metrics
        weights = workload.get("metric_weights", {
            "execution_time": 0.4,
            "throughput": 0.3,
            "latency": 0.3
        })
        
        score = 0
        if "execution_time" in metrics:
            # Lower execution time is better
            score += weights["execution_time"] * (1.0 / metrics["execution_time"])
        if "throughput" in metrics:
            score += weights["throughput"] * metrics["throughput"]
        if "avg_latency" in metrics:
            # Lower latency is better
            score += weights["latency"] * (100.0 / metrics["avg_latency"])
            
        return score
    
    def _calculate_hardware_cost(self, hw_config: Dict) -> float:
        """Calculate total hardware cost based on configuration"""
        cost = 0
        
        # Base system cost
        cost += hw_config.get("base_system_cost", 5000)
        
        # DRAM cost
        local_memory_gb = hw_config.get("local_memory_gb", 128)
        cost += local_memory_gb * hw_config.get("dram_cost_per_gb", 8)
        
        # CXL memory cost
        cxl_memory_gb = hw_config.get("cxl_memory_gb", 0)
        cost += cxl_memory_gb * hw_config.get("cxl_cost_per_gb", 5)
        
        # CXL controller/device cost
        if cxl_memory_gb > 0:
            cost += hw_config.get("cxl_device_cost", 500)
            
        # Switch cost if using switched topology
        if "switch" in hw_config.get("topology", ""):
            cost += hw_config.get("cxl_switch_cost", 2000)
            
        return cost
    
    def _estimate_power_consumption(self, hw_config: Dict, metrics: Dict) -> float:
        """Estimate power consumption in watts"""
        power = 0
        
        # Base system power
        power += hw_config.get("base_power", 200)
        
        # DRAM power (active)
        local_memory_gb = hw_config.get("local_memory_gb", 128)
        power += local_memory_gb * 0.375  # ~3W per 8GB
        
        # CXL memory power
        cxl_memory_gb = hw_config.get("cxl_memory_gb", 0)
        if cxl_memory_gb > 0:
            # CXL has additional controller overhead
            power += cxl_memory_gb * 0.3 + 10  # Controller overhead
            
        # Dynamic power based on utilization
        if "remote_accesses" in metrics:
            utilization = min(metrics["remote_accesses"] / 1000000, 1.0)
            power += utilization * 20  # Up to 20W for high CXL traffic
            
        return power
    
    def run_procurement_analysis(self, config_file: str, output_dir: str):
        """Run complete procurement analysis"""
        
        output_path = Path(output_dir)
        output_path.mkdir(parents=True, exist_ok=True)
        
        with open(config_file, 'r') as f:
            config = yaml.safe_load(f)
            
        hardware_configs = config["hardware_configurations"]
        workloads = config["workloads"]
        
        results = []
        
        # Evaluate each hardware configuration with each workload
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as executor:
            futures = []
            
            for hw_config in hardware_configs:
                for workload in workloads:
                    future = executor.submit(self.evaluate_hardware_config, hw_config, workload)
                    futures.append((hw_config["name"], workload["name"], future))
                    
            for hw_name, wl_name, future in futures:
                result = future.result()
                result["hardware_name"] = hw_name
                result["workload_name"] = wl_name
                results.append(result)
                
        # Generate procurement report
        self._generate_procurement_report(results, output_path)
        
        # Generate TCO analysis
        self._generate_tco_analysis(results, config.get("tco_parameters", {}), output_path)
        
        # Generate recommendation
        recommendation = self._generate_recommendation(results, config.get("requirements", {}))
        
        # Save all results
        with open(output_path / "procurement_analysis.json", 'w') as f:
            json.dump({
                "results": results,
                "recommendation": recommendation,
                "timestamp": pd.Timestamp.now().isoformat()
            }, f, indent=2)
            
        return recommendation
    
    def _generate_procurement_report(self, results: List[Dict], output_dir: Path):
        """Generate visual procurement analysis report"""
        
        # Filter out errored results
        valid_results = [r for r in results if "error" not in r]
        
        if not valid_results:
            print("No valid results to generate report")
            return
            
        # Create DataFrame
        df = pd.DataFrame([{
            "Hardware": r["hardware_name"],
            "Workload": r["workload_name"],
            "Performance": r["performance_score"],
            "Cost": r["total_cost"],
            "Cost/Perf": r["cost_per_performance"],
            "Power": r["power_estimate"]
        } for r in valid_results])
        
        # Generate visualizations
        fig, axes = plt.subplots(2, 2, figsize=(14, 10))
        
        # Cost vs Performance scatter
        for hw in df["Hardware"].unique():
            hw_data = df[df["Hardware"] == hw]
            axes[0, 0].scatter(hw_data["Cost"], hw_data["Performance"], label=hw, s=100)
        axes[0, 0].set_xlabel("Total Cost ($)")
        axes[0, 0].set_ylabel("Performance Score")
        axes[0, 0].set_title("Cost vs Performance Analysis")
        axes[0, 0].legend()
        axes[0, 0].grid(True, alpha=0.3)
        
        # Cost/Performance ratio comparison
        cost_perf_avg = df.groupby("Hardware")["Cost/Perf"].mean().sort_values()
        cost_perf_avg.plot(kind="barh", ax=axes[0, 1])
        axes[0, 1].set_xlabel("Cost per Performance Unit")
        axes[0, 1].set_title("Cost Efficiency Comparison")
        axes[0, 1].grid(True, alpha=0.3)
        
        # Power consumption comparison
        power_avg = df.groupby("Hardware")["Power"].mean().sort_values()
        power_avg.plot(kind="bar", ax=axes[1, 0], color="orange")
        axes[1, 0].set_ylabel("Power Consumption (W)")
        axes[1, 0].set_title("Average Power Consumption")
        axes[1, 0].grid(True, alpha=0.3)
        
        # Performance by workload heatmap
        perf_pivot = df.pivot_table(values="Performance", index="Hardware", columns="Workload")
        im = axes[1, 1].imshow(perf_pivot.values, cmap="YlOrRd", aspect="auto")
        axes[1, 1].set_xticks(range(len(perf_pivot.columns)))
        axes[1, 1].set_yticks(range(len(perf_pivot.index)))
        axes[1, 1].set_xticklabels(perf_pivot.columns, rotation=45)
        axes[1, 1].set_yticklabels(perf_pivot.index)
        axes[1, 1].set_title("Performance Heatmap")
        plt.colorbar(im, ax=axes[1, 1])
        
        plt.tight_layout()
        plt.savefig(output_dir / "procurement_analysis.png", dpi=150)
        plt.close()
        
    def _generate_tco_analysis(self, results: List[Dict], tco_params: Dict, output_dir: Path):
        """Generate Total Cost of Ownership analysis"""
        
        years = tco_params.get("years", 3)
        electricity_cost = tco_params.get("electricity_cost_per_kwh", 0.12)
        
        tco_data = []
        
        for hw_name in set(r["hardware_name"] for r in results if "error" not in r):
            hw_results = [r for r in results if r.get("hardware_name") == hw_name and "error" not in r]
            
            if not hw_results:
                continue
                
            # Initial hardware cost
            initial_cost = hw_results[0]["total_cost"]
            
            # Average power consumption
            avg_power = np.mean([r["power_estimate"] for r in hw_results])
            
            # Annual electricity cost
            annual_electricity = avg_power * 24 * 365 * electricity_cost / 1000
            
            # Maintenance cost estimate (2% of hardware cost)
            annual_maintenance = initial_cost * 0.02
            
            # Calculate TCO over years
            tco = initial_cost + (annual_electricity + annual_maintenance) * years
            
            tco_data.append({
                "Hardware": hw_name,
                "Initial Cost": initial_cost,
                "Annual Electricity": annual_electricity,
                "Annual Maintenance": annual_maintenance,
                f"{years}-Year TCO": tco
            })
            
        # Create TCO comparison chart
        tco_df = pd.DataFrame(tco_data)
        
        fig, ax = plt.subplots(figsize=(10, 6))
        
        x = np.arange(len(tco_df))
        width = 0.2
        
        ax.bar(x - width*1.5, tco_df["Initial Cost"], width, label="Initial Cost")
        ax.bar(x - width/2, tco_df["Annual Electricity"]*years, width, label=f"{years}Y Electricity")
        ax.bar(x + width/2, tco_df["Annual Maintenance"]*years, width, label=f"{years}Y Maintenance")
        ax.bar(x + width*1.5, tco_df[f"{years}-Year TCO"], width, label=f"{years}-Year TCO", alpha=0.7)
        
        ax.set_xlabel("Hardware Configuration")
        ax.set_ylabel("Cost ($)")
        ax.set_title(f"{years}-Year Total Cost of Ownership Analysis")
        ax.set_xticks(x)
        ax.set_xticklabels(tco_df["Hardware"], rotation=45)
        ax.legend()
        ax.grid(True, alpha=0.3)
        
        plt.tight_layout()
        plt.savefig(output_dir / "tco_analysis.png", dpi=150)
        plt.close()
        
        # Save TCO data
        tco_df.to_csv(output_dir / "tco_analysis.csv", index=False)
        
    def _generate_recommendation(self, results: List[Dict], requirements: Dict) -> Dict:
        """Generate procurement recommendation based on requirements"""
        
        valid_results = [r for r in results if "error" not in r]
        
        if not valid_results:
            return {"error": "No valid configurations found"}
            
        # Apply requirement filters
        filtered_results = valid_results
        
        if "max_budget" in requirements:
            filtered_results = [r for r in filtered_results if r["total_cost"] <= requirements["max_budget"]]
            
        if "min_performance" in requirements:
            filtered_results = [r for r in filtered_results if r["performance_score"] >= requirements["min_performance"]]
            
        if "max_power" in requirements:
            filtered_results = [r for r in filtered_results if r["power_estimate"] <= requirements["max_power"]]
            
        if not filtered_results:
            return {
                "recommendation": "No configuration meets all requirements",
                "suggestion": "Consider relaxing constraints or exploring custom configurations"
            }
            
        # Score configurations
        scores = []
        for result in filtered_results:
            # Multi-criteria scoring
            perf_norm = result["performance_score"] / max(r["performance_score"] for r in filtered_results)
            cost_norm = 1 - (result["total_cost"] / max(r["total_cost"] for r in filtered_results))
            power_norm = 1 - (result["power_estimate"] / max(r["power_estimate"] for r in filtered_results))
            
            weights = requirements.get("optimization_weights", {
                "performance": 0.4,
                "cost": 0.4,
                "power": 0.2
            })
            
            total_score = (
                weights.get("performance", 0.4) * perf_norm +
                weights.get("cost", 0.4) * cost_norm +
                weights.get("power", 0.2) * power_norm
            )
            
            scores.append((total_score, result))
            
        # Sort by score
        scores.sort(reverse=True, key=lambda x: x[0])
        
        best_config = scores[0][1]
        
        return {
            "recommended_configuration": best_config["hardware_name"],
            "reasoning": {
                "performance_score": best_config["performance_score"],
                "total_cost": best_config["total_cost"],
                "cost_per_performance": best_config["cost_per_performance"],
                "power_consumption": best_config["power_estimate"],
                "meets_requirements": True
            },
            "alternatives": [
                {
                    "name": s[1]["hardware_name"],
                    "score": s[0],
                    "tradeoff": self._describe_tradeoff(s[1], best_config)
                }
                for s in scores[1:4]  # Top 3 alternatives
            ]
        }
    
    def _describe_tradeoff(self, alt_config: Dict, best_config: Dict) -> str:
        """Describe tradeoff between alternative and best configuration"""
        
        perf_diff = (alt_config["performance_score"] - best_config["performance_score"]) / best_config["performance_score"] * 100
        cost_diff = (alt_config["total_cost"] - best_config["total_cost"]) / best_config["total_cost"] * 100
        
        if cost_diff < 0 and perf_diff < 0:
            return f"{abs(cost_diff):.1f}% cheaper but {abs(perf_diff):.1f}% slower"
        elif cost_diff > 0 and perf_diff > 0:
            return f"{perf_diff:.1f}% faster but {cost_diff:.1f}% more expensive"
        else:
            return f"Different optimization balance"


def main():
    parser = argparse.ArgumentParser(description="CXL Hardware Procurement Decision Support")
    parser.add_argument("--cxlmemsim", required=True, help="Path to CXLMemSim binary")
    parser.add_argument("--config", required=True, help="Procurement analysis configuration")
    parser.add_argument("--output", default="./procurement_results", help="Output directory")
    parser.add_argument("--calibration", help="gem5 calibration data file")
    
    args = parser.parse_args()
    
    analyzer = ProcurementAnalyzer(args.cxlmemsim, args.calibration)
    recommendation = analyzer.run_procurement_analysis(args.config, args.output)
    
    print("\nProcurement Recommendation:")
    print(json.dumps(recommendation, indent=2))


if __name__ == "__main__":
    main()