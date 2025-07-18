#!/usr/bin/env python3
"""
Topology-Aware Hotness Prediction and Optimization Engine for CXLMemSim
Implements intelligent topology selection and strategy optimization based on hotness patterns
"""

import argparse
import json
import numpy as np
import pandas as pd
from pathlib import Path
from typing import Dict, List, Tuple, Optional, Any
import subprocess
import time
import yaml
from concurrent.futures import ProcessPoolExecutor, as_completed
from sklearn.ensemble import RandomForestRegressor, GradientBoostingRegressor
from sklearn.neural_network import MLPRegressor
from sklearn.model_selection import train_test_split, cross_val_score
import matplotlib.pyplot as plt
import seaborn as sns
from dataclasses import dataclass
from enum import Enum

class TopologyType(Enum):
    FLAT = "flat"  # (1,(2,3,4,...))
    HIERARCHICAL = "hierarchical"  # (1,((2,3),(4,5)))
    STAR = "star"  # (1,(2,(3,4,5)))
    MESH = "mesh"  # Complex interconnected topology
    CUSTOM = "custom"  # User-defined topology

@dataclass
class TopologyConfig:
    topology_string: str
    topology_type: TopologyType
    num_endpoints: int
    max_hop_distance: int
    avg_latency: float
    bandwidth_distribution: List[float]
    
@dataclass
class HotnessProfile:
    endpoint_hotness: Dict[str, float]
    temporal_pattern: str  # "stable", "bursty", "cyclic", "random"
    spatial_locality: float  # 0-1, higher means more localized access
    hotness_skew: float  # 0-1, higher means more skewed distribution
    
@dataclass
class StrategyRecommendation:
    topology: TopologyConfig
    policy_name: str
    predicted_performance: float
    confidence_score: float
    reasoning: str

class TopologyHotnessOptimizer:
    def __init__(self, cxlmemsim_path: str):
        self.cxlmemsim_path = Path(cxlmemsim_path)
        self.hotness_predictor = None
        self.topology_selector = None
        self.strategy_optimizer = None
        self.evaluation_cache = {}
        self.topology_library = self._build_topology_library()
        
    def _build_topology_library(self) -> Dict[str, TopologyConfig]:
        """Build a library of common CXL topologies"""
        topologies = {
            "flat_2": TopologyConfig(
                topology_string="(1,(2))",
                topology_type=TopologyType.FLAT,
                num_endpoints=2,
                max_hop_distance=1,
                avg_latency=150,
                bandwidth_distribution=[1.0, 0.9]
            ),
            "flat_4": TopologyConfig(
                topology_string="(1,(2,3,4))",
                topology_type=TopologyType.FLAT,
                num_endpoints=4,
                max_hop_distance=1,
                avg_latency=160,
                bandwidth_distribution=[1.0, 0.9, 0.9, 0.9]
            ),
            "hierarchical_4": TopologyConfig(
                topology_string="(1,((2,3),(4)))",
                topology_type=TopologyType.HIERARCHICAL,
                num_endpoints=4,
                max_hop_distance=2,
                avg_latency=180,
                bandwidth_distribution=[1.0, 0.8, 0.8, 0.7]
            ),
            "star_5": TopologyConfig(
                topology_string="(1,(2,(3,4,5)))",
                topology_type=TopologyType.STAR,
                num_endpoints=5,
                max_hop_distance=2,
                avg_latency=190,
                bandwidth_distribution=[1.0, 0.9, 0.7, 0.7, 0.7]
            ),
            "mesh_6": TopologyConfig(
                topology_string="(1,((2,3),(4,(5,6))))",
                topology_type=TopologyType.MESH,
                num_endpoints=6,
                max_hop_distance=3,
                avg_latency=200,
                bandwidth_distribution=[1.0, 0.8, 0.8, 0.7, 0.6, 0.6]
            )
        }
        return topologies
        
    def train_hotness_predictor(self, training_data: List[Dict]) -> None:
        """Train ML model to predict hotness patterns from workload characteristics"""
        features = []
        targets = []
        
        for data in training_data:
            # Extract workload features
            feature_vector = [
                data.get("memory_intensity", 0),
                data.get("access_locality", 0),
                data.get("read_write_ratio", 0),
                data.get("working_set_size", 0),
                data.get("thread_count", 1),
                data.get("access_pattern_type", 0),  # 0: sequential, 1: random, 2: strided
                data.get("temporal_reuse", 0),
                data.get("spatial_reuse", 0)
            ]
            features.append(feature_vector)
            
            # Target is the hotness profile
            hotness_values = list(data.get("hotness_profile", {}).values())
            targets.append(hotness_values)
            
        X = np.array(features)
        y = np.array(targets)
        
        # Train ensemble model for robustness
        self.hotness_predictor = GradientBoostingRegressor(
            n_estimators=100,
            learning_rate=0.1,
            max_depth=5,
            random_state=42
        )
        self.hotness_predictor.fit(X, y)
        
    def predict_hotness_profile(self, workload_info: Dict) -> HotnessProfile:
        """Predict hotness profile for a given workload"""
        if self.hotness_predictor is None:
            # Use heuristic if no trained model
            return self._heuristic_hotness_prediction(workload_info)
            
        features = np.array([[
            workload_info.get("memory_intensity", 0.5),
            workload_info.get("access_locality", 0.5),
            workload_info.get("read_write_ratio", 0.7),
            workload_info.get("working_set_size", 100),
            workload_info.get("thread_count", 1),
            workload_info.get("access_pattern_type", 1),
            workload_info.get("temporal_reuse", 0.5),
            workload_info.get("spatial_reuse", 0.5)
        ]])
        
        predicted_hotness = self.hotness_predictor.predict(features)[0]
        
        # Convert to hotness profile
        endpoint_hotness = {}
        for i, hotness in enumerate(predicted_hotness):
            endpoint_hotness[f"endpoint_{i+2}"] = float(np.clip(hotness, 0, 1))
            
        # Analyze pattern characteristics
        temporal_pattern = self._detect_temporal_pattern(workload_info)
        spatial_locality = workload_info.get("access_locality", 0.5)
        hotness_skew = np.std(predicted_hotness) / (np.mean(predicted_hotness) + 1e-6)
        
        return HotnessProfile(
            endpoint_hotness=endpoint_hotness,
            temporal_pattern=temporal_pattern,
            spatial_locality=spatial_locality,
            hotness_skew=float(hotness_skew)
        )
        
    def _heuristic_hotness_prediction(self, workload_info: Dict) -> HotnessProfile:
        """Heuristic-based hotness prediction when ML model unavailable"""
        workload_type = workload_info.get("type", "general")
        num_endpoints = workload_info.get("num_endpoints", 4)
        
        endpoint_hotness = {}
        
        if workload_type == "database":
            # Database workloads typically have skewed access patterns
            for i in range(num_endpoints - 1):
                hotness = 0.8 * np.exp(-i * 0.5)  # Exponential decay
                endpoint_hotness[f"endpoint_{i+2}"] = float(np.clip(hotness, 0.1, 1.0))
            temporal_pattern = "cyclic"
            spatial_locality = 0.8
            
        elif workload_type == "analytics":
            # Analytics workloads often access data more uniformly
            for i in range(num_endpoints - 1):
                hotness = 0.4 + 0.2 * np.sin(i * np.pi / num_endpoints)
                endpoint_hotness[f"endpoint_{i+2}"] = float(np.clip(hotness, 0.2, 0.8))
            temporal_pattern = "bursty"
            spatial_locality = 0.6
            
        elif workload_type == "web":
            # Web workloads have random access patterns
            for i in range(num_endpoints - 1):
                hotness = 0.3 + 0.3 * np.random.random()
                endpoint_hotness[f"endpoint_{i+2}"] = float(hotness)
            temporal_pattern = "random"
            spatial_locality = 0.5
            
        else:
            # General workload
            for i in range(num_endpoints - 1):
                hotness = 0.5 - 0.1 * i
                endpoint_hotness[f"endpoint_{i+2}"] = float(np.clip(hotness, 0.2, 0.8))
            temporal_pattern = "stable"
            spatial_locality = 0.7
            
        hotness_values = list(endpoint_hotness.values())
        hotness_skew = np.std(hotness_values) / (np.mean(hotness_values) + 1e-6)
        
        return HotnessProfile(
            endpoint_hotness=endpoint_hotness,
            temporal_pattern=temporal_pattern,
            spatial_locality=spatial_locality,
            hotness_skew=float(hotness_skew)
        )
        
    def _detect_temporal_pattern(self, workload_info: Dict) -> str:
        """Detect temporal access pattern from workload characteristics"""
        access_pattern_type = workload_info.get("access_pattern_type", 1)
        temporal_reuse = workload_info.get("temporal_reuse", 0.5)
        
        if temporal_reuse > 0.8:
            return "stable"
        elif temporal_reuse < 0.3:
            return "random"
        elif access_pattern_type == 2:  # strided
            return "cyclic"
        else:
            return "bursty"
            
    def evaluate_topology_performance(self, 
                                    topology: TopologyConfig,
                                    hotness_profile: HotnessProfile,
                                    policy_name: str,
                                    workload_config: Dict) -> Dict:
        """Evaluate performance of a specific topology with given hotness profile"""
        
        # Check cache first
        cache_key = f"{topology.topology_string}_{policy_name}_{hash(str(hotness_profile))}"
        if cache_key in self.evaluation_cache:
            return self.evaluation_cache[cache_key]
            
        # Prepare allocation based on policy and hotness
        allocation = self._compute_allocation(topology, hotness_profile, policy_name)
        
        # Run simulation
        cmd = [
            str(self.cxlmemsim_path),
            "-t", workload_config["binary"],
            "-p", str(workload_config.get("pebs_period", 10)),
            "-q", ",".join(map(str, allocation)),
            "-d", str(workload_config.get("dram_latency", 85)),
            "-o", topology.topology_string
        ]
        
        # Add latency and bandwidth parameters based on topology
        latencies = self._compute_topology_latencies(topology)
        bandwidths = self._compute_topology_bandwidths(topology)
        
        cmd.extend(["-l", ",".join(map(str, latencies))])
        cmd.extend(["-b", ",".join(map(str, bandwidths))])
        
        try:
            result = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
            metrics = self._parse_simulation_output(result.stdout)
            
            # Calculate comprehensive performance score
            score = self._calculate_topology_aware_score(metrics, topology, hotness_profile)
            
            result_dict = {
                "topology": topology.topology_string,
                "policy": policy_name,
                "metrics": metrics,
                "score": score,
                "allocation": allocation
            }
            
            # Cache the result
            self.evaluation_cache[cache_key] = result_dict
            
            return result_dict
            
        except subprocess.TimeoutExpired:
            return {"error": "Simulation timeout", "score": 0}
            
    def _compute_allocation(self, 
                          topology: TopologyConfig,
                          hotness_profile: HotnessProfile,
                          policy_name: str) -> List[float]:
        """Compute memory allocation based on topology, hotness, and policy"""
        
        num_tiers = topology.num_endpoints
        allocation = [0.0] * num_tiers
        
        if policy_name == "hotness_aware_balanced":
            # Balance allocation inversely with hotness
            total_hotness = sum(hotness_profile.endpoint_hotness.values())
            
            # Local memory allocation based on overall hotness
            if total_hotness > 0.7 * (num_tiers - 1):
                allocation[0] = 0.5  # High hotness: prioritize local
            else:
                allocation[0] = 0.3  # Low hotness: use more CXL
                
            # Distribute remaining to CXL endpoints
            remaining = 1.0 - allocation[0]
            for i, (endpoint, hotness) in enumerate(hotness_profile.endpoint_hotness.items()):
                if i + 1 < num_tiers:
                    # Less hot endpoints get more allocation (load balancing)
                    allocation[i + 1] = remaining * (1.0 - hotness) / (num_tiers - 1)
                    
        elif policy_name == "topology_optimized":
            # Consider both hotness and topology distance
            allocation[0] = 0.4  # Base local allocation
            
            remaining = 0.6
            for i in range(1, num_tiers):
                endpoint_key = f"endpoint_{i+1}"
                hotness = hotness_profile.endpoint_hotness.get(endpoint_key, 0.5)
                
                # Factor in bandwidth distribution from topology
                bandwidth_factor = topology.bandwidth_distribution[i]
                
                # Closer endpoints with good bandwidth get more allocation
                allocation[i] = remaining * bandwidth_factor * (1.0 - hotness) / (num_tiers - 1)
                
        elif policy_name == "adaptive_hotness":
            # Adaptive policy based on hotness characteristics
            if hotness_profile.hotness_skew > 0.5:
                # High skew: concentrate on less hot endpoints
                allocation[0] = 0.6
                remaining = 0.4
                
                sorted_endpoints = sorted(
                    hotness_profile.endpoint_hotness.items(),
                    key=lambda x: x[1]
                )
                
                for i, (endpoint, hotness) in enumerate(sorted_endpoints):
                    if i + 1 < num_tiers:
                        allocation[i + 1] = remaining * (1.0 - i / (num_tiers - 1))
            else:
                # Low skew: more uniform distribution
                allocation = [1.0 / num_tiers] * num_tiers
                
        else:
            # Default: static balanced
            allocation = [1.0 / num_tiers] * num_tiers
            
        # Normalize allocation
        total = sum(allocation)
        if total > 0:
            allocation = [a / total for a in allocation]
            
        return allocation
        
    def _compute_topology_latencies(self, topology: TopologyConfig) -> List[float]:
        """Compute latency values based on topology structure"""
        base_latency = topology.avg_latency
        latencies = []
        
        # Scale latencies based on hop distance
        for i in range(topology.num_endpoints - 1):
            hop_distance = min(i + 1, topology.max_hop_distance)
            latency = base_latency * (1.0 + 0.2 * (hop_distance - 1))
            latencies.extend([latency, latency * 1.2])  # Read and write latencies
            
        return latencies
        
    def _compute_topology_bandwidths(self, topology: TopologyConfig) -> List[float]:
        """Compute bandwidth values based on topology structure"""
        base_bandwidth = 30000  # MB/s
        bandwidths = []
        
        for i in range(topology.num_endpoints - 1):
            bandwidth = base_bandwidth * topology.bandwidth_distribution[i + 1]
            bandwidths.extend([bandwidth, bandwidth * 0.8])  # Read and write bandwidths
            
        return bandwidths
        
    def _parse_simulation_output(self, output: str) -> Dict:
        """Parse CXLMemSim output for metrics"""
        metrics = {
            "execution_time": 0.0,
            "throughput": 0.0,
            "avg_latency": 0.0,
            "local_accesses": 0,
            "remote_accesses": 0,
            "endpoint_metrics": {}
        }
        
        current_endpoint = None
        
        for line in output.split('\n'):
            if "Execution time:" in line:
                try:
                    metrics["execution_time"] = float(line.split(":")[-1].strip().replace("s", ""))
                except:
                    pass
            elif "Throughput:" in line:
                try:
                    metrics["throughput"] = float(line.split(":")[-1].strip().split()[0])
                except:
                    pass
            elif "Average latency:" in line:
                try:
                    metrics["avg_latency"] = float(line.split(":")[-1].strip().replace("ns", ""))
                except:
                    pass
            elif "Local accesses:" in line:
                try:
                    metrics["local_accesses"] = int(line.split(":")[-1].strip())
                except:
                    pass
            elif "Remote accesses:" in line:
                try:
                    metrics["remote_accesses"] = int(line.split(":")[-1].strip())
                except:
                    pass
            elif "Endpoint" in line and "statistics:" in line:
                try:
                    endpoint_id = line.split()[1]
                    current_endpoint = endpoint_id
                    metrics["endpoint_metrics"][endpoint_id] = {}
                except:
                    pass
            elif current_endpoint and ":" in line:
                try:
                    key, value = line.split(":", 1)
                    key = key.strip().lower().replace(" ", "_")
                    value = value.strip()
                    if key in ["accesses", "hit_rate", "latency", "bandwidth_utilization"]:
                        metrics["endpoint_metrics"][current_endpoint][key] = float(value.split()[0])
                except:
                    pass
                    
        return metrics
        
    def _calculate_topology_aware_score(self, 
                                      metrics: Dict,
                                      topology: TopologyConfig,
                                      hotness_profile: HotnessProfile) -> float:
        """Calculate performance score considering topology and hotness"""
        base_score = 0.0
        
        # Performance components
        if metrics["execution_time"] > 0:
            base_score += 50 / metrics["execution_time"]
            
        if metrics["throughput"] > 0:
            base_score += metrics["throughput"] / 1000
            
        if metrics["avg_latency"] > 0:
            base_score += 100 / metrics["avg_latency"]
            
        # Topology efficiency bonus
        topology_efficiency = 1.0 / (1.0 + 0.1 * topology.max_hop_distance)
        
        # Hotness alignment bonus
        hotness_alignment = 1.0
        if metrics["endpoint_metrics"]:
            # Check if hot endpoints are being utilized efficiently
            for endpoint, endpoint_metrics in metrics["endpoint_metrics"].items():
                if endpoint in hotness_profile.endpoint_hotness:
                    hotness = hotness_profile.endpoint_hotness[endpoint]
                    utilization = endpoint_metrics.get("bandwidth_utilization", 0) / 100.0
                    
                    # Penalize if hot endpoints have low utilization
                    if hotness > 0.7 and utilization < 0.5:
                        hotness_alignment *= 0.8
                    # Reward if load is well distributed
                    elif hotness < 0.3 and utilization > 0.3:
                        hotness_alignment *= 1.1
                        
        # Combine scores
        final_score = base_score * topology_efficiency * hotness_alignment
        
        return final_score
        
    def recommend_best_configuration(self,
                                   workload_info: Dict,
                                   candidate_topologies: Optional[List[str]] = None,
                                   candidate_policies: Optional[List[str]] = None) -> StrategyRecommendation:
        """Recommend the best topology and policy configuration"""
        
        # Use default candidates if not specified
        if candidate_topologies is None:
            candidate_topologies = list(self.topology_library.keys())
            
        if candidate_policies is None:
            candidate_policies = ["hotness_aware_balanced", "topology_optimized", "adaptive_hotness"]
            
        # Predict hotness profile
        hotness_profile = self.predict_hotness_profile(workload_info)
        
        best_score = -float('inf')
        best_config = None
        
        # Evaluate all combinations
        results = []
        with ProcessPoolExecutor(max_workers=4) as executor:
            futures = []
            
            for topology_name in candidate_topologies:
                topology = self.topology_library[topology_name]
                
                # Skip if topology has fewer endpoints than hotness profile
                if topology.num_endpoints - 1 < len(hotness_profile.endpoint_hotness):
                    continue
                    
                for policy in candidate_policies:
                    future = executor.submit(
                        self.evaluate_topology_performance,
                        topology,
                        hotness_profile,
                        policy,
                        workload_info
                    )
                    futures.append((topology_name, policy, future))
                    
            # Collect results
            for topology_name, policy, future in futures:
                try:
                    result = future.result(timeout=120)
                    if "error" not in result:
                        results.append({
                            "topology": topology_name,
                            "policy": policy,
                            "score": result["score"],
                            "metrics": result["metrics"]
                        })
                        
                        if result["score"] > best_score:
                            best_score = result["score"]
                            best_config = (topology_name, policy, result)
                except:
                    pass
                    
        if best_config is None:
            return StrategyRecommendation(
                topology=self.topology_library["flat_2"],
                policy_name="hotness_aware_balanced",
                predicted_performance=0.0,
                confidence_score=0.0,
                reasoning="Failed to evaluate configurations"
            )
            
        # Generate recommendation
        best_topology_name, best_policy, best_result = best_config
        best_topology = self.topology_library[best_topology_name]
        
        # Calculate confidence based on score distribution
        scores = [r["score"] for r in results]
        confidence = (best_score - np.mean(scores)) / (np.std(scores) + 1e-6)
        confidence = float(np.clip(confidence, 0, 1))
        
        # Generate reasoning
        reasoning = self._generate_recommendation_reasoning(
            best_topology,
            best_policy,
            hotness_profile,
            best_result["metrics"]
        )
        
        return StrategyRecommendation(
            topology=best_topology,
            policy_name=best_policy,
            predicted_performance=best_score,
            confidence_score=confidence,
            reasoning=reasoning
        )
        
    def _generate_recommendation_reasoning(self,
                                         topology: TopologyConfig,
                                         policy: str,
                                         hotness_profile: HotnessProfile,
                                         metrics: Dict) -> str:
        """Generate human-readable reasoning for the recommendation"""
        
        reasons = []
        
        # Topology reasoning
        if topology.topology_type == TopologyType.FLAT:
            reasons.append(f"Flat topology with {topology.num_endpoints} endpoints provides low latency access")
        elif topology.topology_type == TopologyType.HIERARCHICAL:
            reasons.append("Hierarchical topology balances latency and bandwidth efficiency")
        elif topology.topology_type == TopologyType.STAR:
            reasons.append("Star topology optimizes for centralized access patterns")
            
        # Hotness reasoning
        if hotness_profile.hotness_skew > 0.5:
            reasons.append(f"High hotness skew ({hotness_profile.hotness_skew:.2f}) indicates concentrated access patterns")
        else:
            reasons.append(f"Low hotness skew ({hotness_profile.hotness_skew:.2f}) suggests distributed access")
            
        # Policy reasoning
        if policy == "hotness_aware_balanced":
            reasons.append("Hotness-aware balanced policy optimizes for load distribution")
        elif policy == "topology_optimized":
            reasons.append("Topology-optimized policy leverages bandwidth characteristics")
        elif policy == "adaptive_hotness":
            reasons.append("Adaptive policy adjusts to changing hotness patterns")
            
        # Performance reasoning
        if metrics.get("local_accesses", 0) > metrics.get("remote_accesses", 0):
            reasons.append("Configuration achieves high local memory hit rate")
        
        if metrics.get("avg_latency", float('inf')) < 200:
            reasons.append(f"Low average latency ({metrics.get('avg_latency', 0):.1f}ns) ensures responsive performance")
            
        return " ".join(reasons)
        
    def generate_optimization_report(self,
                                   workload_configs: List[Dict],
                                   output_dir: Path) -> None:
        """Generate comprehensive optimization report"""
        
        output_dir.mkdir(parents=True, exist_ok=True)
        
        all_recommendations = []
        all_evaluations = []
        
        for workload in workload_configs:
            print(f"Analyzing workload: {workload['name']}")
            
            # Get recommendation
            recommendation = self.recommend_best_configuration(workload)
            all_recommendations.append({
                "workload": workload["name"],
                "recommendation": recommendation
            })
            
            # Evaluate multiple configurations for comparison
            hotness_profile = self.predict_hotness_profile(workload)
            
            for topology_name, topology in self.topology_library.items():
                if topology.num_endpoints - 1 < len(hotness_profile.endpoint_hotness):
                    continue
                    
                for policy in ["hotness_aware_balanced", "topology_optimized", "adaptive_hotness"]:
                    result = self.evaluate_topology_performance(
                        topology,
                        hotness_profile,
                        policy,
                        workload
                    )
                    
                    if "error" not in result:
                        all_evaluations.append({
                            "workload": workload["name"],
                            "topology": topology_name,
                            "policy": policy,
                            "score": result["score"],
                            "metrics": result["metrics"]
                        })
                        
        # Generate visualizations
        self._generate_performance_heatmap(all_evaluations, output_dir)
        self._generate_topology_comparison(all_evaluations, output_dir)
        self._generate_recommendation_summary(all_recommendations, output_dir)
        
        # Save detailed results
        with open(output_dir / "optimization_results.json", 'w') as f:
            json.dump({
                "recommendations": all_recommendations,
                "evaluations": all_evaluations
            }, f, indent=2, default=str)
            
    def _generate_performance_heatmap(self, evaluations: List[Dict], output_dir: Path) -> None:
        """Generate performance heatmap across topologies and policies"""
        
        # Create DataFrame for heatmap
        data_for_heatmap = []
        for eval_result in evaluations:
            data_for_heatmap.append({
                "Workload": eval_result["workload"],
                "Topology": eval_result["topology"],
                "Policy": eval_result["policy"],
                "Score": eval_result["score"]
            })
            
        df = pd.DataFrame(data_for_heatmap)
        
        # Create heatmap for each workload
        workloads = df["Workload"].unique()
        
        fig, axes = plt.subplots(len(workloads), 1, figsize=(12, 6 * len(workloads)))
        if len(workloads) == 1:
            axes = [axes]
            
        for idx, workload in enumerate(workloads):
            workload_df = df[df["Workload"] == workload]
            pivot_df = workload_df.pivot(index="Topology", columns="Policy", values="Score")
            
            sns.heatmap(pivot_df, annot=True, fmt=".1f", cmap="RdYlGn", ax=axes[idx])
            axes[idx].set_title(f"Performance Scores - {workload}")
            
        plt.tight_layout()
        plt.savefig(output_dir / "performance_heatmap.png", dpi=150, bbox_inches='tight')
        plt.close()
        
    def _generate_topology_comparison(self, evaluations: List[Dict], output_dir: Path) -> None:
        """Generate topology performance comparison"""
        
        df = pd.DataFrame(evaluations)
        
        # Group by topology and calculate average performance
        topology_perf = df.groupby("topology")["score"].agg(['mean', 'std', 'max'])
        
        fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14, 6))
        
        # Bar plot of average performance
        topology_perf['mean'].plot(kind='bar', ax=ax1, color='skyblue')
        ax1.set_title("Average Performance by Topology")
        ax1.set_xlabel("Topology")
        ax1.set_ylabel("Average Score")
        ax1.set_xticklabels(ax1.get_xticklabels(), rotation=45)
        
        # Error bars for stability
        x_pos = range(len(topology_perf))
        ax1.errorbar(x_pos, topology_perf['mean'], yerr=topology_perf['std'], 
                    fmt='none', color='black', capsize=5)
        
        # Box plot by workload
        workload_colors = plt.cm.Set3(np.linspace(0, 1, len(df["Workload"].unique())))
        
        for i, workload in enumerate(df["Workload"].unique()):
            workload_data = df[df["Workload"] == workload]
            topology_scores = [workload_data[workload_data["topology"] == t]["score"].values 
                             for t in topology_perf.index]
            
            positions = [x + (i - 1) * 0.2 for x in range(len(topology_perf))]
            bp = ax2.boxplot(topology_scores, positions=positions, widths=0.15,
                           patch_artist=True, label=workload)
            
            for patch in bp['boxes']:
                patch.set_facecolor(workload_colors[i])
                
        ax2.set_title("Performance Distribution by Topology and Workload")
        ax2.set_xlabel("Topology")
        ax2.set_ylabel("Performance Score")
        ax2.set_xticks(range(len(topology_perf)))
        ax2.set_xticklabels(topology_perf.index, rotation=45)
        ax2.legend()
        
        plt.tight_layout()
        plt.savefig(output_dir / "topology_comparison.png", dpi=150, bbox_inches='tight')
        plt.close()
        
    def _generate_recommendation_summary(self, recommendations: List[Dict], output_dir: Path) -> None:
        """Generate summary of recommendations"""
        
        # Create summary table
        summary_data = []
        for rec in recommendations:
            recommendation = rec["recommendation"]
            summary_data.append({
                "Workload": rec["workload"],
                "Recommended Topology": recommendation.topology.topology_string,
                "Recommended Policy": recommendation.policy_name,
                "Predicted Performance": f"{recommendation.predicted_performance:.2f}",
                "Confidence": f"{recommendation.confidence_score:.2%}",
                "Key Reasoning": recommendation.reasoning.split('.')[0] + "."
            })
            
        summary_df = pd.DataFrame(summary_data)
        
        # Create figure with table
        fig, ax = plt.subplots(figsize=(14, len(summary_df) * 0.5 + 2))
        ax.axis('tight')
        ax.axis('off')
        
        # Create table
        table = ax.table(cellText=summary_df.values,
                        colLabels=summary_df.columns,
                        cellLoc='left',
                        loc='center')
        
        table.auto_set_font_size(False)
        table.set_fontsize(10)
        table.scale(1.2, 1.5)
        
        # Style header
        for i in range(len(summary_df.columns)):
            table[(0, i)].set_facecolor('#40466e')
            table[(0, i)].set_text_props(weight='bold', color='white')
            
        # Alternate row colors
        for i in range(1, len(summary_df) + 1):
            for j in range(len(summary_df.columns)):
                if i % 2 == 0:
                    table[(i, j)].set_facecolor('#f0f0f0')
                    
        plt.title("Topology and Strategy Recommendations", fontsize=16, fontweight='bold', pad=20)
        plt.savefig(output_dir / "recommendation_summary.png", dpi=150, bbox_inches='tight')
        plt.close()
        
        # Also save as CSV
        summary_df.to_csv(output_dir / "recommendations.csv", index=False)


def main():
    parser = argparse.ArgumentParser(
        description="Topology-Aware Hotness Prediction and Optimization for CXL Memory"
    )
    parser.add_argument("--cxlmemsim", required=True, help="Path to CXLMemSim binary")
    parser.add_argument("--config", required=True, help="Configuration file for optimization")
    parser.add_argument("--output", default="./topology_optimization_results", 
                       help="Output directory for results")
    parser.add_argument("--train-model", help="Training data file for ML model")
    
    args = parser.parse_args()
    
    # Initialize optimizer
    optimizer = TopologyHotnessOptimizer(args.cxlmemsim)
    
    # Train ML model if data provided
    if args.train_model:
        with open(args.train_model, 'r') as f:
            training_data = yaml.safe_load(f)
        optimizer.train_hotness_predictor(training_data)
        print("Trained hotness prediction model")
        
    # Load configuration
    with open(args.config, 'r') as f:
        config = yaml.safe_load(f)
        
    # Run optimization
    print("Starting topology and strategy optimization...")
    optimizer.generate_optimization_report(config["workloads"], Path(args.output))
    
    print(f"\nOptimization completed. Results saved to {args.output}")
    
    # Print best configurations
    with open(Path(args.output) / "optimization_results.json", 'r') as f:
        results = json.load(f)
        
    print("\nBest configurations by workload:")
    for rec in results["recommendations"]:
        recommendation = rec["recommendation"]
        print(f"\n{rec['workload']}:")
        print(f"  Topology: {recommendation['topology']['topology_string']}")
        print(f"  Policy: {recommendation['policy_name']}")
        print(f"  Score: {recommendation['predicted_performance']:.2f}")
        print(f"  Confidence: {recommendation['confidence_score']:.2%}")


if __name__ == "__main__":
    main()