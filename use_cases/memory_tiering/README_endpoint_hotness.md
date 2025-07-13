# Endpoint Hotness-Aware Memory Tiering

This enhancement to the memory tiering system tracks and visualizes how page hotness affects different CXL endpoints, enabling more intelligent memory placement decisions.

## Key Features

### 1. Per-Endpoint Hotness Tracking
- Tracks hotness scores for each CXL endpoint independently
- Considers workload type (database, analytics, web) to simulate realistic hotness patterns
- Records historical hotness data for analysis

### 2. Endpoint-Aware Hotness Policy
The new `endpoint_aware_hotness` policy considers:
- Individual endpoint hotness scores
- Distance penalty (farther endpoints have higher access costs)
- Dynamic allocation based on hotness distribution
- Inverse allocation strategy: less hot endpoints get more memory to balance load

### 3. Enhanced Visualizations
- **Endpoint Hotness Over Time**: Shows how hotness varies for each endpoint during execution
- **Average Hotness Distribution**: Bar charts showing average hotness per endpoint
- **Hotness Impact Analysis**: Demonstrates how hotness affects performance on different memory tiers
- **Policy Effectiveness**: Compares policies under different hotness scenarios

## Usage

### Basic Usage
```bash
python tiering_policy_engine.py \
    --cxlmemsim /path/to/cxlmemsim \
    --config endpoint_hotness_demo_config.yaml \
    --output ./hotness_results
```

### Configuration
The configuration file supports multiple CXL endpoints:
```yaml
workloads:
  - name: "multi_endpoint_workload"
    topology: "(1,(2,3,4))"  # 3 CXL endpoints
    latency: [150, 180, 200, 250]  # Per-endpoint latencies
    bandwidth: [30000, 25000, 20000, 15000]  # Per-endpoint bandwidths
```

### Policies
- `static_balanced`: Fixed 50/50 allocation
- `hotness_based`: Original hotness-based policy
- `endpoint_aware_hotness`: New policy considering per-endpoint hotness

## Hotness Patterns by Workload Type

### Database Workloads
- Periodic hot/cold cycles
- Phase-shifted patterns across endpoints
- Simulates temporal locality

### Analytics Workloads
- Bursty access patterns
- Skewed endpoint usage (first endpoint gets most traffic during bursts)
- Suitable for batch processing

### Web Workloads
- Random access with locality
- More uniform distribution across endpoints
- Simulates request-driven patterns

## Output Files

The system generates:
1. `policy_comparison.png`: Overall policy performance comparison
2. `endpoint_hotness_analysis.png`: Per-endpoint hotness visualization
3. `hotness_impact_analysis.png`: Hotness impact on different memory tiers
4. `policy_comparison.json`: Detailed results in JSON format
5. `policy_summary_stats.csv`: Summary statistics

## Implementation Details

### Hotness Calculation
```python
# Per-endpoint hotness with distance penalty
endpoint_num = int(endpoint.split('_')[1])
distance_penalty = 1.0 / (1.0 + (endpoint_num - 2) * 0.2)
weighted_score = hotness * distance_penalty * endpoint_weight_factor
```

### Allocation Strategy
- High hotness (>0.7): Prioritize local memory (60%), distribute remaining to cold endpoints
- Low hotness (<0.7): Use more CXL memory (70%), distribute evenly

## Future Enhancements
1. Real-time hotness monitoring from actual CXLMemSim output
2. Machine learning models trained on endpoint-specific patterns
3. Integration with page migration policies
4. Support for heterogeneous CXL devices with different characteristics