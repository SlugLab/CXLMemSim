# CXLMemSim Use Cases: Advanced Topology and Hotness-Based Optimization

This directory contains three cutting-edge use case implementations that leverage CXLMemSim's capabilities for intelligent CXL memory system optimization based on topology awareness and hotness prediction.

## 🚀 Use Cases Overview

### 1. Topology-Guided Hardware Procurement (`topology_guided_procurement/`)
**Purpose**: Make data-driven hardware purchasing decisions based on workload hotness patterns and topology optimization

- **Key Innovation**: Predicts optimal CXL topology configurations before hardware purchase
- **Key Benefits**: 
  - Evaluates hardware options based on predicted workload hotness distributions
  - Compares different topology types (flat, hierarchical, star, mesh) for your specific workloads
  - Provides TCO analysis including performance-per-dollar metrics
  - Generates procurement recommendations with confidence scores

### 2. Predictive Topology and Placement Optimization (`predictive_placement/`)
**Purpose**: Uses ML to predict optimal data placement across CXL topology based on access patterns

- **Key Innovation**: Deep learning model predicts future access patterns and optimizes placement
- **Key Benefits**:
  - Analyzes page-level access patterns to predict hotness
  - Recommends data placement across CXL endpoints for optimal performance
  - Provides migration plans with prioritization based on expected benefit
  - Considers topology structure in placement decisions

### 3. Dynamic Migration Policy Engine (`dynamic_migration/`)
**Purpose**: Implements intelligent data migration policies with real-time hotness monitoring

- **Key Innovation**: Adaptive policies that learn from migration outcomes and adjust strategies
- **Key Benefits**:
  - Multiple migration policies: Conservative, Balanced, Aggressive, Adaptive, Predictive
  - Real-time trigger detection: hotness thresholds, load imbalance, congestion
  - Policy evaluation framework to find optimal strategy for your workload
  - Continuous learning from migration success/failure

## 📋 Prerequisites

### System Requirements
- Linux with kernel 5.15+ (CXL support)
- Python 3.8+
- Built CXLMemSim binary
- At least 16GB RAM for ML model training

### Python Dependencies
```bash
pip install pandas matplotlib numpy pyyaml scikit-learn torch seaborn
```

### Building CXLMemSim
```bash
cd /path/to/CXLMemSim
mkdir -p build && cd build
cmake .. && make -j$(nproc)
```

## 🏃 Quick Start

### 1. Topology-Guided Hardware Procurement

```bash
cd use_cases/topology_guided_procurement
python3 topology_procurement_advisor.py \
    --cxlmemsim ../../build/CXLMemSim \
    --workloads workload_requirements.yaml \
    --constraints procurement_constraints.yaml \
    --output ./procurement_results
```

**Example Output**: 
- Hardware comparison charts showing performance vs cost
- TCO analysis over 3 years
- Ranked recommendations based on workload fit
- Risk assessment for each option

### 2. Predictive Topology and Placement Optimization

```bash
cd use_cases/predictive_placement
python3 topology_placement_predictor.py \
    --cxlmemsim ../../build/CXLMemSim \
    --topology topology_config.yaml \
    --workload workload_trace.yaml \
    --output ./placement_results
```

**Example Output**:
- Placement distribution visualizations
- Migration flow matrix
- Prioritized migration plan with expected improvements
- Performance impact predictions

### 3. Dynamic Migration Policy Evaluation

```bash
cd use_cases/dynamic_migration
# Evaluate all policies
python3 migration_policy_engine.py \
    --cxlmemsim ../../build/CXLMemSim \
    --topology migration_topology.yaml \
    --evaluate \
    --duration 300 \
    --output ./migration_results

# Or run with specific policy
python3 migration_policy_engine.py \
    --cxlmemsim ../../build/CXLMemSim \
    --topology migration_topology.yaml \
    --policy adaptive \
    --output ./migration_results
```

**Example Output**:
- Policy comparison charts (success rate, efficiency, activity)
- Migration pattern analysis
- Policy recommendations based on workload characteristics
- Real-time migration statistics

## 📊 Key Advantages

| Use Case | Traditional Approach | CXLMemSim Advantage |
|----------|---------------------|---------------------|
| **Hardware Procurement** | Overprovisioning based on peak load | Precise sizing based on hotness prediction |
| **Data Placement** | Static allocation or simple LRU | ML-based prediction of future access patterns |
| **Migration Policy** | Fixed thresholds | Adaptive policies that learn and improve |

## 🔧 Configuration Guide

### Hardware Procurement Configuration
```yaml
workloads:
  - name: "production_database"
    type: "database"
    working_set_size: 800  # GB
    latency_critical: true
    annual_growth_rate: 0.3
    
constraints:
  max_budget: 150000
  min_performance_score: 0.7
  max_power_consumption_kw: 5.0
```

### Placement Optimization Configuration
```yaml
pages:
  - id: 1001
    access_count: 10000
    pattern: "temporal"
    current_location: "endpoint_3"
    heat_score: 0.95
```

### Migration Policy Configuration
```yaml
thresholds:
  hotness_trigger: 0.8
  imbalance_trigger: 0.3
  congestion_trigger: 0.9
  
policy_parameters:
  adaptive:
    learning_rate: 0.1
    history_window: 100
```

## 📈 Understanding Results

### Procurement Recommendations
- **Performance Score**: 0-1 scale, higher is better
- **TCO**: Total cost including power over 3 years
- **Topology Fit**: How well the topology matches your workload's hotness distribution

### Placement Optimization
- **Expected Improvement**: Percentage performance gain from migration
- **Confidence Score**: Reliability of the prediction (0-1)
- **Migration Priority**: High/Medium/Low based on benefit-cost ratio

### Migration Policies
- **Success Rate**: Percentage of migrations that improved performance
- **Benefit/Cost Ratio**: Performance gain per unit of migration overhead
- **Trigger Distribution**: Which conditions cause migrations

## 🔬 Advanced Features

### 1. Custom Hotness Prediction Models
```python
# Train your own hotness predictor
predictor.train_hotness_predictor(training_data)
```

### 2. Multi-Workload Optimization
```yaml
# Optimize for multiple concurrent workloads
workloads:
  - name: "oltp_database"
    weight: 0.6
  - name: "analytics_batch"
    weight: 0.4
```

### 3. Power-Aware Migration
```python
# Consider power consumption in migration decisions
engine.set_power_budget(max_watts=1000)
```

## 🚦 Integration Examples

### Kubernetes Integration
```yaml
apiVersion: v1
kind: ConfigMap
metadata:
  name: cxl-topology-optimizer
data:
  policy: "adaptive"
  hotness_threshold: "0.8"
```

### Prometheus Metrics Export
```python
# Export migration metrics to Prometheus
from prometheus_client import Counter, Histogram

migration_counter = Counter('cxl_migrations_total', 
                          'Total CXL memory migrations')
migration_duration = Histogram('cxl_migration_duration_seconds',
                             'Migration duration distribution')
```

## 🔍 Troubleshooting

### Common Issues

1. **ML Model Training Fails**: Ensure sufficient training data (>100 samples)
2. **Migration Thrashing**: Increase cooldown period in policy parameters
3. **Poor Placement Predictions**: Check if workload patterns match training data
4. **High Migration Overhead**: Reduce concurrent migration limit

### Performance Tuning

1. **Hotness Threshold**: Start with 0.8, decrease if too few migrations
2. **Migration Batch Size**: Balance between throughput and latency impact
3. **Prediction Horizon**: Longer horizons need more historical data
4. **Policy Selection**: Start with Balanced, move to Adaptive after learning

## 📚 References

- [Hotness-Aware Memory Tiering](https://arxiv.org/abs/hotness-tiering)
- [CXL Topology Optimization](https://cxl-topology-paper.com)
- [Adaptive Migration Policies](https://adaptive-migration.org)
- [ML for Memory Placement](https://ml-memory-placement.edu)

## 🤝 Contributing

We welcome contributions! Areas of interest:
- Additional migration policies
- Enhanced ML models for hotness prediction
- Integration with cloud orchestrators
- Real hardware validation results

---

For questions or support, please open an issue in the CXLMemSim repository.