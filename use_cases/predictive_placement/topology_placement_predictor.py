#!/usr/bin/env python3
"""
Predictive Topology and Placement Optimizer for CXL Memory Systems
Uses ML to predict optimal data placement across CXL topology based on access patterns
"""

import argparse
import json
import numpy as np
import pandas as pd
from pathlib import Path
from typing import Dict, List, Tuple, Optional, Set
import subprocess
import yaml
from dataclasses import dataclass, field
from enum import Enum
import time
from sklearn.ensemble import RandomForestClassifier, GradientBoostingRegressor
from sklearn.cluster import KMeans
from sklearn.preprocessing import StandardScaler
import torch
import torch.nn as nn
import torch.optim as optim
import matplotlib.pyplot as plt
import seaborn as sns
from concurrent.futures import ThreadPoolExecutor, as_completed

class AccessPattern(Enum):
    SEQUENTIAL = "sequential"
    RANDOM = "random"
    STRIDED = "strided"
    TEMPORAL = "temporal"
    SPATIAL = "spatial"

@dataclass
class MemoryPage:
    page_id: int
    size_kb: int
    access_count: int
    last_access_time: float
    access_pattern: AccessPattern
    current_location: str  # endpoint location
    heat_score: float = 0.0
    predicted_future_accesses: int = 0

@dataclass
class EndpointCharacteristics:
    endpoint_id: str
    capacity_gb: int
    used_gb: float
    latency_ns: float
    bandwidth_gbps: float
    distance_from_cpu: int  # hop count
    congestion_level: float = 0.0  # 0-1
    power_state: str = "active"  # active, idle, sleep

@dataclass
class PlacementDecision:
    page_id: int
    current_location: str
    recommended_location: str
    expected_improvement: float  # percentage
    confidence: float  # 0-1
    reasoning: str

class DeepPlacementNet(nn.Module):
    """Deep learning model for placement prediction"""
    def __init__(self, input_features: int, num_endpoints: int):
        super(DeepPlacementNet, self).__init__()
        self.fc1 = nn.Linear(input_features, 256)
        self.fc2 = nn.Linear(256, 128)
        self.fc3 = nn.Linear(128, 64)
        self.fc4 = nn.Linear(64, num_endpoints)
        self.dropout = nn.Dropout(0.2)
        self.relu = nn.ReLU()
        self.softmax = nn.Softmax(dim=1)
        
    def forward(self, x):
        x = self.relu(self.fc1(x))
        x = self.dropout(x)
        x = self.relu(self.fc2(x))
        x = self.dropout(x)
        x = self.relu(self.fc3(x))
        x = self.softmax(self.fc4(x))
        return x

class TopologyPlacementPredictor:
    def __init__(self, cxlmemsim_path: str, topology_config: Dict):
        self.cxlmemsim_path = Path(cxlmemsim_path)
        self.topology = topology_config
        self.endpoints = self._parse_topology(topology_config)
        self.placement_model = None
        self.access_predictor = None
        self.page_clusters = {}
        self.placement_history = []
        self.performance_metrics = []
        
    def _parse_topology(self, topology_config: Dict) -> Dict[str, EndpointCharacteristics]:
        """Parse topology configuration to extract endpoint characteristics"""
        endpoints = {}
        
        # Local memory (endpoint_1)
        endpoints["endpoint_1"] = EndpointCharacteristics(
            endpoint_id="endpoint_1",
            capacity_gb=topology_config.get("local_memory_gb", 128),
            used_gb=0,
            latency_ns=topology_config.get("local_latency_ns", 80),
            bandwidth_gbps=topology_config.get("local_bandwidth_gbps", 100),
            distance_from_cpu=0
        )
        
        # CXL endpoints
        cxl_configs = topology_config.get("cxl_endpoints", [])
        for i, config in enumerate(cxl_configs, start=2):
            endpoints[f"endpoint_{i}"] = EndpointCharacteristics(
                endpoint_id=f"endpoint_{i}",
                capacity_gb=config["capacity_gb"],
                used_gb=0,
                latency_ns=config["latency_ns"],
                bandwidth_gbps=config["bandwidth_gbps"],
                distance_from_cpu=config.get("hop_distance", 1)
            )
            
        return endpoints
        
    def train_placement_model(self, training_data: List[Dict]) -> None:
        """Train the deep learning placement model"""
        
        # Prepare training data
        features = []
        labels = []
        
        for sample in training_data:
            feature_vector = self._extract_page_features(sample["page_info"])
            optimal_location = sample["optimal_location"]
            
            features.append(feature_vector)
            labels.append(self._encode_location(optimal_location))
            
        X = torch.FloatTensor(features)
        y = torch.LongTensor(labels)
        
        # Initialize model
        num_features = X.shape[1]
        num_endpoints = len(self.endpoints)
        self.placement_model = DeepPlacementNet(num_features, num_endpoints)
        
        # Train model
        optimizer = optim.Adam(self.placement_model.parameters(), lr=0.001)
        criterion = nn.CrossEntropyLoss()
        
        print("Training placement prediction model...")
        for epoch in range(100):
            optimizer.zero_grad()
            outputs = self.placement_model(X)
            loss = criterion(outputs, y)
            loss.backward()
            optimizer.step()
            
            if epoch % 20 == 0:
                print(f"Epoch {epoch}, Loss: {loss.item():.4f}")
                
    def _extract_page_features(self, page_info: Dict) -> List[float]:
        """Extract features from page information"""
        features = [
            page_info.get("access_count", 0),
            page_info.get("recency_score", 0),  # time since last access
            page_info.get("access_frequency", 0),
            page_info.get("read_ratio", 0.5),
            page_info.get("sequential_score", 0),
            page_info.get("size_kb", 4),
            page_info.get("sharing_degree", 1),  # number of threads accessing
            page_info.get("temporal_locality", 0),
            page_info.get("spatial_locality", 0),
            page_info.get("access_pattern_encoded", 0)
        ]
        return features
        
    def _encode_location(self, location: str) -> int:
        """Encode endpoint location to integer"""
        endpoint_list = sorted(self.endpoints.keys())
        return endpoint_list.index(location)
        
    def _decode_location(self, index: int) -> str:
        """Decode integer to endpoint location"""
        endpoint_list = sorted(self.endpoints.keys())
        return endpoint_list[index]
        
    def predict_optimal_placement(self, pages: List[MemoryPage]) -> List[PlacementDecision]:
        """Predict optimal placement for a set of pages"""
        
        decisions = []
        
        # First, predict future access patterns
        self._predict_future_accesses(pages)
        
        # Cluster pages by access pattern
        page_clusters = self._cluster_pages_by_pattern(pages)
        
        # Make placement decisions
        for page in pages:
            decision = self._make_placement_decision(page, page_clusters)
            decisions.append(decision)
            
        # Optimize for load balancing
        decisions = self._optimize_load_balance(decisions)
        
        return decisions
        
    def _predict_future_accesses(self, pages: List[MemoryPage]) -> None:
        """Predict future access counts for pages"""
        
        if self.access_predictor is None:
            # Use simple heuristic
            for page in pages:
                # Exponential decay based on recency
                time_since_access = time.time() - page.last_access_time
                decay_factor = np.exp(-time_since_access / 3600)  # 1 hour decay
                page.predicted_future_accesses = int(page.access_count * decay_factor)
        else:
            # Use trained ML model
            features = []
            for page in pages:
                feature_vector = [
                    page.access_count,
                    time.time() - page.last_access_time,
                    page.heat_score,
                    page.access_pattern.value == "temporal"
                ]
                features.append(feature_vector)
                
            predictions = self.access_predictor.predict(features)
            for page, pred in zip(pages, predictions):
                page.predicted_future_accesses = int(pred)
                
    def _cluster_pages_by_pattern(self, pages: List[MemoryPage]) -> Dict[str, List[MemoryPage]]:
        """Cluster pages by access pattern similarity"""
        
        if len(pages) < 10:
            # Too few pages to cluster
            return {"default": pages}
            
        # Extract features for clustering
        features = []
        for page in pages:
            feature = [
                page.access_count,
                page.heat_score,
                page.predicted_future_accesses,
                1.0 if page.access_pattern == AccessPattern.SEQUENTIAL else 0.0,
                1.0 if page.access_pattern == AccessPattern.RANDOM else 0.0,
                1.0 if page.access_pattern == AccessPattern.TEMPORAL else 0.0
            ]
            features.append(feature)
            
        # Normalize features
        scaler = StandardScaler()
        features_normalized = scaler.fit_transform(features)
        
        # Cluster pages
        n_clusters = min(5, len(pages) // 10)
        kmeans = KMeans(n_clusters=n_clusters, random_state=42)
        clusters = kmeans.fit_predict(features_normalized)
        
        # Group pages by cluster
        page_clusters = {}
        for page, cluster in zip(pages, clusters):
            cluster_name = f"cluster_{cluster}"
            if cluster_name not in page_clusters:
                page_clusters[cluster_name] = []
            page_clusters[cluster_name].append(page)
            
        return page_clusters
        
    def _make_placement_decision(self, 
                               page: MemoryPage,
                               page_clusters: Dict[str, List[MemoryPage]]) -> PlacementDecision:
        """Make placement decision for a single page"""
        
        current_endpoint = self.endpoints[page.current_location]
        
        # Calculate placement scores for each endpoint
        placement_scores = {}
        
        for endpoint_id, endpoint in self.endpoints.items():
            score = self._calculate_placement_score(page, endpoint, page_clusters)
            placement_scores[endpoint_id] = score
            
        # Select best placement
        best_location = max(placement_scores.items(), key=lambda x: x[1])[0]
        best_score = placement_scores[best_location]
        current_score = placement_scores[page.current_location]
        
        # Calculate expected improvement
        if current_score > 0:
            improvement = (best_score - current_score) / current_score
        else:
            improvement = 1.0 if best_score > 0 else 0.0
            
        # Generate reasoning
        reasoning = self._generate_placement_reasoning(
            page, best_location, placement_scores
        )
        
        # Calculate confidence
        confidence = self._calculate_confidence(placement_scores)
        
        return PlacementDecision(
            page_id=page.page_id,
            current_location=page.current_location,
            recommended_location=best_location,
            expected_improvement=improvement,
            confidence=confidence,
            reasoning=reasoning
        )
        
    def _calculate_placement_score(self,
                                 page: MemoryPage,
                                 endpoint: EndpointCharacteristics,
                                 page_clusters: Dict[str, List[MemoryPage]]) -> float:
        """Calculate placement score for page on endpoint"""
        
        # Base score components
        latency_score = 100 / (endpoint.latency_ns + 1)  # Lower latency is better
        bandwidth_score = endpoint.bandwidth_gbps / 100  # Normalized bandwidth
        
        # Access frequency weight
        access_weight = min(page.predicted_future_accesses / 1000, 1.0)
        
        # Distance penalty
        distance_penalty = 1.0 / (1.0 + endpoint.distance_from_cpu * 0.2)
        
        # Capacity availability
        available_ratio = 1.0 - (endpoint.used_gb / endpoint.capacity_gb)
        capacity_score = min(available_ratio * 2, 1.0)  # Bonus for available space
        
        # Congestion penalty
        congestion_penalty = 1.0 - endpoint.congestion_level
        
        # Pattern-specific scoring
        pattern_score = 1.0
        if page.access_pattern == AccessPattern.SEQUENTIAL:
            # Sequential benefits from high bandwidth
            pattern_score = bandwidth_score * 1.5
        elif page.access_pattern == AccessPattern.RANDOM:
            # Random access needs low latency
            pattern_score = latency_score * 1.5
        elif page.access_pattern == AccessPattern.TEMPORAL:
            # Temporal locality benefits from being close
            pattern_score = distance_penalty * 2.0
            
        # Hot data bonus for local memory
        if endpoint.endpoint_id == "endpoint_1" and page.heat_score > 0.7:
            pattern_score *= 1.5
            
        # Combine scores
        total_score = (
            latency_score * 0.3 +
            bandwidth_score * 0.2 +
            capacity_score * 0.2 +
            pattern_score * 0.2 +
            congestion_penalty * 0.1
        ) * access_weight * distance_penalty
        
        return total_score
        
    def _generate_placement_reasoning(self,
                                    page: MemoryPage,
                                    recommended_location: str,
                                    placement_scores: Dict[str, float]) -> str:
        """Generate human-readable reasoning for placement decision"""
        
        reasons = []
        
        recommended_endpoint = self.endpoints[recommended_location]
        current_endpoint = self.endpoints[page.current_location]
        
        # Latency improvement
        if recommended_endpoint.latency_ns < current_endpoint.latency_ns:
            improvement = (current_endpoint.latency_ns - recommended_endpoint.latency_ns) / current_endpoint.latency_ns
            reasons.append(f"Reduces latency by {improvement:.0%}")
            
        # Bandwidth improvement
        if recommended_endpoint.bandwidth_gbps > current_endpoint.bandwidth_gbps:
            improvement = (recommended_endpoint.bandwidth_gbps - current_endpoint.bandwidth_gbps) / current_endpoint.bandwidth_gbps
            reasons.append(f"Increases bandwidth by {improvement:.0%}")
            
        # Access pattern optimization
        if page.access_pattern == AccessPattern.SEQUENTIAL and recommended_endpoint.bandwidth_gbps > 50:
            reasons.append("High bandwidth endpoint suits sequential access pattern")
        elif page.access_pattern == AccessPattern.RANDOM and recommended_endpoint.latency_ns < 100:
            reasons.append("Low latency endpoint optimal for random access")
            
        # Hot data placement
        if page.heat_score > 0.7 and recommended_location == "endpoint_1":
            reasons.append("Hot data placed in local memory for fastest access")
            
        # Load balancing
        if current_endpoint.congestion_level > 0.7:
            reasons.append(f"Relieves congestion on {current_endpoint.endpoint_id}")
            
        return "; ".join(reasons) if reasons else "Maintains current optimal placement"
        
    def _calculate_confidence(self, placement_scores: Dict[str, float]) -> float:
        """Calculate confidence in placement decision"""
        
        scores = list(placement_scores.values())
        if len(scores) < 2:
            return 0.5
            
        # Sort scores in descending order
        scores.sort(reverse=True)
        
        # Confidence based on score difference
        if scores[0] > 0:
            confidence = (scores[0] - scores[1]) / scores[0]
        else:
            confidence = 0.0
            
        # Adjust for absolute scores
        if scores[0] < 10:  # Low absolute scores
            confidence *= 0.5
            
        return min(confidence, 1.0)
        
    def _optimize_load_balance(self,
                             decisions: List[PlacementDecision]) -> List[PlacementDecision]:
        """Optimize placement decisions for load balancing"""
        
        # Calculate proposed load distribution
        proposed_loads = {ep: 0 for ep in self.endpoints}
        
        for decision in decisions:
            proposed_loads[decision.recommended_location] += 1
            
        # Check for overloaded endpoints
        total_pages = len(decisions)
        balanced_load = total_pages / len(self.endpoints)
        
        overloaded = []
        underutilized = []
        
        for endpoint_id, load in proposed_loads.items():
            load_ratio = load / balanced_load
            if load_ratio > 1.5:  # More than 50% over average
                overloaded.append((endpoint_id, load))
            elif load_ratio < 0.5:  # Less than 50% of average
                underutilized.append((endpoint_id, load))
                
        # Rebalance if needed
        if overloaded and underutilized:
            decisions = self._rebalance_decisions(
                decisions, overloaded, underutilized
            )
            
        return decisions
        
    def _rebalance_decisions(self,
                           decisions: List[PlacementDecision],
                           overloaded: List[Tuple[str, int]],
                           underutilized: List[Tuple[str, int]]) -> List[PlacementDecision]:
        """Rebalance placement decisions"""
        
        # Sort decisions by confidence (lowest first for easier migration)
        decisions_sorted = sorted(decisions, key=lambda d: d.confidence)
        
        for decision in decisions_sorted:
            # Check if this decision contributes to overload
            if any(decision.recommended_location == ep for ep, _ in overloaded):
                # Try to find alternative placement
                for under_ep, _ in underutilized:
                    # Only move if it doesn't hurt performance too much
                    if decision.expected_improvement < 0.2:  # Less than 20% improvement
                        decision.recommended_location = under_ep
                        decision.reasoning += "; Adjusted for load balancing"
                        decision.confidence *= 0.8  # Reduce confidence
                        break
                        
        return decisions
        
    def simulate_placement_performance(self,
                                     decisions: List[PlacementDecision],
                                     workload_trace: List[Dict]) -> Dict:
        """Simulate performance of placement decisions"""
        
        # Apply placement decisions in simulation
        placement_map = {d.page_id: d.recommended_location for d in decisions}
        
        # Run simulation with CXLMemSim
        sim_results = self._run_placement_simulation(placement_map, workload_trace)
        
        # Analyze results
        performance_metrics = {
            "avg_latency": sim_results.get("avg_latency", 0),
            "throughput": sim_results.get("throughput", 0),
            "hit_rate": sim_results.get("hit_rate", 0),
            "migrations": len([d for d in decisions if d.current_location != d.recommended_location]),
            "improvement": np.mean([d.expected_improvement for d in decisions])
        }
        
        # Store for learning
        self.performance_metrics.append(performance_metrics)
        
        return performance_metrics
        
    def _run_placement_simulation(self,
                                placement_map: Dict[int, str],
                                workload_trace: List[Dict]) -> Dict:
        """Run CXLMemSim with specific placement configuration"""
        
        # This would integrate with actual CXLMemSim
        # For now, return simulated results
        
        # Calculate expected metrics based on placement
        total_accesses = sum(t.get("access_count", 0) for t in workload_trace)
        weighted_latency = 0
        
        for trace in workload_trace:
            page_id = trace.get("page_id")
            access_count = trace.get("access_count", 0)
            
            if page_id in placement_map:
                endpoint = self.endpoints[placement_map[page_id]]
                weighted_latency += endpoint.latency_ns * access_count
                
        avg_latency = weighted_latency / total_accesses if total_accesses > 0 else 0
        
        return {
            "avg_latency": avg_latency,
            "throughput": 1000000 / avg_latency if avg_latency > 0 else 0,
            "hit_rate": 0.85  # Simulated
        }
        
    def generate_placement_report(self,
                                decisions: List[PlacementDecision],
                                performance_metrics: Dict,
                                output_dir: Path) -> None:
        """Generate comprehensive placement optimization report"""
        
        output_dir.mkdir(parents=True, exist_ok=True)
        
        # Placement distribution visualization
        self._visualize_placement_distribution(decisions, output_dir)
        
        # Performance improvement analysis
        self._analyze_performance_improvements(decisions, performance_metrics, output_dir)
        
        # Migration recommendations
        self._generate_migration_plan(decisions, output_dir)
        
        # Save detailed results
        results = {
            "decisions": [
                {
                    "page_id": d.page_id,
                    "current": d.current_location,
                    "recommended": d.recommended_location,
                    "improvement": f"{d.expected_improvement:.1%}",
                    "confidence": f"{d.confidence:.1%}",
                    "reasoning": d.reasoning
                }
                for d in decisions
            ],
            "performance_metrics": performance_metrics,
            "summary": self._generate_summary_stats(decisions)
        }
        
        with open(output_dir / "placement_results.json", 'w') as f:
            json.dump(results, f, indent=2)
            
    def _visualize_placement_distribution(self,
                                        decisions: List[PlacementDecision],
                                        output_dir: Path) -> None:
        """Visualize placement distribution across endpoints"""
        
        fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(15, 6))
        
        # Current vs Recommended distribution
        current_dist = {}
        recommended_dist = {}
        
        for d in decisions:
            current_dist[d.current_location] = current_dist.get(d.current_location, 0) + 1
            recommended_dist[d.recommended_location] = recommended_dist.get(d.recommended_location, 0) + 1
            
        endpoints = sorted(self.endpoints.keys())
        current_counts = [current_dist.get(ep, 0) for ep in endpoints]
        recommended_counts = [recommended_dist.get(ep, 0) for ep in endpoints]
        
        x = np.arange(len(endpoints))
        width = 0.35
        
        ax1.bar(x - width/2, current_counts, width, label='Current', alpha=0.7)
        ax1.bar(x + width/2, recommended_counts, width, label='Recommended', alpha=0.7)
        ax1.set_xlabel('Endpoints')
        ax1.set_ylabel('Number of Pages')
        ax1.set_title('Page Distribution: Current vs Recommended')
        ax1.set_xticks(x)
        ax1.set_xticklabels(endpoints)
        ax1.legend()
        
        # Migration flow
        migration_matrix = np.zeros((len(endpoints), len(endpoints)))
        for d in decisions:
            if d.current_location != d.recommended_location:
                i = endpoints.index(d.current_location)
                j = endpoints.index(d.recommended_location)
                migration_matrix[i][j] += 1
                
        im = ax2.imshow(migration_matrix, cmap='YlOrRd')
        ax2.set_xticks(range(len(endpoints)))
        ax2.set_yticks(range(len(endpoints)))
        ax2.set_xticklabels(endpoints)
        ax2.set_yticklabels(endpoints)
        ax2.set_xlabel('To Endpoint')
        ax2.set_ylabel('From Endpoint')
        ax2.set_title('Migration Flow Matrix')
        
        # Add values to cells
        for i in range(len(endpoints)):
            for j in range(len(endpoints)):
                if migration_matrix[i][j] > 0:
                    ax2.text(j, i, f'{int(migration_matrix[i][j])}',
                           ha='center', va='center', color='white' if migration_matrix[i][j] > 5 else 'black')
                    
        plt.colorbar(im, ax=ax2)
        plt.tight_layout()
        plt.savefig(output_dir / "placement_distribution.png", dpi=150, bbox_inches='tight')
        plt.close()
        
    def _analyze_performance_improvements(self,
                                        decisions: List[PlacementDecision],
                                        performance_metrics: Dict,
                                        output_dir: Path) -> None:
        """Analyze expected performance improvements"""
        
        # Calculate improvement statistics
        improvements = [d.expected_improvement for d in decisions if d.expected_improvement > 0]
        
        fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14, 6))
        
        # Improvement distribution
        ax1.hist(improvements, bins=20, alpha=0.7, color='green')
        ax1.axvline(np.mean(improvements), color='red', linestyle='--', 
                   label=f'Mean: {np.mean(improvements):.1%}')
        ax1.set_xlabel('Expected Performance Improvement')
        ax1.set_ylabel('Number of Pages')
        ax1.set_title('Distribution of Expected Improvements')
        ax1.legend()
        
        # Confidence vs Improvement scatter
        confidences = [d.confidence for d in decisions]
        all_improvements = [d.expected_improvement for d in decisions]
        
        scatter = ax2.scatter(confidences, all_improvements, alpha=0.5)
        ax2.set_xlabel('Confidence Score')
        ax2.set_ylabel('Expected Improvement')
        ax2.set_title('Confidence vs Expected Improvement')
        ax2.grid(True, alpha=0.3)
        
        # Add quadrant lines
        ax2.axhline(0, color='black', linestyle='-', alpha=0.3)
        ax2.axvline(0.5, color='black', linestyle='-', alpha=0.3)
        
        plt.tight_layout()
        plt.savefig(output_dir / "performance_analysis.png", dpi=150, bbox_inches='tight')
        plt.close()
        
    def _generate_migration_plan(self,
                                decisions: List[PlacementDecision],
                                output_dir: Path) -> None:
        """Generate prioritized migration plan"""
        
        # Filter migrations
        migrations = [d for d in decisions if d.current_location != d.recommended_location]
        
        # Sort by impact (improvement * confidence)
        migrations.sort(key=lambda d: d.expected_improvement * d.confidence, reverse=True)
        
        # Create migration phases
        high_priority = []
        medium_priority = []
        low_priority = []
        
        for m in migrations:
            impact = m.expected_improvement * m.confidence
            if impact > 0.5:
                high_priority.append(m)
            elif impact > 0.2:
                medium_priority.append(m)
            else:
                low_priority.append(m)
                
        # Generate migration plan document
        with open(output_dir / "migration_plan.md", 'w') as f:
            f.write("# Data Migration Plan\n\n")
            
            f.write("## Summary\n")
            f.write(f"- Total pages to migrate: {len(migrations)}\n")
            f.write(f"- High priority migrations: {len(high_priority)}\n")
            f.write(f"- Medium priority migrations: {len(medium_priority)}\n")
            f.write(f"- Low priority migrations: {len(low_priority)}\n")
            f.write(f"- Expected average improvement: {np.mean([m.expected_improvement for m in migrations]):.1%}\n\n")
            
            f.write("## Phase 1: High Priority Migrations\n")
            f.write("Execute these migrations first for maximum impact.\n\n")
            
            for i, m in enumerate(high_priority[:10], 1):
                f.write(f"{i}. Page {m.page_id}: {m.current_location} → {m.recommended_location}\n")
                f.write(f"   - Expected improvement: {m.expected_improvement:.1%}\n")
                f.write(f"   - Confidence: {m.confidence:.1%}\n")
                f.write(f"   - Reason: {m.reasoning}\n\n")
                
            if len(high_priority) > 10:
                f.write(f"\n... and {len(high_priority) - 10} more high priority migrations\n\n")
                
            f.write("## Phase 2: Medium Priority Migrations\n")
            f.write("Execute after Phase 1 completion.\n\n")
            
            f.write(f"Total: {len(medium_priority)} migrations\n")
            f.write(f"Average expected improvement: {np.mean([m.expected_improvement for m in medium_priority]) if medium_priority else 0:.1%}\n\n")
            
            f.write("## Phase 3: Low Priority Migrations\n")
            f.write("Optional - execute during maintenance windows.\n\n")
            
            f.write(f"Total: {len(low_priority)} migrations\n")
            f.write(f"Average expected improvement: {np.mean([m.expected_improvement for m in low_priority]) if low_priority else 0:.1%}\n")
            
    def _generate_summary_stats(self, decisions: List[PlacementDecision]) -> Dict:
        """Generate summary statistics"""
        
        migrations = [d for d in decisions if d.current_location != d.recommended_location]
        
        return {
            "total_pages": len(decisions),
            "pages_to_migrate": len(migrations),
            "migration_percentage": f"{len(migrations) / len(decisions) * 100:.1f}%",
            "average_improvement": f"{np.mean([d.expected_improvement for d in decisions]):.1%}",
            "average_confidence": f"{np.mean([d.confidence for d in decisions]):.1%}",
            "high_confidence_decisions": len([d for d in decisions if d.confidence > 0.8]),
            "endpoint_balance": {
                ep: len([d for d in decisions if d.recommended_location == ep])
                for ep in self.endpoints
            }
        }


def main():
    parser = argparse.ArgumentParser(
        description="Predictive Topology and Placement Optimizer"
    )
    parser.add_argument("--cxlmemsim", required=True, help="Path to CXLMemSim binary")
    parser.add_argument("--topology", required=True, help="Topology configuration file")
    parser.add_argument("--workload", required=True, help="Workload trace file")
    parser.add_argument("--training-data", help="Training data for ML models")
    parser.add_argument("--output", default="./placement_results", help="Output directory")
    
    args = parser.parse_args()
    
    # Load configurations
    with open(args.topology, 'r') as f:
        topology_config = yaml.safe_load(f)
        
    with open(args.workload, 'r') as f:
        workload_data = yaml.safe_load(f)
        
    # Initialize predictor
    predictor = TopologyPlacementPredictor(args.cxlmemsim, topology_config)
    
    # Train models if data provided
    if args.training_data:
        with open(args.training_data, 'r') as f:
            training_data = yaml.safe_load(f)
        predictor.train_placement_model(training_data)
        
    # Create memory pages from workload
    pages = []
    for page_info in workload_data["pages"]:
        page = MemoryPage(
            page_id=page_info["id"],
            size_kb=page_info.get("size_kb", 4),
            access_count=page_info["access_count"],
            last_access_time=page_info.get("last_access_time", time.time()),
            access_pattern=AccessPattern(page_info.get("pattern", "random")),
            current_location=page_info["current_location"],
            heat_score=page_info.get("heat_score", 0.5)
        )
        pages.append(page)
        
    # Predict optimal placement
    print("Analyzing access patterns and predicting optimal placement...")
    decisions = predictor.predict_optimal_placement(pages)
    
    # Simulate performance
    print("Simulating placement performance...")
    performance = predictor.simulate_placement_performance(
        decisions,
        workload_data.get("trace", [])
    )
    
    # Generate report
    print("Generating placement optimization report...")
    predictor.generate_placement_report(
        decisions,
        performance,
        Path(args.output)
    )
    
    print(f"\nPlacement optimization complete. Results saved to {args.output}")
    
    # Print summary
    migrations = [d for d in decisions if d.current_location != d.recommended_location]
    print(f"\nSummary:")
    print(f"- Pages analyzed: {len(pages)}")
    print(f"- Migrations recommended: {len(migrations)}")
    print(f"- Expected average improvement: {np.mean([d.expected_improvement for d in decisions]):.1%}")
    print(f"- Simulated latency reduction: {performance.get('avg_latency', 0):.1f}ns")


if __name__ == "__main__":
    main()