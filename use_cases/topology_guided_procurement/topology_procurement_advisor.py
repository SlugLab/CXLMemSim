#!/usr/bin/env python3
"""
Topology-Guided Hardware Procurement Advisor for CXL Memory Systems
Helps make data-driven hardware purchasing decisions based on workload hotness patterns
"""

import argparse
import json
import numpy as np
import pandas as pd
from pathlib import Path
from typing import Dict, List, Tuple, Optional
import subprocess
import yaml
from dataclasses import dataclass
import matplotlib.pyplot as plt
import seaborn as sns
from concurrent.futures import ProcessPoolExecutor, as_completed

@dataclass
class HardwareOption:
    vendor: str
    model: str
    topology_type: str  # flat, hierarchical, star, mesh
    num_endpoints: int
    memory_per_endpoint_gb: int
    latency_profile: List[float]  # base latencies for each hop distance
    bandwidth_profile: List[float]  # base bandwidths for each hop distance
    cost_per_endpoint: float
    power_per_endpoint_watts: float
    topology_string: str
    features: List[str]  # e.g., ["hot-cold-separation", "multi-tier", "pooling"]

@dataclass
class WorkloadProfile:
    name: str
    type: str
    hotness_distribution: Dict[str, float]  # predicted hotness per endpoint
    memory_requirement_gb: int
    performance_sensitivity: float  # 0-1, higher = more sensitive
    growth_projection: float  # expected growth rate per year

@dataclass
class ProcurementRecommendation:
    hardware_option: HardwareOption
    topology_config: str
    total_cost: float
    performance_score: float
    scalability_score: float
    tco_3_year: float
    reasoning: List[str]
    risk_factors: List[str]

class TopologyProcurementAdvisor:
    def __init__(self, cxlmemsim_path: str):
        self.cxlmemsim_path = Path(cxlmemsim_path)
        self.hardware_catalog = self._load_hardware_catalog()
        self.evaluation_cache = {}
        
    def _load_hardware_catalog(self) -> List[HardwareOption]:
        """Load available hardware options from vendors"""
        # In practice, this would load from a database or API
        # For demo, we'll create representative options
        catalog = [
            HardwareOption(
                vendor="Vendor A",
                model="CXL-Flat-256",
                topology_type="flat",
                num_endpoints=4,
                memory_per_endpoint_gb=256,
                latency_profile=[150, 170, 190],
                bandwidth_profile=[64000, 60000, 56000],
                cost_per_endpoint=2000,
                power_per_endpoint_watts=25,
                topology_string="(1,(2,3,4))",
                features=["hot-cold-separation", "multi-tier"]
            ),
            HardwareOption(
                vendor="Vendor B", 
                model="CXL-Hierarchy-512",
                topology_type="hierarchical",
                num_endpoints=8,
                memory_per_endpoint_gb=512,
                latency_profile=[180, 220, 280],
                bandwidth_profile=[48000, 40000, 32000],
                cost_per_endpoint=3500,
                power_per_endpoint_watts=35,
                topology_string="(1,((2,3,4),(5,6,7,8)))",
                features=["pooling", "multi-tier", "dynamic-allocation"]
            ),
            HardwareOption(
                vendor="Vendor C",
                model="CXL-Star-384",
                topology_type="star",
                num_endpoints=6,
                memory_per_endpoint_gb=384,
                latency_profile=[160, 200, 240],
                bandwidth_profile=[56000, 48000, 40000],
                cost_per_endpoint=2800,
                power_per_endpoint_watts=30,
                topology_string="(1,(2,(3,4,5,6)))",
                features=["centralized-management", "hot-cold-separation"]
            ),
            HardwareOption(
                vendor="Vendor D",
                model="CXL-Mesh-1024",
                topology_type="mesh",
                num_endpoints=16,
                memory_per_endpoint_gb=1024,
                latency_profile=[200, 280, 360, 440],
                bandwidth_profile=[40000, 32000, 24000, 16000],
                cost_per_endpoint=6000,
                power_per_endpoint_watts=50,
                topology_string="(1,((2,3,4,5),((6,7),(8,9)),((10,11),(12,13,14,15,16))))",
                features=["massive-capacity", "fault-tolerance", "dynamic-routing"]
            ),
            HardwareOption(
                vendor="Vendor A",
                model="CXL-Economy-128",
                topology_type="flat",
                num_endpoints=2,
                memory_per_endpoint_gb=128,
                latency_profile=[140, 160],
                bandwidth_profile=[48000, 44000],
                cost_per_endpoint=1200,
                power_per_endpoint_watts=20,
                topology_string="(1,(2))",
                features=["budget-friendly", "simple-setup"]
            )
        ]
        return catalog
        
    def analyze_workload_requirements(self, workload_configs: List[Dict]) -> List[WorkloadProfile]:
        """Analyze workloads to determine hardware requirements"""
        profiles = []
        
        for config in workload_configs:
            # Predict hotness distribution
            hotness_dist = self._predict_hotness_distribution(config)
            
            # Calculate memory requirements
            memory_req = config.get("working_set_size", 100)
            memory_req += memory_req * config.get("memory_overhead", 0.2)  # Add overhead
            
            # Determine performance sensitivity
            perf_sensitivity = self._calculate_performance_sensitivity(config)
            
            # Estimate growth
            growth = config.get("annual_growth_rate", 0.2)
            
            profile = WorkloadProfile(
                name=config["name"],
                type=config.get("type", "general"),
                hotness_distribution=hotness_dist,
                memory_requirement_gb=int(memory_req),
                performance_sensitivity=perf_sensitivity,
                growth_projection=growth
            )
            profiles.append(profile)
            
        return profiles
        
    def _predict_hotness_distribution(self, workload_config: Dict) -> Dict[str, float]:
        """Predict hotness distribution across endpoints"""
        workload_type = workload_config.get("type", "general")
        num_endpoints = workload_config.get("target_endpoints", 4)
        
        hotness_dist = {}
        
        if workload_type == "database":
            # Databases often have skewed access patterns
            for i in range(num_endpoints):
                hotness = 0.9 * np.exp(-i * 0.5)
                hotness_dist[f"endpoint_{i+2}"] = float(np.clip(hotness, 0.1, 1.0))
                
        elif workload_type == "analytics":
            # Analytics may have more uniform distribution
            base_hotness = 0.4
            for i in range(num_endpoints):
                hotness = base_hotness + 0.2 * np.sin(i * np.pi / num_endpoints)
                hotness_dist[f"endpoint_{i+2}"] = float(np.clip(hotness, 0.2, 0.8))
                
        elif workload_type == "ml_training":
            # ML training may have phases with different patterns
            for i in range(num_endpoints):
                if i < num_endpoints // 2:
                    hotness = 0.7  # Active training data
                else:
                    hotness = 0.3  # Validation/test data
                hotness_dist[f"endpoint_{i+2}"] = hotness
                
        else:
            # General workload
            for i in range(num_endpoints):
                hotness = 0.5 - 0.05 * i
                hotness_dist[f"endpoint_{i+2}"] = float(np.clip(hotness, 0.2, 0.8))
                
        return hotness_dist
        
    def _calculate_performance_sensitivity(self, workload_config: Dict) -> float:
        """Calculate how sensitive the workload is to performance"""
        factors = {
            "latency_critical": workload_config.get("latency_critical", False),
            "real_time": workload_config.get("real_time", False),
            "interactive": workload_config.get("interactive", False),
            "sla_strict": workload_config.get("sla_requirements", {}).get("strict", False)
        }
        
        sensitivity = 0.3  # Base sensitivity
        
        if factors["latency_critical"]:
            sensitivity += 0.3
        if factors["real_time"]:
            sensitivity += 0.2
        if factors["interactive"]:
            sensitivity += 0.1
        if factors["sla_strict"]:
            sensitivity += 0.1
            
        return min(sensitivity, 1.0)
        
    def evaluate_hardware_for_workloads(self,
                                      hardware: HardwareOption,
                                      workload_profiles: List[WorkloadProfile]) -> Dict:
        """Evaluate how well hardware meets workload requirements"""
        
        total_score = 0
        detailed_scores = {}
        
        for workload in workload_profiles:
            # Check capacity
            total_capacity = hardware.num_endpoints * hardware.memory_per_endpoint_gb
            capacity_score = min(1.0, total_capacity / workload.memory_requirement_gb)
            
            # Evaluate topology fit for hotness pattern
            topology_score = self._evaluate_topology_fit(
                hardware.topology_type,
                workload.hotness_distribution,
                hardware.num_endpoints
            )
            
            # Performance evaluation
            perf_score = self._evaluate_performance(
                hardware,
                workload.hotness_distribution,
                workload.performance_sensitivity
            )
            
            # Scalability for growth
            growth_capacity = total_capacity * (1 + workload.growth_projection * 3)
            scalability_score = min(1.0, total_capacity / growth_capacity)
            
            # Weighted score
            workload_score = (
                capacity_score * 0.3 +
                topology_score * 0.3 +
                perf_score * 0.3 +
                scalability_score * 0.1
            )
            
            detailed_scores[workload.name] = {
                "capacity": capacity_score,
                "topology_fit": topology_score,
                "performance": perf_score,
                "scalability": scalability_score,
                "overall": workload_score
            }
            
            total_score += workload_score
            
        return {
            "average_score": total_score / len(workload_profiles),
            "detailed_scores": detailed_scores,
            "min_score": min(s["overall"] for s in detailed_scores.values()),
            "capacity_utilization": self._calculate_capacity_utilization(
                hardware, workload_profiles
            )
        }
        
    def _evaluate_topology_fit(self,
                              topology_type: str,
                              hotness_dist: Dict[str, float],
                              num_endpoints: int) -> float:
        """Evaluate how well topology matches hotness distribution"""
        
        hotness_values = list(hotness_dist.values())[:num_endpoints-1]
        if not hotness_values:
            return 0.5
            
        hotness_skew = np.std(hotness_values) / (np.mean(hotness_values) + 1e-6)
        
        # Match topology to hotness characteristics
        if topology_type == "flat":
            # Flat is good for uniform distribution
            return 1.0 - min(hotness_skew, 1.0)
            
        elif topology_type == "hierarchical":
            # Hierarchical is good for moderate skew
            return 1.0 - abs(hotness_skew - 0.5) * 2
            
        elif topology_type == "star":
            # Star is good for centralized access
            return min(hotness_skew * 1.5, 1.0)
            
        elif topology_type == "mesh":
            # Mesh provides flexibility
            return 0.7  # Decent for any pattern
            
        return 0.5
        
    def _evaluate_performance(self,
                            hardware: HardwareOption,
                            hotness_dist: Dict[str, float],
                            sensitivity: float) -> float:
        """Evaluate performance based on hardware characteristics"""
        
        # Calculate weighted average latency based on hotness
        weighted_latency = 0
        total_weight = 0
        
        for i, (endpoint, hotness) in enumerate(hotness_dist.items()):
            if i < len(hardware.latency_profile):
                weighted_latency += hardware.latency_profile[i] * hotness
                total_weight += hotness
                
        if total_weight > 0:
            weighted_latency /= total_weight
            
        # Score based on latency (lower is better)
        base_latency = 150  # Reference latency
        latency_score = base_latency / (weighted_latency + 1)
        
        # Adjust for sensitivity
        perf_score = latency_score ** sensitivity
        
        return perf_score
        
    def _calculate_capacity_utilization(self,
                                      hardware: HardwareOption,
                                      workload_profiles: List[WorkloadProfile]) -> float:
        """Calculate expected capacity utilization"""
        
        total_capacity = hardware.num_endpoints * hardware.memory_per_endpoint_gb
        total_requirement = sum(w.memory_requirement_gb for w in workload_profiles)
        
        return min(total_requirement / total_capacity, 1.0)
        
    def calculate_tco(self,
                     hardware: HardwareOption,
                     years: int = 3,
                     electricity_cost_kwh: float = 0.12) -> Dict:
        """Calculate Total Cost of Ownership"""
        
        # Initial hardware cost
        hardware_cost = hardware.cost_per_endpoint * hardware.num_endpoints
        
        # Power costs
        power_consumption_kw = (hardware.power_per_endpoint_watts * 
                               hardware.num_endpoints) / 1000
        annual_power_cost = power_consumption_kw * 8760 * electricity_cost_kwh
        
        # Maintenance and support (estimated)
        annual_maintenance = hardware_cost * 0.15
        
        # Calculate TCO
        tco_breakdown = {
            "hardware_cost": hardware_cost,
            "power_cost_annual": annual_power_cost,
            "maintenance_annual": annual_maintenance,
            "total_annual_opex": annual_power_cost + annual_maintenance,
            "tco_3_year": hardware_cost + (annual_power_cost + annual_maintenance) * years
        }
        
        return tco_breakdown
        
    def generate_procurement_recommendations(self,
                                           workload_configs: List[Dict],
                                           constraints: Dict) -> List[ProcurementRecommendation]:
        """Generate hardware procurement recommendations"""
        
        # Analyze workload requirements
        workload_profiles = self.analyze_workload_requirements(workload_configs)
        
        recommendations = []
        
        # Evaluate each hardware option
        for hardware in self.hardware_catalog:
            # Check basic constraints
            if hardware.cost_per_endpoint * hardware.num_endpoints > constraints.get("max_budget", float('inf')):
                continue
                
            if hardware.num_endpoints < constraints.get("min_endpoints", 0):
                continue
                
            # Evaluate fit
            evaluation = self.evaluate_hardware_for_workloads(hardware, workload_profiles)
            
            if evaluation["average_score"] < constraints.get("min_performance_score", 0.6):
                continue
                
            # Calculate TCO
            tco = self.calculate_tco(hardware)
            
            # Generate reasoning
            reasoning = self._generate_reasoning(hardware, evaluation, workload_profiles)
            risk_factors = self._identify_risks(hardware, evaluation, workload_profiles)
            
            recommendation = ProcurementRecommendation(
                hardware_option=hardware,
                topology_config=hardware.topology_string,
                total_cost=hardware.cost_per_endpoint * hardware.num_endpoints,
                performance_score=evaluation["average_score"],
                scalability_score=evaluation["detailed_scores"][workload_profiles[0].name]["scalability"],
                tco_3_year=tco["tco_3_year"],
                reasoning=reasoning,
                risk_factors=risk_factors
            )
            
            recommendations.append(recommendation)
            
        # Sort by performance score
        recommendations.sort(key=lambda x: x.performance_score, reverse=True)
        
        return recommendations[:5]  # Top 5 recommendations
        
    def _generate_reasoning(self,
                          hardware: HardwareOption,
                          evaluation: Dict,
                          workload_profiles: List[WorkloadProfile]) -> List[str]:
        """Generate reasoning for recommendation"""
        
        reasoning = []
        
        # Topology fit
        if hardware.topology_type == "flat":
            reasoning.append("Flat topology provides low latency for all endpoints")
        elif hardware.topology_type == "hierarchical":
            reasoning.append("Hierarchical topology balances capacity and performance")
        elif hardware.topology_type == "star":
            reasoning.append("Star topology optimizes for centralized access patterns")
        elif hardware.topology_type == "mesh":
            reasoning.append("Mesh topology offers maximum flexibility and fault tolerance")
            
        # Capacity
        utilization = evaluation["capacity_utilization"]
        if utilization > 0.8:
            reasoning.append(f"High capacity utilization ({utilization:.0%}) maximizes investment")
        elif utilization < 0.5:
            reasoning.append(f"Provides growth headroom with {utilization:.0%} initial utilization")
            
        # Performance
        min_score = evaluation["min_score"]
        if min_score > 0.8:
            reasoning.append("Excellent performance across all workloads")
        elif min_score > 0.6:
            reasoning.append("Good performance for most workloads")
            
        # Features
        if "hot-cold-separation" in hardware.features:
            reasoning.append("Supports hot-cold data separation for optimization")
        if "pooling" in hardware.features:
            reasoning.append("Memory pooling enables flexible resource allocation")
            
        return reasoning
        
    def _identify_risks(self,
                       hardware: HardwareOption,
                       evaluation: Dict,
                       workload_profiles: List[WorkloadProfile]) -> List[str]:
        """Identify potential risks"""
        
        risks = []
        
        # Capacity risks
        total_growth_req = sum(w.memory_requirement_gb * (1 + w.growth_projection * 3) 
                              for w in workload_profiles)
        total_capacity = hardware.num_endpoints * hardware.memory_per_endpoint_gb
        
        if total_growth_req > total_capacity:
            risks.append("May require additional expansion within 3 years")
            
        # Performance risks
        if evaluation["min_score"] < 0.7:
            risks.append("Some workloads may not meet performance targets")
            
        # Topology risks
        if hardware.topology_type == "mesh" and hardware.num_endpoints > 8:
            risks.append("Complex topology may require specialized management")
            
        # Vendor risks
        if hardware.num_endpoints > 8:
            risks.append("Large configurations may have longer lead times")
            
        return risks
        
    def generate_comparison_report(self,
                                 recommendations: List[ProcurementRecommendation],
                                 output_dir: Path) -> None:
        """Generate visual comparison report"""
        
        output_dir.mkdir(parents=True, exist_ok=True)
        
        # Prepare data for visualization
        comparison_data = []
        for rec in recommendations:
            comparison_data.append({
                "Model": f"{rec.hardware_option.vendor} {rec.hardware_option.model}",
                "Topology": rec.hardware_option.topology_type,
                "Endpoints": rec.hardware_option.num_endpoints,
                "Total Memory (GB)": rec.hardware_option.num_endpoints * 
                                   rec.hardware_option.memory_per_endpoint_gb,
                "Performance Score": rec.performance_score,
                "Initial Cost": rec.total_cost,
                "3-Year TCO": rec.tco_3_year,
                "Cost per GB": rec.total_cost / (rec.hardware_option.num_endpoints * 
                                                rec.hardware_option.memory_per_endpoint_gb)
            })
            
        df = pd.DataFrame(comparison_data)
        
        # Create visualizations
        fig, axes = plt.subplots(2, 2, figsize=(15, 10))
        
        # Performance vs Cost scatter
        ax1 = axes[0, 0]
        scatter = ax1.scatter(df["Initial Cost"], df["Performance Score"], 
                            s=df["Total Memory (GB)"]/10, alpha=0.6)
        ax1.set_xlabel("Initial Cost ($)")
        ax1.set_ylabel("Performance Score")
        ax1.set_title("Performance vs Cost (bubble size = memory capacity)")
        
        # Add labels
        for idx, row in df.iterrows():
            ax1.annotate(row["Model"].split()[-1], 
                        (row["Initial Cost"], row["Performance Score"]),
                        fontsize=8)
                        
        # TCO comparison
        ax2 = axes[0, 1]
        df_sorted = df.sort_values("3-Year TCO")
        bars = ax2.bar(range(len(df_sorted)), df_sorted["3-Year TCO"])
        ax2.set_xticks(range(len(df_sorted)))
        ax2.set_xticklabels(df_sorted["Model"], rotation=45, ha='right')
        ax2.set_ylabel("3-Year TCO ($)")
        ax2.set_title("Total Cost of Ownership Comparison")
        
        # Color bars by performance
        colors = plt.cm.RdYlGn(df_sorted["Performance Score"])
        for bar, color in zip(bars, colors):
            bar.set_color(color)
            
        # Topology distribution
        ax3 = axes[1, 0]
        topology_perf = df.groupby("Topology")["Performance Score"].mean()
        topology_perf.plot(kind='bar', ax=ax3, color='skyblue')
        ax3.set_xlabel("Topology Type")
        ax3.set_ylabel("Average Performance Score")
        ax3.set_title("Performance by Topology Type")
        ax3.set_xticklabels(ax3.get_xticklabels(), rotation=0)
        
        # Cost efficiency (Cost per GB vs Performance)
        ax4 = axes[1, 1]
        ax4.scatter(df["Cost per GB"], df["Performance Score"])
        ax4.set_xlabel("Cost per GB ($)")
        ax4.set_ylabel("Performance Score")
        ax4.set_title("Cost Efficiency Analysis")
        
        # Add efficiency frontier
        efficient = df.nsmallest(3, "Cost per GB")
        ax4.plot(efficient["Cost per GB"], efficient["Performance Score"], 
                'r--', label='Efficiency frontier')
        ax4.legend()
        
        plt.tight_layout()
        plt.savefig(output_dir / "hardware_comparison.png", dpi=150, bbox_inches='tight')
        plt.close()
        
        # Generate detailed recommendation table
        self._generate_recommendation_table(recommendations, output_dir)
        
    def _generate_recommendation_table(self,
                                     recommendations: List[ProcurementRecommendation],
                                     output_dir: Path) -> None:
        """Generate detailed recommendation table"""
        
        # Create detailed comparison
        detailed_data = []
        for i, rec in enumerate(recommendations, 1):
            detailed_data.append({
                "Rank": i,
                "Vendor": rec.hardware_option.vendor,
                "Model": rec.hardware_option.model,
                "Configuration": f"{rec.hardware_option.num_endpoints} endpoints × "
                               f"{rec.hardware_option.memory_per_endpoint_gb}GB",
                "Total Capacity": f"{rec.hardware_option.num_endpoints * "
                                f"rec.hardware_option.memory_per_endpoint_gb}GB",
                "Performance": f"{rec.performance_score:.2%}",
                "Initial Cost": f"${rec.total_cost:,.0f}",
                "3-Year TCO": f"${rec.tco_3_year:,.0f}",
                "Key Benefits": rec.reasoning[0] if rec.reasoning else "N/A",
                "Main Risk": rec.risk_factors[0] if rec.risk_factors else "None identified"
            })
            
        df_detailed = pd.DataFrame(detailed_data)
        
        # Save as CSV
        df_detailed.to_csv(output_dir / "procurement_recommendations.csv", index=False)
        
        # Create summary report
        with open(output_dir / "procurement_summary.md", 'w') as f:
            f.write("# CXL Hardware Procurement Recommendations\n\n")
            
            if recommendations:
                top_rec = recommendations[0]
                f.write("## Top Recommendation\n\n")
                f.write(f"**{top_rec.hardware_option.vendor} {top_rec.hardware_option.model}**\n\n")
                f.write(f"- Configuration: {top_rec.hardware_option.num_endpoints} endpoints "
                       f"× {top_rec.hardware_option.memory_per_endpoint_gb}GB\n")
                f.write(f"- Topology: {top_rec.hardware_option.topology_type}\n")
                f.write(f"- Performance Score: {top_rec.performance_score:.2%}\n")
                f.write(f"- Initial Cost: ${top_rec.total_cost:,.0f}\n")
                f.write(f"- 3-Year TCO: ${top_rec.tco_3_year:,.0f}\n\n")
                
                f.write("### Why This Configuration?\n\n")
                for reason in top_rec.reasoning:
                    f.write(f"- {reason}\n")
                    
                if top_rec.risk_factors:
                    f.write("\n### Considerations\n\n")
                    for risk in top_rec.risk_factors:
                        f.write(f"- {risk}\n")
                        
            f.write("\n## All Recommendations\n\n")
            f.write("See `procurement_recommendations.csv` for detailed comparison.\n")
            f.write("\n## Visual Analysis\n\n")
            f.write("See `hardware_comparison.png` for visual comparison of options.\n")


def main():
    parser = argparse.ArgumentParser(
        description="Topology-Guided Hardware Procurement Advisor"
    )
    parser.add_argument("--cxlmemsim", required=True, help="Path to CXLMemSim binary")
    parser.add_argument("--workloads", required=True, help="Workload configuration file")
    parser.add_argument("--constraints", required=True, help="Procurement constraints file")
    parser.add_argument("--output", default="./procurement_results", help="Output directory")
    
    args = parser.parse_args()
    
    # Load configurations
    with open(args.workloads, 'r') as f:
        workload_configs = yaml.safe_load(f)["workloads"]
        
    with open(args.constraints, 'r') as f:
        constraints = yaml.safe_load(f)
        
    # Initialize advisor
    advisor = TopologyProcurementAdvisor(args.cxlmemsim)
    
    # Generate recommendations
    print("Analyzing workload requirements...")
    recommendations = advisor.generate_procurement_recommendations(
        workload_configs,
        constraints
    )
    
    # Generate report
    print("Generating procurement report...")
    advisor.generate_comparison_report(recommendations, Path(args.output))
    
    print(f"\nProcurement analysis complete. Results saved to {args.output}")
    
    # Print summary
    if recommendations:
        print("\nTop 3 Recommendations:")
        for i, rec in enumerate(recommendations[:3], 1):
            print(f"\n{i}. {rec.hardware_option.vendor} {rec.hardware_option.model}")
            print(f"   Performance Score: {rec.performance_score:.2%}")
            print(f"   Initial Cost: ${rec.total_cost:,.0f}")
            print(f"   3-Year TCO: ${rec.tco_3_year:,.0f}")


if __name__ == "__main__":
    main()