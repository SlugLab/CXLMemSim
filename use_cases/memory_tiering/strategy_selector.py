#!/usr/bin/env python3
"""
Intelligent Strategy Selector for CXL Memory Tiering
Selects the best memory management strategy based on workload characteristics and hotness patterns
"""

import numpy as np
from typing import Dict, List, Tuple, Optional
from dataclasses import dataclass
from enum import Enum
import json

class MemoryStrategy(Enum):
    STATIC_BALANCED = "static_balanced"
    STATIC_LOCAL_HEAVY = "static_local_heavy"
    STATIC_CXL_HEAVY = "static_cxl_heavy"
    HOTNESS_BASED = "hotness_based"
    TOPOLOGY_OPTIMIZED = "topology_optimized"
    ADAPTIVE_HOTNESS = "adaptive_hotness"
    ML_BASED = "ml_based"
    HYBRID_ADAPTIVE = "hybrid_adaptive"

@dataclass
class StrategyProfile:
    name: MemoryStrategy
    suitable_workload_types: List[str]
    hotness_skew_range: Tuple[float, float]  # (min, max)
    locality_range: Tuple[float, float]
    performance_characteristics: Dict[str, float]
    description: str

class StrategySelector:
    def __init__(self):
        self.strategy_profiles = self._initialize_strategy_profiles()
        self.selection_history = []
        
    def _initialize_strategy_profiles(self) -> Dict[MemoryStrategy, StrategyProfile]:
        """Initialize strategy profiles with their characteristics"""
        return {
            MemoryStrategy.STATIC_BALANCED: StrategyProfile(
                name=MemoryStrategy.STATIC_BALANCED,
                suitable_workload_types=["general", "mixed"],
                hotness_skew_range=(0.0, 0.3),
                locality_range=(0.4, 0.6),
                performance_characteristics={
                    "latency_sensitivity": 0.5,
                    "bandwidth_efficiency": 0.5,
                    "adaptation_overhead": 0.0,
                    "stability": 1.0
                },
                description="Fixed 50/50 allocation between local and CXL memory"
            ),
            
            MemoryStrategy.STATIC_LOCAL_HEAVY: StrategyProfile(
                name=MemoryStrategy.STATIC_LOCAL_HEAVY,
                suitable_workload_types=["database", "latency_critical"],
                hotness_skew_range=(0.6, 1.0),
                locality_range=(0.7, 1.0),
                performance_characteristics={
                    "latency_sensitivity": 0.9,
                    "bandwidth_efficiency": 0.3,
                    "adaptation_overhead": 0.0,
                    "stability": 1.0
                },
                description="Prioritizes local memory (80/20) for hot data"
            ),
            
            MemoryStrategy.STATIC_CXL_HEAVY: StrategyProfile(
                name=MemoryStrategy.STATIC_CXL_HEAVY,
                suitable_workload_types=["analytics", "batch_processing"],
                hotness_skew_range=(0.0, 0.4),
                locality_range=(0.0, 0.4),
                performance_characteristics={
                    "latency_sensitivity": 0.2,
                    "bandwidth_efficiency": 0.8,
                    "adaptation_overhead": 0.0,
                    "stability": 1.0
                },
                description="Leverages CXL capacity (30/70) for large datasets"
            ),
            
            MemoryStrategy.HOTNESS_BASED: StrategyProfile(
                name=MemoryStrategy.HOTNESS_BASED,
                suitable_workload_types=["database", "web", "cache"],
                hotness_skew_range=(0.4, 0.8),
                locality_range=(0.5, 0.9),
                performance_characteristics={
                    "latency_sensitivity": 0.8,
                    "bandwidth_efficiency": 0.6,
                    "adaptation_overhead": 0.2,
                    "stability": 0.8
                },
                description="Dynamically allocates based on page hotness"
            ),
            
            MemoryStrategy.TOPOLOGY_OPTIMIZED: StrategyProfile(
                name=MemoryStrategy.TOPOLOGY_OPTIMIZED,
                suitable_workload_types=["distributed", "multi_tenant"],
                hotness_skew_range=(0.2, 0.6),
                locality_range=(0.3, 0.7),
                performance_characteristics={
                    "latency_sensitivity": 0.6,
                    "bandwidth_efficiency": 0.8,
                    "adaptation_overhead": 0.1,
                    "stability": 0.9
                },
                description="Optimizes allocation based on CXL topology structure"
            ),
            
            MemoryStrategy.ADAPTIVE_HOTNESS: StrategyProfile(
                name=MemoryStrategy.ADAPTIVE_HOTNESS,
                suitable_workload_types=["dynamic", "bursty"],
                hotness_skew_range=(0.3, 0.9),
                locality_range=(0.2, 0.8),
                performance_characteristics={
                    "latency_sensitivity": 0.7,
                    "bandwidth_efficiency": 0.7,
                    "adaptation_overhead": 0.4,
                    "stability": 0.6
                },
                description="Continuously adapts to changing hotness patterns"
            ),
            
            MemoryStrategy.ML_BASED: StrategyProfile(
                name=MemoryStrategy.ML_BASED,
                suitable_workload_types=["predictable", "ml", "scientific"],
                hotness_skew_range=(0.0, 1.0),
                locality_range=(0.0, 1.0),
                performance_characteristics={
                    "latency_sensitivity": 0.7,
                    "bandwidth_efficiency": 0.8,
                    "adaptation_overhead": 0.3,
                    "stability": 0.7
                },
                description="Uses machine learning to predict optimal allocation"
            ),
            
            MemoryStrategy.HYBRID_ADAPTIVE: StrategyProfile(
                name=MemoryStrategy.HYBRID_ADAPTIVE,
                suitable_workload_types=["complex", "multi_phase"],
                hotness_skew_range=(0.0, 1.0),
                locality_range=(0.0, 1.0),
                performance_characteristics={
                    "latency_sensitivity": 0.8,
                    "bandwidth_efficiency": 0.9,
                    "adaptation_overhead": 0.5,
                    "stability": 0.5
                },
                description="Combines multiple strategies adaptively"
            )
        }
        
    def select_strategy(self,
                       workload_characteristics: Dict,
                       hotness_profile: Dict,
                       topology_info: Dict,
                       performance_requirements: Optional[Dict] = None) -> Tuple[MemoryStrategy, float, str]:
        """
        Select the best memory strategy based on comprehensive analysis
        
        Returns:
            - Selected strategy
            - Confidence score (0-1)
            - Reasoning for selection
        """
        
        # Extract key metrics
        workload_type = workload_characteristics.get("type", "general")
        memory_intensity = workload_characteristics.get("memory_intensity", 0.5)
        access_locality = workload_characteristics.get("access_locality", 0.5)
        temporal_pattern = hotness_profile.get("temporal_pattern", "stable")
        hotness_skew = hotness_profile.get("hotness_skew", 0.5)
        
        # Score each strategy
        strategy_scores = {}
        strategy_reasons = {}
        
        for strategy, profile in self.strategy_profiles.items():
            score = 0.0
            reasons = []
            
            # Workload type compatibility
            if workload_type in profile.suitable_workload_types:
                score += 0.3
                reasons.append(f"Suitable for {workload_type} workloads")
            elif "general" in profile.suitable_workload_types:
                score += 0.15
                reasons.append("General purpose strategy")
                
            # Hotness skew compatibility
            if profile.hotness_skew_range[0] <= hotness_skew <= profile.hotness_skew_range[1]:
                score += 0.25
                reasons.append(f"Matches hotness skew ({hotness_skew:.2f})")
            else:
                # Partial score based on distance from range
                distance = min(
                    abs(hotness_skew - profile.hotness_skew_range[0]),
                    abs(hotness_skew - profile.hotness_skew_range[1])
                )
                score += max(0, 0.25 - distance * 0.5)
                
            # Locality compatibility
            if profile.locality_range[0] <= access_locality <= profile.locality_range[1]:
                score += 0.2
                reasons.append(f"Matches locality pattern ({access_locality:.2f})")
                
            # Performance requirements matching
            if performance_requirements:
                perf_score = self._calculate_performance_match(
                    profile.performance_characteristics,
                    performance_requirements
                )
                score += 0.25 * perf_score
                if perf_score > 0.8:
                    reasons.append("Meets performance requirements")
                    
            # Temporal pattern bonus
            if temporal_pattern == "stable" and profile.performance_characteristics["stability"] > 0.8:
                score += 0.1
                reasons.append("Good for stable access patterns")
            elif temporal_pattern in ["bursty", "random"] and profile.performance_characteristics["adaptation_overhead"] > 0.3:
                score += 0.1
                reasons.append(f"Handles {temporal_pattern} patterns well")
                
            # Topology considerations
            num_endpoints = topology_info.get("num_endpoints", 2)
            if strategy in [MemoryStrategy.TOPOLOGY_OPTIMIZED, MemoryStrategy.HYBRID_ADAPTIVE] and num_endpoints > 3:
                score += 0.15
                reasons.append(f"Optimized for {num_endpoints} endpoints")
                
            strategy_scores[strategy] = score
            strategy_reasons[strategy] = reasons
            
        # Select best strategy
        best_strategy = max(strategy_scores.items(), key=lambda x: x[1])
        selected_strategy = best_strategy[0]
        confidence = best_strategy[1]
        
        # Generate comprehensive reasoning
        selected_profile = self.strategy_profiles[selected_strategy]
        reasoning = f"{selected_profile.description}. "
        reasoning += f"Selected because: {', '.join(strategy_reasons[selected_strategy])}. "
        
        # Add comparative reasoning
        second_best = sorted(strategy_scores.items(), key=lambda x: x[1], reverse=True)[1]
        if confidence - second_best[1] < 0.1:
            reasoning += f"Close alternative: {second_best[0].value} (score difference: {confidence - second_best[1]:.2f})"
            
        # Record selection
        self.selection_history.append({
            "workload_type": workload_type,
            "selected_strategy": selected_strategy.value,
            "confidence": confidence,
            "hotness_skew": hotness_skew,
            "locality": access_locality
        })
        
        return selected_strategy, confidence, reasoning
        
    def _calculate_performance_match(self,
                                   strategy_characteristics: Dict[str, float],
                                   requirements: Dict[str, float]) -> float:
        """Calculate how well strategy characteristics match performance requirements"""
        
        score = 0.0
        weight_sum = 0.0
        
        # Map requirements to strategy characteristics
        mapping = {
            "latency_critical": "latency_sensitivity",
            "bandwidth_critical": "bandwidth_efficiency",
            "stability_required": "stability",
            "adaptation_allowed": lambda x: 1.0 - x["adaptation_overhead"]
        }
        
        for req_key, req_value in requirements.items():
            if req_key in mapping:
                weight = req_value  # Use requirement value as weight
                
                if callable(mapping[req_key]):
                    char_value = mapping[req_key](strategy_characteristics)
                else:
                    char_key = mapping[req_key]
                    char_value = strategy_characteristics.get(char_key, 0.5)
                    
                score += weight * char_value
                weight_sum += weight
                
        return score / weight_sum if weight_sum > 0 else 0.5
        
    def get_strategy_recommendation_matrix(self) -> Dict:
        """Generate a recommendation matrix for different scenarios"""
        
        scenarios = [
            {"name": "High Locality Database", "type": "database", "locality": 0.9, "skew": 0.8},
            {"name": "Distributed Analytics", "type": "analytics", "locality": 0.3, "skew": 0.2},
            {"name": "Web Cache Server", "type": "web", "locality": 0.6, "skew": 0.5},
            {"name": "ML Training", "type": "ml", "locality": 0.7, "skew": 0.6},
            {"name": "Random Access", "type": "general", "locality": 0.2, "skew": 0.1},
        ]
        
        matrix = {}
        
        for scenario in scenarios:
            workload_chars = {
                "type": scenario["type"],
                "access_locality": scenario["locality"]
            }
            hotness_profile = {
                "hotness_skew": scenario["skew"],
                "temporal_pattern": "stable"
            }
            topology_info = {"num_endpoints": 4}
            
            strategy, confidence, _ = self.select_strategy(
                workload_chars,
                hotness_profile,
                topology_info
            )
            
            matrix[scenario["name"]] = {
                "recommended_strategy": strategy.value,
                "confidence": confidence
            }
            
        return matrix
        
    def analyze_selection_history(self) -> Dict:
        """Analyze historical strategy selections for insights"""
        
        if not self.selection_history:
            return {"error": "No selection history available"}
            
        analysis = {
            "total_selections": len(self.selection_history),
            "strategy_distribution": {},
            "avg_confidence_by_strategy": {},
            "workload_strategy_mapping": {},
            "hotness_skew_correlation": {}
        }
        
        # Strategy distribution
        strategy_counts = {}
        confidence_sums = {}
        
        for selection in self.selection_history:
            strategy = selection["selected_strategy"]
            confidence = selection["confidence"]
            
            strategy_counts[strategy] = strategy_counts.get(strategy, 0) + 1
            confidence_sums[strategy] = confidence_sums.get(strategy, 0) + confidence
            
        for strategy, count in strategy_counts.items():
            analysis["strategy_distribution"][strategy] = count / len(self.selection_history)
            analysis["avg_confidence_by_strategy"][strategy] = confidence_sums[strategy] / count
            
        # Workload to strategy mapping
        workload_strategies = {}
        for selection in self.selection_history:
            workload = selection["workload_type"]
            strategy = selection["selected_strategy"]
            
            if workload not in workload_strategies:
                workload_strategies[workload] = {}
            workload_strategies[workload][strategy] = workload_strategies[workload].get(strategy, 0) + 1
            
        analysis["workload_strategy_mapping"] = workload_strategies
        
        # Hotness skew correlation
        for strategy in set(s["selected_strategy"] for s in self.selection_history):
            skews = [s["hotness_skew"] for s in self.selection_history if s["selected_strategy"] == strategy]
            if skews:
                analysis["hotness_skew_correlation"][strategy] = {
                    "avg_skew": np.mean(skews),
                    "std_skew": np.std(skews)
                }
                
        return analysis


def demonstrate_strategy_selection():
    """Demonstrate strategy selection with various scenarios"""
    
    selector = StrategySelector()
    
    # Test scenarios
    test_scenarios = [
        {
            "name": "High-Performance Database",
            "workload": {"type": "database", "memory_intensity": 0.9, "access_locality": 0.85},
            "hotness": {"hotness_skew": 0.75, "temporal_pattern": "stable"},
            "topology": {"num_endpoints": 4},
            "requirements": {"latency_critical": 0.9, "stability_required": 0.8}
        },
        {
            "name": "Large-Scale Analytics",
            "workload": {"type": "analytics", "memory_intensity": 0.6, "access_locality": 0.3},
            "hotness": {"hotness_skew": 0.2, "temporal_pattern": "bursty"},
            "topology": {"num_endpoints": 6},
            "requirements": {"bandwidth_critical": 0.9, "adaptation_allowed": 0.7}
        },
        {
            "name": "Dynamic Web Application",
            "workload": {"type": "web", "memory_intensity": 0.7, "access_locality": 0.5},
            "hotness": {"hotness_skew": 0.5, "temporal_pattern": "random"},
            "topology": {"num_endpoints": 3},
            "requirements": {"latency_critical": 0.6, "stability_required": 0.5}
        }
    ]
    
    print("Strategy Selection Demonstration\n" + "="*50)
    
    for scenario in test_scenarios:
        print(f"\n{scenario['name']}:")
        
        strategy, confidence, reasoning = selector.select_strategy(
            scenario["workload"],
            scenario["hotness"],
            scenario["topology"],
            scenario.get("requirements")
        )
        
        print(f"  Selected Strategy: {strategy.value}")
        print(f"  Confidence: {confidence:.2%}")
        print(f"  Reasoning: {reasoning}")
        
    # Show recommendation matrix
    print("\n\nRecommendation Matrix\n" + "="*50)
    matrix = selector.get_strategy_recommendation_matrix()
    
    for scenario, recommendation in matrix.items():
        print(f"{scenario:25} -> {recommendation['recommended_strategy']:20} (confidence: {recommendation['confidence']:.2%})")
        
    # Analyze selection history
    print("\n\nSelection History Analysis\n" + "="*50)
    analysis = selector.analyze_selection_history()
    
    print(f"Total selections: {analysis['total_selections']}")
    print("\nStrategy distribution:")
    for strategy, percentage in analysis['strategy_distribution'].items():
        print(f"  {strategy}: {percentage:.1%}")
        
    print("\nAverage confidence by strategy:")
    for strategy, avg_conf in analysis['avg_confidence_by_strategy'].items():
        print(f"  {strategy}: {avg_conf:.2%}")


if __name__ == "__main__":
    demonstrate_strategy_selection()