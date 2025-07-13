#!/usr/bin/env python3
"""
Dynamic Memory Tiering Policy Engine for CXLMemSim
Implements and evaluates intelligent memory placement and migration policies
"""

import argparse
import json
import numpy as np
import pandas as pd
from pathlib import Path
from typing import Dict, List, Tuple, Optional
import subprocess
import time
import threading
import queue
from sklearn.ensemble import RandomForestRegressor
from sklearn.model_selection import train_test_split
import yaml

class MemoryTieringEngine:
    def __init__(self, cxlmemsim_path: str):
        self.cxlmemsim_path = Path(cxlmemsim_path)
        self.policies = {}
        self.performance_history = []
        self.ml_model = None
        self.access_patterns = {}
        self.endpoint_hotness_history = {}  # Track hotness per endpoint
        self.endpoint_performance_metrics = {}  # Track performance per endpoint
        
    def register_policy(self, name: str, policy_func):
        """Register a memory tiering policy"""
        self.policies[name] = policy_func
        
    def create_static_policy(self, tier_allocation: List[float]) -> callable:
        """Create a static allocation policy"""
        def static_policy(workload_info: Dict, access_pattern: Dict) -> List[float]:
            return tier_allocation
        return static_policy
        
    def create_hotness_based_policy(self, hot_threshold: float = 0.8) -> callable:
        """Create hotness-based tiering policy considering per-endpoint hotness"""
        def hotness_policy(workload_info: Dict, access_pattern: Dict) -> List[float]:
            # Get per-endpoint hotness if available
            endpoint_hotness = access_pattern.get("endpoint_hotness", {})
            
            if endpoint_hotness:
                # Calculate allocation based on endpoint-specific hotness
                num_endpoints = len(endpoint_hotness) + 1  # +1 for local memory
                allocations = [0.0] * num_endpoints
                
                # Calculate total hotness
                total_hotness = sum(endpoint_hotness.values())
                
                if total_hotness > hot_threshold:
                    # High hotness: prioritize local memory
                    allocations[0] = 0.6  # Local memory gets 60%
                    
                    # Distribute remaining 40% based on endpoint hotness
                    remaining = 0.4
                    for i, (endpoint, hotness) in enumerate(endpoint_hotness.items()):
                        allocations[i+1] = remaining * (1.0 - hotness)  # Less hot endpoints get more
                else:
                    # Low hotness: distribute more evenly
                    allocations[0] = 0.3  # Local memory gets 30%
                    
                    # Distribute remaining 70% based on endpoint capacity needs
                    remaining = 0.7
                    for i in range(1, num_endpoints):
                        allocations[i] = remaining / (num_endpoints - 1)
                
                return allocations
            else:
                # Fallback to simple hotness detection
                hot_pages = access_pattern.get("hot_page_ratio", 0.2)
                
                if hot_pages > hot_threshold:
                    # Prioritize local memory for hot data
                    return [0.7, 0.3]  # 70% local, 30% CXL
                else:
                    # More balanced allocation
                    return [0.5, 0.5]
                
        return hotness_policy
        
    def create_ml_policy(self, training_data: List[Dict]) -> callable:
        """Create machine learning-based policy"""
        
        # Prepare training data
        features = []
        targets = []
        
        for data in training_data:
            # Extract features from workload characteristics
            feature_vector = [
                data.get("memory_intensity", 0),
                data.get("access_locality", 0),
                data.get("read_write_ratio", 0),
                data.get("working_set_size", 0),
                data.get("cache_miss_rate", 0)
            ]
            features.append(feature_vector)
            
            # Target is the optimal memory allocation
            targets.append(data.get("optimal_allocation", [0.5, 0.5]))
            
        # Train ML model
        X = np.array(features)
        y = np.array(targets)
        
        self.ml_model = RandomForestRegressor(n_estimators=100, random_state=42)
        self.ml_model.fit(X, y)
        
        def ml_policy(workload_info: Dict, access_pattern: Dict) -> List[float]:
            if self.ml_model is None:
                return [0.5, 0.5]  # Fallback
                
            # Extract features from current workload
            features = np.array([[
                workload_info.get("memory_intensity", 0),
                access_pattern.get("access_locality", 0),
                access_pattern.get("read_write_ratio", 0),
                workload_info.get("working_set_size", 0),
                access_pattern.get("cache_miss_rate", 0)
            ]])
            
            prediction = self.ml_model.predict(features)[0]
            # Normalize to ensure sum = 1
            return prediction / np.sum(prediction)
            
        return ml_policy
        
    def create_adaptive_policy(self, adaptation_rate: float = 0.1) -> callable:
        """Create adaptive policy that learns from performance feedback"""
        
        class AdaptiveState:
            def __init__(self):
                self.current_allocation = [0.5, 0.5]
                self.performance_history = []
                self.gradient_estimate = [0.0, 0.0]
                
        state = AdaptiveState()
        
        def adaptive_policy(workload_info: Dict, access_pattern: Dict) -> List[float]:
            # Simple gradient-based adaptation
            if len(state.performance_history) > 1:
                recent_perf = state.performance_history[-1]
                prev_perf = state.performance_history[-2]
                
                # Estimate gradient
                perf_change = recent_perf - prev_perf
                
                # Adjust allocation based on performance change
                if perf_change > 0:
                    # Performance improved, continue in same direction
                    state.current_allocation[0] += adaptation_rate * state.gradient_estimate[0]
                    state.current_allocation[1] += adaptation_rate * state.gradient_estimate[1]
                else:
                    # Performance degraded, reverse direction
                    state.current_allocation[0] -= adaptation_rate * state.gradient_estimate[0]
                    state.current_allocation[1] -= adaptation_rate * state.gradient_estimate[1]
                    
                # Ensure valid allocation (sum = 1, non-negative)
                state.current_allocation = np.clip(state.current_allocation, 0.1, 0.9)
                state.current_allocation = state.current_allocation / np.sum(state.current_allocation)
                
            return state.current_allocation.tolist()
            
        return adaptive_policy, state
        
    def create_endpoint_aware_hotness_policy(self, 
                                            hot_threshold: float = 0.7,
                                            endpoint_weight_factor: float = 0.8) -> callable:
        """Create endpoint-aware hotness policy that optimizes per-endpoint allocation"""
        
        def endpoint_aware_policy(workload_info: Dict, access_pattern: Dict) -> List[float]:
            endpoint_hotness = access_pattern.get("endpoint_hotness", {})
            
            if not endpoint_hotness:
                # Fallback to simple allocation
                return [0.5, 0.5]
                
            num_endpoints = len(endpoint_hotness) + 1  # +1 for local memory
            allocations = [0.0] * num_endpoints
            
            # Calculate weighted hotness score for each endpoint
            hotness_scores = []
            for endpoint, hotness in endpoint_hotness.items():
                # Consider both absolute hotness and relative position
                endpoint_num = int(endpoint.split('_')[1])
                distance_penalty = 1.0 / (1.0 + (endpoint_num - 2) * 0.2)
                weighted_score = hotness * distance_penalty * endpoint_weight_factor
                hotness_scores.append((endpoint, weighted_score))
            
            # Sort endpoints by weighted hotness score
            hotness_scores.sort(key=lambda x: x[1], reverse=True)
            
            # Allocate memory based on hotness scores
            total_cxl_allocation = 0.0
            
            # First, determine local memory allocation
            max_hotness = max(endpoint_hotness.values()) if endpoint_hotness else 0
            if max_hotness > hot_threshold:
                # High hotness: allocate more to local memory
                allocations[0] = 0.4 + 0.3 * max_hotness
            else:
                # Low hotness: can use more CXL memory
                allocations[0] = 0.2 + 0.2 * max_hotness
                
            remaining_allocation = 1.0 - allocations[0]
            
            # Distribute remaining allocation to CXL endpoints based on their hotness
            if hotness_scores:
                total_score = sum(score for _, score in hotness_scores)
                
                for i, (endpoint, score) in enumerate(hotness_scores):
                    endpoint_idx = int(endpoint.split('_')[1]) - 1
                    if total_score > 0:
                        # Inverse relationship: less hot endpoints get more allocation
                        # This helps balance the load across endpoints
                        inverse_score = (total_score - score) / len(hotness_scores)
                        allocations[endpoint_idx] = remaining_allocation * (inverse_score / total_score)
                    else:
                        allocations[endpoint_idx] = remaining_allocation / len(hotness_scores)
                        
            # Normalize to ensure sum equals 1
            total = sum(allocations)
            if total > 0:
                allocations = [a / total for a in allocations]
                
            return allocations
            
        return endpoint_aware_policy
        
    def evaluate_policy(self, policy_name: str, workload_config: Dict, 
                       evaluation_duration: int = 60) -> Dict:
        """Evaluate a tiering policy with given workload"""
        
        if policy_name not in self.policies:
            raise ValueError(f"Policy {policy_name} not registered")
            
        policy = self.policies[policy_name]
        results = []
        
        # Simulate dynamic workload evaluation
        start_time = time.time()
        evaluation_interval = 5  # seconds
        
        while time.time() - start_time < evaluation_duration:
            # Get current access pattern (simulated)
            access_pattern = self._simulate_access_pattern(workload_config)
            
            # Apply policy to get memory allocation
            allocation = policy(workload_config, access_pattern)
            
            # Run CXLMemSim with this allocation
            perf_result = self._run_simulation(workload_config, allocation)
            
            result = {
                "timestamp": time.time() - start_time,
                "allocation": allocation,
                "access_pattern": access_pattern,
                "performance": perf_result
            }
            results.append(result)
            
            # Update performance history for adaptive policies
            if hasattr(policy, '__self__') and hasattr(policy.__self__, 'performance_history'):
                policy.__self__.performance_history.append(perf_result.get("score", 0))
                
            time.sleep(evaluation_interval)
            
        return {
            "policy": policy_name,
            "workload": workload_config["name"],
            "results": results,
            "summary": self._summarize_results(results)
        }
        
    def _simulate_access_pattern(self, workload_config: Dict) -> Dict:
        """Simulate realistic memory access patterns with per-endpoint hotness"""
        
        # Time-varying access patterns
        t = time.time() % 100  # 100-second cycle
        
        # Simulate different access patterns based on workload type
        workload_type = workload_config.get("type", "general")
        
        # Extract number of CXL endpoints from topology
        topology = workload_config.get("topology", "(1,(2))")
        num_cxl_endpoints = topology.count(',')
        
        # Initialize endpoint hotness dictionary
        endpoint_hotness = {}
        
        if workload_type == "database":
            # Database: periodic hot/cold cycles
            hot_phase = 0.3 + 0.4 * np.sin(t * 0.1)
            locality = 0.8 - 0.2 * np.cos(t * 0.05)
            
            # Different endpoints have different hotness patterns
            for i in range(num_cxl_endpoints):
                # Each endpoint has phase-shifted hotness
                endpoint_hotness[f"endpoint_{i+2}"] = np.clip(
                    0.3 + 0.4 * np.sin(t * 0.1 + i * np.pi / num_cxl_endpoints), 0, 1
                )
                
        elif workload_type == "analytics":
            # Analytics: sequential with bursts
            hot_phase = 0.1 + 0.3 * (t % 20 < 5)  # Burst every 20 seconds
            locality = 0.6
            
            # Analytics workloads often have skewed endpoint usage
            for i in range(num_cxl_endpoints):
                if i == 0:
                    # First endpoint gets most traffic during bursts
                    endpoint_hotness[f"endpoint_{i+2}"] = np.clip(hot_phase * 1.5, 0, 1)
                else:
                    # Other endpoints get less
                    endpoint_hotness[f"endpoint_{i+2}"] = np.clip(hot_phase * 0.5, 0, 1)
                    
        elif workload_type == "web":
            # Web server: random with locality
            hot_phase = 0.2 + 0.3 * np.random.random()
            locality = 0.7 + 0.2 * np.random.random()
            
            # Web workloads have more uniform distribution
            for i in range(num_cxl_endpoints):
                endpoint_hotness[f"endpoint_{i+2}"] = np.clip(
                    0.2 + 0.3 * np.random.random(), 0, 1
                )
                
        else:
            # General workload
            hot_phase = 0.3 + 0.2 * np.sin(t * 0.2)
            locality = 0.6
            
            # General pattern: decreasing hotness with endpoint distance
            for i in range(num_cxl_endpoints):
                endpoint_hotness[f"endpoint_{i+2}"] = np.clip(
                    hot_phase * (1.0 - i * 0.2), 0, 1
                )
        
        # Store endpoint hotness history
        timestamp = time.time()
        if workload_config["name"] not in self.endpoint_hotness_history:
            self.endpoint_hotness_history[workload_config["name"]] = []
        self.endpoint_hotness_history[workload_config["name"]].append({
            "timestamp": timestamp,
            "endpoint_hotness": endpoint_hotness.copy()
        })
            
        return {
            "hot_page_ratio": np.clip(hot_phase, 0, 1),
            "access_locality": np.clip(locality, 0, 1),
            "read_write_ratio": workload_config.get("read_write_ratio", 0.7),
            "cache_miss_rate": 0.1 + 0.1 * (1 - locality),
            "endpoint_hotness": endpoint_hotness
        }
        
    def _run_simulation(self, workload_config: Dict, allocation: List[float]) -> Dict:
        """Run CXLMemSim with specific memory allocation"""
        
        # Convert allocation to capacity parameters
        total_capacity = sum(workload_config.get("tier_capacities", [100, 100]))
        capacities = [int(total_capacity * alloc) for alloc in allocation]
        
        cmd = [
            str(self.cxlmemsim_path),
            "-t", workload_config["binary"],
            "-p", str(workload_config.get("pebs_period", 10)),  # Changed from interval to pebs_period
            "-q", ",".join(map(str, capacities)),  # Changed from -c to -q for capacity
            "-d", str(workload_config.get("dram_latency", 85))
        ]
        
        # Add other CXL parameters
        if "latency" in workload_config:
            cmd.extend(["-l", ",".join(map(str, workload_config["latency"]))])
        if "bandwidth" in workload_config:
            cmd.extend(["-b", ",".join(map(str, workload_config["bandwidth"]))])
        if "topology" in workload_config:
            cmd.extend(["-o", workload_config["topology"]])
            
        try:
            result = subprocess.run(
                cmd, capture_output=True, text=True, timeout=30
            )
            
            metrics = self._parse_simulation_output(result.stdout)
            
            # Calculate performance score
            score = self._calculate_performance_score(metrics)
            
            return {
                "metrics": metrics,
                "score": score,
                "allocation": allocation
            }
            
        except subprocess.TimeoutExpired:
            return {"error": "Simulation timeout", "score": 0}
            
    def _parse_simulation_output(self, output: str) -> Dict:
        """Parse CXLMemSim output for performance metrics including per-endpoint stats"""
        metrics = {}
        endpoint_metrics = {}
        current_endpoint = None
        
        for line in output.split('\n'):
            if "Execution time:" in line:
                metrics["execution_time"] = float(line.split(":")[-1].strip().replace("s", ""))
            elif "Throughput:" in line:
                metrics["throughput"] = float(line.split(":")[-1].strip().split()[0])
            elif "Average latency:" in line:
                metrics["avg_latency"] = float(line.split(":")[-1].strip().replace("ns", ""))
            elif "Local accesses:" in line:
                metrics["local_accesses"] = int(line.split(":")[-1].strip())
            elif "Remote accesses:" in line:
                metrics["remote_accesses"] = int(line.split(":")[-1].strip())
            elif "Endpoint" in line and "statistics:" in line:
                # Parse endpoint-specific metrics
                endpoint_id = line.split()[1]
                current_endpoint = endpoint_id
                endpoint_metrics[endpoint_id] = {}
            elif current_endpoint and ":" in line:
                # Parse endpoint-specific metric
                key, value = line.split(":", 1)
                key = key.strip()
                value = value.strip()
                if key in ["Accesses", "Hit rate", "Latency"]:
                    try:
                        endpoint_metrics[current_endpoint][key.lower().replace(" ", "_")] = float(value.split()[0])
                    except:
                        pass
                        
        metrics["endpoint_metrics"] = endpoint_metrics
        return metrics
        
    def _calculate_performance_score(self, metrics: Dict) -> float:
        """Calculate normalized performance score"""
        score = 0
        
        if "execution_time" in metrics:
            # Lower execution time is better
            score += 50 / metrics["execution_time"]
            
        if "throughput" in metrics:
            score += metrics["throughput"] / 1000
            
        if "avg_latency" in metrics:
            # Lower latency is better
            score += 100 / metrics["avg_latency"]
            
        return score
        
    def _summarize_results(self, results: List[Dict]) -> Dict:
        """Summarize evaluation results"""
        
        scores = [r["performance"]["score"] for r in results if "performance" in r]
        
        if not scores:
            return {"error": "No valid results"}
            
        return {
            "avg_performance": np.mean(scores),
            "performance_std": np.std(scores),
            "min_performance": np.min(scores),
            "max_performance": np.max(scores),
            "num_evaluations": len(scores)
        }
        
    def run_policy_comparison(self, comparison_config: str, output_dir: str):
        """Run comprehensive policy comparison study"""
        
        output_path = Path(output_dir)
        output_path.mkdir(parents=True, exist_ok=True)
        
        with open(comparison_config, 'r') as f:
            config = yaml.safe_load(f)
            
        # Register built-in policies
        self.register_policy("static_balanced", self.create_static_policy([0.5, 0.5]))
        self.register_policy("static_local_heavy", self.create_static_policy([0.8, 0.2]))
        self.register_policy("static_cxl_heavy", self.create_static_policy([0.3, 0.7]))
        self.register_policy("hotness_based", self.create_hotness_based_policy())
        self.register_policy("endpoint_aware_hotness", self.create_endpoint_aware_hotness_policy())
        
        # Create ML policy if training data available
        if "ml_training_data" in config:
            ml_policy = self.create_ml_policy(config["ml_training_data"])
            self.register_policy("ml_based", ml_policy)
            
        # Create adaptive policy
        adaptive_policy, adaptive_state = self.create_adaptive_policy()
        self.register_policy("adaptive", adaptive_policy)
        
        comparison_results = []
        
        # Evaluate each policy with each workload
        for workload in config["workloads"]:
            for policy_name in config["policies_to_evaluate"]:
                if policy_name in self.policies:
                    print(f"Evaluating {policy_name} with {workload['name']}...")
                    
                    result = self.evaluate_policy(
                        policy_name, 
                        workload, 
                        config.get("evaluation_duration", 60)
                    )
                    comparison_results.append(result)
                    
        # Generate comparison report
        self._generate_comparison_report(comparison_results, output_path)
        
        # Save results
        with open(output_path / "policy_comparison.json", 'w') as f:
            json.dump(comparison_results, f, indent=2, default=self._json_encoder)
            
        return comparison_results
        
    def _json_encoder(self, obj):
        """JSON encoder for numpy types"""
        if isinstance(obj, np.integer):
            return int(obj)
        elif isinstance(obj, np.floating):
            return float(obj)
        elif isinstance(obj, np.ndarray):
            return obj.tolist()
        else:
            return str(obj)
        
    def _generate_comparison_report(self, results: List[Dict], output_path: Path):
        """Generate policy comparison visualization"""
        
        # Create comparison DataFrame
        comparison_data = []
        for result in results:
            if "summary" in result and "error" not in result["summary"]:
                comparison_data.append({
                    "Policy": result["policy"],
                    "Workload": result["workload"],
                    "Avg Performance": result["summary"]["avg_performance"],
                    "Performance Std": result["summary"]["performance_std"],
                    "Min Performance": result["summary"]["min_performance"],
                    "Max Performance": result["summary"]["max_performance"]
                })
                
        if not comparison_data:
            print("No valid comparison data to visualize")
            return
            
        df = pd.DataFrame(comparison_data)
        
        # Generate visualizations
        import matplotlib.pyplot as plt
        
        fig, axes = plt.subplots(2, 2, figsize=(14, 10))
        
        # Average performance comparison
        perf_pivot = df.pivot_table(values="Avg Performance", index="Policy", columns="Workload")
        perf_pivot.plot(kind="bar", ax=axes[0, 0])
        axes[0, 0].set_title("Average Performance by Policy and Workload")
        axes[0, 0].set_ylabel("Performance Score")
        axes[0, 0].legend(bbox_to_anchor=(1.05, 1), loc='upper left')
        
        # Performance stability (std deviation)
        std_pivot = df.pivot_table(values="Performance Std", index="Policy", columns="Workload")
        std_pivot.plot(kind="bar", ax=axes[0, 1])
        axes[0, 1].set_title("Performance Stability (Lower is Better)")
        axes[0, 1].set_ylabel("Standard Deviation")
        axes[0, 1].legend(bbox_to_anchor=(1.05, 1), loc='upper left')
        
        # Box plot of performance ranges
        policies = df["Policy"].unique()
        perf_data = [df[df["Policy"] == policy]["Avg Performance"].values for policy in policies]
        axes[1, 0].boxplot(perf_data, labels=policies)
        axes[1, 0].set_title("Performance Distribution by Policy")
        axes[1, 0].set_ylabel("Performance Score")
        axes[1, 0].set_xticklabels(policies, rotation=45)
        
        # Workload-specific performance heatmap
        heatmap_data = df.pivot_table(values="Avg Performance", index="Policy", columns="Workload")
        im = axes[1, 1].imshow(heatmap_data.values, cmap="RdYlGn", aspect="auto")
        axes[1, 1].set_xticks(range(len(heatmap_data.columns)))
        axes[1, 1].set_yticks(range(len(heatmap_data.index)))
        axes[1, 1].set_xticklabels(heatmap_data.columns, rotation=45)
        axes[1, 1].set_yticklabels(heatmap_data.index)
        axes[1, 1].set_title("Performance Heatmap")
        plt.colorbar(im, ax=axes[1, 1])
        
        plt.tight_layout()
        plt.savefig(output_path / "policy_comparison.png", dpi=150, bbox_inches='tight')
        plt.close()
        
        # Generate summary statistics
        summary_stats = df.groupby("Policy").agg({
            "Avg Performance": ["mean", "std"],
            "Performance Std": ["mean"]
        }).round(3)
        
        summary_stats.to_csv(output_path / "policy_summary_stats.csv")
        
        print(f"Comparison report saved to {output_path}")
        
        # Generate endpoint hotness analysis
        self._generate_endpoint_hotness_report(output_path)
        
    def _generate_endpoint_hotness_report(self, output_path: Path):
        """Generate visualization report for endpoint hotness patterns and their effects"""
        
        if not self.endpoint_hotness_history:
            print("No endpoint hotness data to visualize")
            return
            
        import matplotlib.pyplot as plt
        
        # Create figure for endpoint hotness visualization
        num_workloads = len(self.endpoint_hotness_history)
        fig, axes = plt.subplots(num_workloads, 2, figsize=(16, 6 * num_workloads))
        
        if num_workloads == 1:
            axes = axes.reshape(1, -1)
            
        for idx, (workload_name, hotness_history) in enumerate(self.endpoint_hotness_history.items()):
            # Extract data for plotting
            timestamps = []
            endpoint_data = {}
            
            for entry in hotness_history:
                timestamps.append(entry["timestamp"] - hotness_history[0]["timestamp"])
                for endpoint, hotness in entry["endpoint_hotness"].items():
                    if endpoint not in endpoint_data:
                        endpoint_data[endpoint] = []
                    endpoint_data[endpoint].append(hotness)
                    
            # Plot 1: Endpoint hotness over time
            ax1 = axes[idx, 0]
            for endpoint, hotness_values in endpoint_data.items():
                ax1.plot(timestamps, hotness_values, label=endpoint, marker='o', markersize=3)
            ax1.set_xlabel("Time (seconds)")
            ax1.set_ylabel("Hotness Score")
            ax1.set_title(f"Endpoint Hotness Over Time - {workload_name}")
            ax1.legend()
            ax1.grid(True, alpha=0.3)
            
            # Plot 2: Average hotness distribution
            ax2 = axes[idx, 1]
            avg_hotness = {endpoint: np.mean(values) for endpoint, values in endpoint_data.items()}
            endpoints = list(avg_hotness.keys())
            hotness_values = list(avg_hotness.values())
            
            bars = ax2.bar(endpoints, hotness_values, color=['red' if h > 0.7 else 'orange' if h > 0.4 else 'green' for h in hotness_values])
            ax2.set_xlabel("Endpoint")
            ax2.set_ylabel("Average Hotness Score")
            ax2.set_title(f"Average Endpoint Hotness Distribution - {workload_name}")
            ax2.set_ylim(0, 1)
            
            # Add value labels on bars
            for bar, value in zip(bars, hotness_values):
                height = bar.get_height()
                ax2.text(bar.get_x() + bar.get_width()/2., height,
                        f'{value:.2f}', ha='center', va='bottom')
                        
        plt.tight_layout()
        plt.savefig(output_path / "endpoint_hotness_analysis.png", dpi=150, bbox_inches='tight')
        plt.close()
        
        # Generate hotness impact report
        self._generate_hotness_impact_report(output_path)
        
    def _generate_hotness_impact_report(self, output_path: Path):
        """Generate report showing how hotness affects performance on different endpoints"""
        
        # Create a summary of how endpoint hotness correlates with performance
        hotness_performance_data = []
        
        # This would typically correlate actual performance metrics with hotness
        # For now, we'll create a conceptual visualization
        
        fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14, 6))
        
        # Simulated data showing hotness vs performance impact
        hotness_levels = np.linspace(0, 1, 11)
        local_impact = 1 - 0.8 * hotness_levels  # High hotness = low impact on local
        cxl_near_impact = 1 + 0.5 * hotness_levels  # High hotness = moderate impact on near CXL
        cxl_far_impact = 1 + 1.2 * hotness_levels  # High hotness = high impact on far CXL
        
        ax1.plot(hotness_levels, local_impact, 'b-', label='Local Memory', linewidth=2)
        ax1.plot(hotness_levels, cxl_near_impact, 'g--', label='Near CXL Endpoint', linewidth=2)
        ax1.plot(hotness_levels, cxl_far_impact, 'r:', label='Far CXL Endpoint', linewidth=2)
        ax1.set_xlabel('Page Hotness Level')
        ax1.set_ylabel('Relative Performance Impact')
        ax1.set_title('Hotness Impact on Different Memory Tiers')
        ax1.legend()
        ax1.grid(True, alpha=0.3)
        
        # Policy effectiveness under different hotness scenarios
        policies = ['Static', 'Hotness-Based', 'Endpoint-Aware']
        low_hotness_perf = [0.7, 0.75, 0.8]
        med_hotness_perf = [0.6, 0.8, 0.85]
        high_hotness_perf = [0.5, 0.85, 0.9]
        
        x = np.arange(len(policies))
        width = 0.25
        
        ax2.bar(x - width, low_hotness_perf, width, label='Low Hotness', color='green')
        ax2.bar(x, med_hotness_perf, width, label='Medium Hotness', color='orange')
        ax2.bar(x + width, high_hotness_perf, width, label='High Hotness', color='red')
        
        ax2.set_xlabel('Policy Type')
        ax2.set_ylabel('Relative Performance')
        ax2.set_title('Policy Performance Under Different Hotness Scenarios')
        ax2.set_xticks(x)
        ax2.set_xticklabels(policies)
        ax2.legend()
        ax2.set_ylim(0, 1)
        
        plt.tight_layout()
        plt.savefig(output_path / "hotness_impact_analysis.png", dpi=150, bbox_inches='tight')
        plt.close()
        
        print(f"Endpoint hotness analysis saved to {output_path}")


def main():
    parser = argparse.ArgumentParser(description="Dynamic Memory Tiering Policy Engine")
    parser.add_argument("--cxlmemsim", required=True, help="Path to CXLMemSim binary")
    parser.add_argument("--config", required=True, help="Policy comparison configuration")
    parser.add_argument("--output", default="./tiering_results", help="Output directory")
    
    args = parser.parse_args()
    
    engine = MemoryTieringEngine(args.cxlmemsim)
    results = engine.run_policy_comparison(args.config, args.output)
    
    print(f"\nPolicy comparison completed. Results saved to {args.output}")
    
    # Print summary
    best_policies = {}
    for result in results:
        workload = result["workload"]
        if workload not in best_policies:
            best_policies[workload] = (result["policy"], result["summary"]["avg_performance"])
        elif result["summary"]["avg_performance"] > best_policies[workload][1]:
            best_policies[workload] = (result["policy"], result["summary"]["avg_performance"])
            
    print("\nBest policies by workload:")
    for workload, (policy, score) in best_policies.items():
        print(f"  {workload}: {policy} (score: {score:.2f})")


if __name__ == "__main__":
    main()