#!/usr/bin/env python3
"""
Dynamic Migration Policy Engine for CXL Memory Systems
Implements intelligent data migration policies based on real-time hotness and topology awareness
"""

import argparse
import json
import numpy as np
import pandas as pd
from pathlib import Path
from typing import Dict, List, Tuple, Optional, Callable
import subprocess
import yaml
import time
from dataclasses import dataclass, field
from enum import Enum
from collections import deque
import threading
import queue
from concurrent.futures import ThreadPoolExecutor
import matplotlib.pyplot as plt
import seaborn as sns
from sklearn.ensemble import IsolationForest
from sklearn.preprocessing import StandardScaler

class MigrationTrigger(Enum):
    HOTNESS_THRESHOLD = "hotness_threshold"
    LOAD_IMBALANCE = "load_imbalance"
    PERFORMANCE_DEGRADATION = "performance_degradation"
    PERIODIC = "periodic"
    CONGESTION = "congestion"
    ANOMALY_DETECTED = "anomaly_detected"

class MigrationPolicy(Enum):
    CONSERVATIVE = "conservative"  # Migrate only when absolutely necessary
    BALANCED = "balanced"  # Balance between performance and stability
    AGGRESSIVE = "aggressive"  # Proactive migration for optimization
    ADAPTIVE = "adaptive"  # Learns from migration outcomes
    PREDICTIVE = "predictive"  # Uses ML to predict future access

@dataclass
class MigrationCandidate:
    page_id: int
    current_location: str
    target_location: str
    benefit_score: float  # Expected benefit from migration
    cost_score: float  # Cost of migration
    net_benefit: float  # benefit - cost
    trigger: MigrationTrigger
    timestamp: float
    
@dataclass
class MigrationOutcome:
    candidate: MigrationCandidate
    actual_improvement: float
    migration_duration_ms: float
    success: bool
    post_migration_metrics: Dict

@dataclass
class EndpointState:
    endpoint_id: str
    capacity_gb: int
    used_gb: float
    free_gb: float
    hotness_score: float  # 0-1, aggregate hotness of pages
    access_rate: float  # accesses per second
    bandwidth_utilization: float  # 0-1
    latency_percentiles: Dict[int, float]  # 50th, 95th, 99th
    migration_in_progress: int  # number of ongoing migrations
    last_update: float

@dataclass
class PolicyState:
    policy_type: MigrationPolicy
    migrations_triggered: int
    migrations_succeeded: int
    migrations_failed: int
    total_benefit: float
    total_cost: float
    learning_data: List[MigrationOutcome] = field(default_factory=list)

class DynamicMigrationEngine:
    def __init__(self, cxlmemsim_path: str, topology_config: Dict):
        self.cxlmemsim_path = Path(cxlmemsim_path)
        self.topology = topology_config
        self.endpoint_states = {}
        self.policy_states = {}
        self.migration_queue = queue.PriorityQueue()
        self.migration_history = deque(maxlen=1000)
        self.anomaly_detector = None
        self.executor = ThreadPoolExecutor(max_workers=4)
        self.monitoring_active = False
        self.current_policy = MigrationPolicy.BALANCED
        self._initialize_endpoints()
        self._initialize_policies()
        
    def _initialize_endpoints(self):
        """Initialize endpoint states from topology"""
        # Local memory
        self.endpoint_states["endpoint_1"] = EndpointState(
            endpoint_id="endpoint_1",
            capacity_gb=self.topology.get("local_memory_gb", 128),
            used_gb=0,
            free_gb=self.topology.get("local_memory_gb", 128),
            hotness_score=0.0,
            access_rate=0.0,
            bandwidth_utilization=0.0,
            latency_percentiles={50: 80, 95: 85, 99: 90},
            migration_in_progress=0,
            last_update=time.time()
        )
        
        # CXL endpoints
        for i, ep_config in enumerate(self.topology.get("cxl_endpoints", []), start=2):
            self.endpoint_states[f"endpoint_{i}"] = EndpointState(
                endpoint_id=f"endpoint_{i}",
                capacity_gb=ep_config["capacity_gb"],
                used_gb=0,
                free_gb=ep_config["capacity_gb"],
                hotness_score=0.0,
                access_rate=0.0,
                bandwidth_utilization=0.0,
                latency_percentiles={50: ep_config["latency_ns"], 
                                   95: ep_config["latency_ns"] * 1.2,
                                   99: ep_config["latency_ns"] * 1.5},
                migration_in_progress=0,
                last_update=time.time()
            )
            
    def _initialize_policies(self):
        """Initialize policy states"""
        for policy in MigrationPolicy:
            self.policy_states[policy] = PolicyState(
                policy_type=policy,
                migrations_triggered=0,
                migrations_succeeded=0,
                migrations_failed=0,
                total_benefit=0.0,
                total_cost=0.0
            )
            
    def set_migration_policy(self, policy: MigrationPolicy):
        """Set active migration policy"""
        self.current_policy = policy
        print(f"Migration policy set to: {policy.value}")
        
    def start_monitoring(self, monitoring_interval: float = 1.0):
        """Start continuous monitoring thread"""
        self.monitoring_active = True
        monitor_thread = threading.Thread(
            target=self._monitoring_loop,
            args=(monitoring_interval,),
            daemon=True
        )
        monitor_thread.start()
        
    def stop_monitoring(self):
        """Stop monitoring thread"""
        self.monitoring_active = False
        
    def _monitoring_loop(self, interval: float):
        """Main monitoring loop"""
        while self.monitoring_active:
            # Update endpoint states
            self._update_endpoint_states()
            
            # Check migration triggers
            triggers = self._check_migration_triggers()
            
            # Process triggers based on policy
            if triggers:
                self._process_migration_triggers(triggers)
                
            # Process migration queue
            self._process_migration_queue()
            
            time.sleep(interval)
            
    def _update_endpoint_states(self):
        """Update real-time endpoint states"""
        # In production, this would query actual system metrics
        # For demo, we'll simulate state updates
        
        for endpoint_id, state in self.endpoint_states.items():
            # Simulate metric updates
            state.access_rate = np.random.poisson(100)  # accesses/sec
            state.bandwidth_utilization = min(state.access_rate / 1000, 1.0)
            
            # Update hotness based on access rate
            state.hotness_score = min(state.access_rate / 500, 1.0)
            
            # Simulate latency variations
            base_latency = state.latency_percentiles[50]
            state.latency_percentiles[50] = base_latency * (1 + np.random.normal(0, 0.1))
            state.latency_percentiles[95] = state.latency_percentiles[50] * 1.2
            state.latency_percentiles[99] = state.latency_percentiles[50] * 1.5
            
            state.last_update = time.time()
            
    def _check_migration_triggers(self) -> List[Tuple[MigrationTrigger, Dict]]:
        """Check for migration triggers"""
        triggers = []
        
        # Hotness threshold trigger
        for endpoint_id, state in self.endpoint_states.items():
            if state.hotness_score > 0.8 and endpoint_id != "endpoint_1":
                triggers.append((
                    MigrationTrigger.HOTNESS_THRESHOLD,
                    {"endpoint": endpoint_id, "hotness": state.hotness_score}
                ))
                
        # Load imbalance trigger
        capacities = [s.capacity_gb for s in self.endpoint_states.values()]
        used = [s.used_gb for s in self.endpoint_states.values()]
        
        if capacities:
            utilizations = [u/c for u, c in zip(used, capacities)]
            imbalance = np.std(utilizations)
            
            if imbalance > 0.3:  # 30% standard deviation
                triggers.append((
                    MigrationTrigger.LOAD_IMBALANCE,
                    {"imbalance": imbalance, "utilizations": utilizations}
                ))
                
        # Performance degradation trigger
        for endpoint_id, state in self.endpoint_states.items():
            if state.latency_percentiles[99] > state.latency_percentiles[50] * 2:
                triggers.append((
                    MigrationTrigger.PERFORMANCE_DEGRADATION,
                    {"endpoint": endpoint_id, "p99_latency": state.latency_percentiles[99]}
                ))
                
        # Congestion trigger
        for endpoint_id, state in self.endpoint_states.items():
            if state.bandwidth_utilization > 0.9:
                triggers.append((
                    MigrationTrigger.CONGESTION,
                    {"endpoint": endpoint_id, "bandwidth_util": state.bandwidth_utilization}
                ))
                
        # Anomaly detection trigger
        if self.anomaly_detector:
            anomalies = self._detect_anomalies()
            if anomalies:
                triggers.append((
                    MigrationTrigger.ANOMALY_DETECTED,
                    {"anomalies": anomalies}
                ))
                
        return triggers
        
    def _detect_anomalies(self) -> List[Dict]:
        """Detect anomalous access patterns"""
        if not self.migration_history:
            return []
            
        # Extract features from recent history
        features = []
        for outcome in list(self.migration_history)[-100:]:
            features.append([
                outcome.candidate.benefit_score,
                outcome.candidate.cost_score,
                outcome.actual_improvement,
                outcome.migration_duration_ms
            ])
            
        if len(features) < 10:
            return []
            
        # Train anomaly detector if not exists
        if self.anomaly_detector is None:
            self.anomaly_detector = IsolationForest(contamination=0.1)
            scaler = StandardScaler()
            features_scaled = scaler.fit_transform(features)
            self.anomaly_detector.fit(features_scaled)
            return []
            
        # Detect anomalies in recent data
        recent_features = features[-10:]
        predictions = self.anomaly_detector.predict(recent_features)
        
        anomalies = []
        for i, pred in enumerate(predictions):
            if pred == -1:  # Anomaly
                anomalies.append({
                    "index": i,
                    "features": recent_features[i]
                })
                
        return anomalies
        
    def _process_migration_triggers(self, triggers: List[Tuple[MigrationTrigger, Dict]]):
        """Process migration triggers based on current policy"""
        
        for trigger_type, trigger_data in triggers:
            if self.current_policy == MigrationPolicy.CONSERVATIVE:
                # Only act on critical triggers
                if trigger_type in [MigrationTrigger.PERFORMANCE_DEGRADATION, 
                                  MigrationTrigger.CONGESTION]:
                    self._generate_migration_candidates(trigger_type, trigger_data)
                    
            elif self.current_policy == MigrationPolicy.BALANCED:
                # Act on most triggers but with moderation
                if trigger_type != MigrationTrigger.PERIODIC:
                    self._generate_migration_candidates(trigger_type, trigger_data)
                    
            elif self.current_policy == MigrationPolicy.AGGRESSIVE:
                # Act on all triggers proactively
                self._generate_migration_candidates(trigger_type, trigger_data)
                
            elif self.current_policy == MigrationPolicy.ADAPTIVE:
                # Use learning to decide
                if self._should_act_on_trigger(trigger_type, trigger_data):
                    self._generate_migration_candidates(trigger_type, trigger_data)
                    
            elif self.current_policy == MigrationPolicy.PREDICTIVE:
                # Predict future state and act preemptively
                if self._predict_future_benefit(trigger_type, trigger_data) > 0.5:
                    self._generate_migration_candidates(trigger_type, trigger_data)
                    
    def _should_act_on_trigger(self, trigger_type: MigrationTrigger, trigger_data: Dict) -> bool:
        """Adaptive decision on whether to act on trigger"""
        
        # Look at historical outcomes for similar triggers
        similar_outcomes = [
            outcome for outcome in self.migration_history
            if outcome.candidate.trigger == trigger_type
        ]
        
        if not similar_outcomes:
            return True  # No history, so try
            
        # Calculate success rate
        successful = sum(1 for o in similar_outcomes if o.success and o.actual_improvement > 0)
        success_rate = successful / len(similar_outcomes)
        
        # Act if success rate is above threshold
        return success_rate > 0.6
        
    def _predict_future_benefit(self, trigger_type: MigrationTrigger, trigger_data: Dict) -> float:
        """Predict future benefit of acting on trigger"""
        
        # Simple prediction based on trigger type and current state
        if trigger_type == MigrationTrigger.HOTNESS_THRESHOLD:
            # High hotness likely to continue
            return 0.8
        elif trigger_type == MigrationTrigger.CONGESTION:
            # Congestion needs immediate action
            return 0.9
        elif trigger_type == MigrationTrigger.LOAD_IMBALANCE:
            # Load imbalance tends to worsen
            return 0.7
        else:
            return 0.5
            
    def _generate_migration_candidates(self, trigger_type: MigrationTrigger, trigger_data: Dict):
        """Generate migration candidates based on trigger"""
        
        candidates = []
        
        if trigger_type == MigrationTrigger.HOTNESS_THRESHOLD:
            # Move hot pages from congested endpoint to local memory
            endpoint = trigger_data["endpoint"]
            
            # Simulate identifying hot pages (would be real page tracking in production)
            for i in range(5):  # Top 5 hot pages
                candidate = MigrationCandidate(
                    page_id=1000 + i,
                    current_location=endpoint,
                    target_location="endpoint_1",  # Move to local memory
                    benefit_score=0.8,
                    cost_score=0.2,
                    net_benefit=0.6,
                    trigger=trigger_type,
                    timestamp=time.time()
                )
                candidates.append(candidate)
                
        elif trigger_type == MigrationTrigger.LOAD_IMBALANCE:
            # Move pages from overloaded to underloaded endpoints
            utilizations = trigger_data["utilizations"]
            
            # Find most and least utilized endpoints
            endpoints = list(self.endpoint_states.keys())
            most_utilized_idx = np.argmax(utilizations)
            least_utilized_idx = np.argmin(utilizations)
            
            if most_utilized_idx != least_utilized_idx:
                # Move some pages
                for i in range(3):
                    candidate = MigrationCandidate(
                        page_id=2000 + i,
                        current_location=endpoints[most_utilized_idx],
                        target_location=endpoints[least_utilized_idx],
                        benefit_score=0.5,
                        cost_score=0.3,
                        net_benefit=0.2,
                        trigger=trigger_type,
                        timestamp=time.time()
                    )
                    candidates.append(candidate)
                    
        # Add candidates to migration queue
        for candidate in candidates:
            # Priority is negative net benefit (higher benefit = higher priority)
            priority = -candidate.net_benefit
            self.migration_queue.put((priority, candidate))
            
    def _process_migration_queue(self):
        """Process pending migrations"""
        
        migrations_to_process = []
        
        # Get migrations up to concurrency limit
        max_concurrent = 5
        current_migrations = sum(s.migration_in_progress for s in self.endpoint_states.values())
        
        while not self.migration_queue.empty() and len(migrations_to_process) < (max_concurrent - current_migrations):
            try:
                _, candidate = self.migration_queue.get_nowait()
                migrations_to_process.append(candidate)
            except queue.Empty:
                break
                
        # Execute migrations asynchronously
        for candidate in migrations_to_process:
            self.executor.submit(self._execute_migration, candidate)
            
    def _execute_migration(self, candidate: MigrationCandidate):
        """Execute a single migration"""
        
        start_time = time.time()
        
        # Update endpoint states
        self.endpoint_states[candidate.current_location].migration_in_progress += 1
        self.endpoint_states[candidate.target_location].migration_in_progress += 1
        
        try:
            # Simulate migration (would be actual data movement in production)
            migration_duration = np.random.uniform(10, 100)  # ms
            time.sleep(migration_duration / 1000)  # Convert to seconds
            
            # Simulate success/failure
            success = np.random.random() > 0.1  # 90% success rate
            
            if success:
                # Update endpoint capacities
                page_size_gb = 0.001  # 1MB page
                self.endpoint_states[candidate.current_location].used_gb -= page_size_gb
                self.endpoint_states[candidate.target_location].used_gb += page_size_gb
                
                # Calculate actual improvement
                actual_improvement = candidate.benefit_score * np.random.uniform(0.8, 1.2)
            else:
                actual_improvement = 0.0
                
            # Record outcome
            outcome = MigrationOutcome(
                candidate=candidate,
                actual_improvement=actual_improvement,
                migration_duration_ms=migration_duration,
                success=success,
                post_migration_metrics={
                    "source_utilization": self.endpoint_states[candidate.current_location].used_gb / 
                                        self.endpoint_states[candidate.current_location].capacity_gb,
                    "target_utilization": self.endpoint_states[candidate.target_location].used_gb / 
                                        self.endpoint_states[candidate.target_location].capacity_gb
                }
            )
            
            # Update history and policy state
            self.migration_history.append(outcome)
            self._update_policy_state(outcome)
            
        finally:
            # Update endpoint states
            self.endpoint_states[candidate.current_location].migration_in_progress -= 1
            self.endpoint_states[candidate.target_location].migration_in_progress -= 1
            
    def _update_policy_state(self, outcome: MigrationOutcome):
        """Update policy state based on migration outcome"""
        
        policy_state = self.policy_states[self.current_policy]
        policy_state.migrations_triggered += 1
        
        if outcome.success:
            policy_state.migrations_succeeded += 1
            policy_state.total_benefit += outcome.actual_improvement
        else:
            policy_state.migrations_failed += 1
            
        policy_state.total_cost += outcome.migration_duration_ms / 1000  # Convert to seconds
        policy_state.learning_data.append(outcome)
        
    def evaluate_policies(self, duration_seconds: int = 300) -> Dict:
        """Evaluate different migration policies"""
        
        results = {}
        
        for policy in MigrationPolicy:
            print(f"\nEvaluating {policy.value} policy...")
            
            # Reset state
            self._initialize_endpoints()
            self._initialize_policies()
            self.migration_history.clear()
            
            # Set policy and run
            self.set_migration_policy(policy)
            self.start_monitoring(monitoring_interval=0.5)
            
            time.sleep(duration_seconds)
            
            self.stop_monitoring()
            
            # Collect results
            policy_state = self.policy_states[policy]
            results[policy.value] = {
                "migrations_triggered": policy_state.migrations_triggered,
                "migrations_succeeded": policy_state.migrations_succeeded,
                "migrations_failed": policy_state.migrations_failed,
                "success_rate": policy_state.migrations_succeeded / max(policy_state.migrations_triggered, 1),
                "total_benefit": policy_state.total_benefit,
                "total_cost": policy_state.total_cost,
                "benefit_cost_ratio": policy_state.total_benefit / max(policy_state.total_cost, 1)
            }
            
        return results
        
    def generate_policy_report(self, evaluation_results: Dict, output_dir: Path):
        """Generate comprehensive policy evaluation report"""
        
        output_dir.mkdir(parents=True, exist_ok=True)
        
        # Create comparison visualizations
        self._visualize_policy_comparison(evaluation_results, output_dir)
        
        # Analyze migration patterns
        self._analyze_migration_patterns(output_dir)
        
        # Generate recommendations
        self._generate_policy_recommendations(evaluation_results, output_dir)
        
    def _visualize_policy_comparison(self, results: Dict, output_dir: Path):
        """Visualize policy comparison"""
        
        fig, axes = plt.subplots(2, 2, figsize=(15, 10))
        
        policies = list(results.keys())
        
        # Success rates
        ax1 = axes[0, 0]
        success_rates = [results[p]["success_rate"] for p in policies]
        bars1 = ax1.bar(policies, success_rates, color='green', alpha=0.7)
        ax1.set_ylabel('Success Rate')
        ax1.set_title('Migration Success Rate by Policy')
        ax1.set_ylim(0, 1)
        
        # Add value labels
        for bar, rate in zip(bars1, success_rates):
            ax1.text(bar.get_x() + bar.get_width()/2, bar.get_height() + 0.01,
                    f'{rate:.1%}', ha='center', va='bottom')
                    
        # Benefit-cost ratio
        ax2 = axes[0, 1]
        bc_ratios = [results[p]["benefit_cost_ratio"] for p in policies]
        bars2 = ax2.bar(policies, bc_ratios, color='blue', alpha=0.7)
        ax2.set_ylabel('Benefit/Cost Ratio')
        ax2.set_title('Efficiency by Policy')
        
        # Migration volume
        ax3 = axes[1, 0]
        migrations = [results[p]["migrations_triggered"] for p in policies]
        bars3 = ax3.bar(policies, migrations, color='orange', alpha=0.7)
        ax3.set_ylabel('Total Migrations')
        ax3.set_title('Migration Activity by Policy')
        
        # Total benefit
        ax4 = axes[1, 1]
        benefits = [results[p]["total_benefit"] for p in policies]
        bars4 = ax4.bar(policies, benefits, color='purple', alpha=0.7)
        ax4.set_ylabel('Total Benefit Score')
        ax4.set_title('Cumulative Benefit by Policy')
        
        # Rotate x-axis labels
        for ax in axes.flat:
            ax.set_xticklabels(ax.get_xticklabels(), rotation=45, ha='right')
            
        plt.tight_layout()
        plt.savefig(output_dir / "policy_comparison.png", dpi=150, bbox_inches='tight')
        plt.close()
        
    def _analyze_migration_patterns(self, output_dir: Path):
        """Analyze migration patterns from history"""
        
        if not self.migration_history:
            return
            
        # Extract migration data
        migration_data = []
        for outcome in self.migration_history:
            migration_data.append({
                "trigger": outcome.candidate.trigger.value,
                "source": outcome.candidate.current_location,
                "target": outcome.candidate.target_location,
                "benefit": outcome.actual_improvement,
                "duration_ms": outcome.migration_duration_ms,
                "success": outcome.success
            })
            
        df = pd.DataFrame(migration_data)
        
        fig, axes = plt.subplots(2, 2, figsize=(15, 10))
        
        # Migration triggers distribution
        ax1 = axes[0, 0]
        trigger_counts = df["trigger"].value_counts()
        trigger_counts.plot(kind='bar', ax=ax1, color='skyblue')
        ax1.set_title('Migration Triggers Distribution')
        ax1.set_xlabel('Trigger Type')
        ax1.set_ylabel('Count')
        ax1.set_xticklabels(ax1.get_xticklabels(), rotation=45, ha='right')
        
        # Migration flow heatmap
        ax2 = axes[0, 1]
        migration_matrix = pd.crosstab(df["source"], df["target"])
        sns.heatmap(migration_matrix, annot=True, fmt='d', cmap='YlOrRd', ax=ax2)
        ax2.set_title('Migration Flow Matrix')
        
        # Benefit distribution by trigger
        ax3 = axes[1, 0]
        df_success = df[df["success"]]
        if not df_success.empty:
            df_success.boxplot(column='benefit', by='trigger', ax=ax3)
            ax3.set_title('Benefit Distribution by Trigger Type')
            ax3.set_xlabel('Trigger Type')
            ax3.set_ylabel('Actual Benefit')
            
        # Migration duration distribution
        ax4 = axes[1, 1]
        ax4.hist(df["duration_ms"], bins=20, alpha=0.7, color='green')
        ax4.axvline(df["duration_ms"].mean(), color='red', linestyle='--',
                   label=f'Mean: {df["duration_ms"].mean():.1f}ms')
        ax4.set_xlabel('Migration Duration (ms)')
        ax4.set_ylabel('Count')
        ax4.set_title('Migration Duration Distribution')
        ax4.legend()
        
        plt.tight_layout()
        plt.savefig(output_dir / "migration_patterns.png", dpi=150, bbox_inches='tight')
        plt.close()
        
        # Save detailed statistics
        stats = {
            "total_migrations": len(df),
            "successful_migrations": len(df[df["success"]]),
            "average_benefit": df[df["success"]]["benefit"].mean() if not df[df["success"]].empty else 0,
            "average_duration_ms": df["duration_ms"].mean(),
            "trigger_distribution": trigger_counts.to_dict(),
            "most_common_migration": f"{df.mode()['source'].iloc[0]} -> {df.mode()['target'].iloc[0]}" if not df.empty else "N/A"
        }
        
        with open(output_dir / "migration_statistics.json", 'w') as f:
            json.dump(stats, f, indent=2)
            
    def _generate_policy_recommendations(self, results: Dict, output_dir: Path):
        """Generate policy recommendations based on evaluation"""
        
        # Rank policies by different criteria
        rankings = {
            "efficiency": sorted(results.items(), 
                               key=lambda x: x[1]["benefit_cost_ratio"], 
                               reverse=True),
            "reliability": sorted(results.items(), 
                                key=lambda x: x[1]["success_rate"], 
                                reverse=True),
            "performance": sorted(results.items(), 
                                key=lambda x: x[1]["total_benefit"], 
                                reverse=True),
            "stability": sorted(results.items(), 
                              key=lambda x: x[1]["migrations_triggered"])
        }
        
        with open(output_dir / "policy_recommendations.md", 'w') as f:
            f.write("# Migration Policy Recommendations\n\n")
            
            f.write("## Executive Summary\n\n")
            
            # Best overall policy
            best_overall = rankings["efficiency"][0][0]
            f.write(f"**Recommended Policy**: {best_overall}\n\n")
            
            f.write("### Key Findings\n\n")
            
            # Efficiency leader
            f.write(f"- **Most Efficient**: {rankings['efficiency'][0][0]} "
                   f"(Benefit/Cost Ratio: {rankings['efficiency'][0][1]['benefit_cost_ratio']:.2f})\n")
                   
            # Reliability leader
            f.write(f"- **Most Reliable**: {rankings['reliability'][0][0]} "
                   f"(Success Rate: {rankings['reliability'][0][1]['success_rate']:.1%})\n")
                   
            # Performance leader
            f.write(f"- **Highest Performance**: {rankings['performance'][0][0]} "
                   f"(Total Benefit: {rankings['performance'][0][1]['total_benefit']:.1f})\n")
                   
            # Stability leader
            f.write(f"- **Most Stable**: {rankings['stability'][0][0]} "
                   f"(Migrations: {rankings['stability'][0][1]['migrations_triggered']})\n\n")
                   
            f.write("## Detailed Analysis\n\n")
            
            for policy, metrics in results.items():
                f.write(f"### {policy.title()} Policy\n\n")
                
                f.write("**Metrics:**\n")
                f.write(f"- Migrations Triggered: {metrics['migrations_triggered']}\n")
                f.write(f"- Success Rate: {metrics['success_rate']:.1%}\n")
                f.write(f"- Benefit/Cost Ratio: {metrics['benefit_cost_ratio']:.2f}\n")
                f.write(f"- Total Benefit: {metrics['total_benefit']:.1f}\n\n")
                
                # Recommendations
                f.write("**When to Use:**\n")
                
                if policy == "conservative":
                    f.write("- Systems requiring high stability\n")
                    f.write("- Production environments with strict SLAs\n")
                    f.write("- Limited migration bandwidth\n")
                elif policy == "balanced":
                    f.write("- General-purpose deployments\n")
                    f.write("- Mixed workload environments\n")
                    f.write("- Moderate performance requirements\n")
                elif policy == "aggressive":
                    f.write("- Performance-critical applications\n")
                    f.write("- Systems with spare migration capacity\n")
                    f.write("- Rapidly changing workloads\n")
                elif policy == "adaptive":
                    f.write("- Long-running systems that can learn\n")
                    f.write("- Environments with predictable patterns\n")
                    f.write("- Systems requiring self-optimization\n")
                elif policy == "predictive":
                    f.write("- Workloads with clear access patterns\n")
                    f.write("- Systems with ML capabilities\n")
                    f.write("- Proactive optimization scenarios\n")
                    
                f.write("\n")
                
            f.write("## Implementation Guidelines\n\n")
            f.write("1. Start with **Balanced** policy for initial deployment\n")
            f.write("2. Monitor migration patterns for 1-2 weeks\n")
            f.write("3. Switch to **Adaptive** once sufficient data collected\n")
            f.write("4. Consider **Aggressive** for specific performance-critical periods\n")
            f.write("5. Use **Conservative** during maintenance windows\n")


def main():
    parser = argparse.ArgumentParser(
        description="Dynamic Migration Policy Engine for CXL Memory"
    )
    parser.add_argument("--cxlmemsim", required=True, help="Path to CXLMemSim binary")
    parser.add_argument("--topology", required=True, help="Topology configuration file")
    parser.add_argument("--evaluate", action="store_true", help="Run policy evaluation")
    parser.add_argument("--duration", type=int, default=300, help="Evaluation duration in seconds")
    parser.add_argument("--policy", choices=[p.value for p in MigrationPolicy],
                       default="balanced", help="Migration policy to use")
    parser.add_argument("--output", default="./migration_results", help="Output directory")
    
    args = parser.parse_args()
    
    # Load topology configuration
    with open(args.topology, 'r') as f:
        topology_config = yaml.safe_load(f)
        
    # Initialize migration engine
    engine = DynamicMigrationEngine(args.cxlmemsim, topology_config)
    
    if args.evaluate:
        # Evaluate all policies
        print(f"Evaluating migration policies for {args.duration} seconds each...")
        results = engine.evaluate_policies(duration_seconds=args.duration)
        
        # Generate report
        print("Generating evaluation report...")
        engine.generate_policy_report(results, Path(args.output))
        
        print(f"\nEvaluation complete. Results saved to {args.output}")
        
        # Print summary
        print("\nPolicy Evaluation Summary:")
        for policy, metrics in results.items():
            print(f"\n{policy}:")
            print(f"  Success Rate: {metrics['success_rate']:.1%}")
            print(f"  Benefit/Cost: {metrics['benefit_cost_ratio']:.2f}")
            
    else:
        # Run with specified policy
        print(f"Starting migration engine with {args.policy} policy...")
        engine.set_migration_policy(MigrationPolicy(args.policy))
        engine.start_monitoring()
        
        print("Migration engine running. Press Ctrl+C to stop.")
        try:
            while True:
                time.sleep(1)
        except KeyboardInterrupt:
            engine.stop_monitoring()
            print("\nMigration engine stopped.")


if __name__ == "__main__":
    main()