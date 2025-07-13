# CXLMemSim Use Cases Implementation

This directory contains three practical use case implementations that demonstrate CXLMemSim's advantages over traditional simulators like gem5 for real-world CXL memory evaluation.

## 🚀 Use Cases Overview

### 1. Production Workload Profiling (`production_profiling/`)
**Performance advantage over gem5**: Orders of magnitude faster execution enables profiling of real production applications

- **Purpose**: Profile production applications with various CXL configurations at scale
- **Key Benefits**: 
  - Works with closed-source binaries and large datasets
  - Runs continuous performance regression testing in CI/CD pipelines
  - Enables realistic workload analysis in practical timeframes

### 2. Hardware Procurement Decision Support (`procurement_decision/`)
**Accuracy advantage**: Calibrated against gem5 for small workloads, then scaled to full applications

- **Purpose**: Make data-driven CXL hardware purchasing decisions
- **Key Benefits**:
  - Evaluate multiple hardware configurations rapidly
  - Cost/performance/power analysis with TCO calculations
  - Risk-free evaluation before hardware investment

### 3. Dynamic Memory Tiering Policies (`memory_tiering/`)
**Speed + Accuracy**: Fast enough for iterative policy development with sufficient accuracy for meaningful results

- **Purpose**: Develop and test intelligent memory placement and migration algorithms
- **Key Benefits**:
  - Machine learning-based policy optimization
  - Adaptive policies that learn from workload behavior
  - Rapid prototyping of tiering strategies

## 📋 Prerequisites

### System Requirements
- Linux with CXL support (kernel 6.11+)
- Python 3.8+
- Built CXLMemSim binary

### Python Dependencies
```bash
pip install pandas matplotlib numpy pyyaml scikit-learn
```

### Building CXLMemSim
```bash
cd /path/to/CXLMemSim
mkdir -p build && cd build
cmake .. && make -j$(nproc)
```

## 🏃 Quick Start

### 1. Production Workload Profiling

```bash
# Run example profiling suite
cd use_cases/production_profiling
python3 production_profiler.py \
    --cxlmemsim ../../build/CXLMemSim \
    --config example_suite.yaml \
    --output ./results

# For CI/CD integration
./ci_profiling.sh
```

**Example Output**: Performance comparison charts, regression detection, and automated reports suitable for continuous integration workflows.

### 2. Hardware Procurement Analysis

```bash
# Run procurement decision analysis
cd use_cases/procurement_decision
python3 procurement_analyzer.py \
    --cxlmemsim ../../build/CXLMemSim \
    --config example_procurement.yaml \
    --output ./procurement_results
```

**Example Output**: 
- Cost vs performance scatter plots
- TCO analysis over 3-5 years
- Hardware recommendations based on requirements
- Power consumption estimates

### 3. Memory Tiering Policy Evaluation

```bash
# Compare different tiering policies
cd use_cases/memory_tiering
python3 tiering_policy_engine.py \
    --cxlmemsim ../../build/CXLMemSim \
    --config example_tiering_config.yaml \
    --output ./tiering_results
```

**Example Output**: Policy performance comparisons, ML-based optimization, and adaptive learning results.

## 📊 Key Advantages Over gem5

| Aspect | CXLMemSim | gem5 | Advantage |
|--------|-----------|------|-----------|
| **Speed** | Orders of magnitude faster | Cycle-accurate (slow) | ✅ Production-scale evaluation |
| **Real Applications** | Any binary | Requires porting | ✅ Closed-source support |
| **CI/CD Integration** | Minutes per run | Hours to days | ✅ Automated testing |
| **Hardware Variety** | Easy configuration | Complex setup | ✅ Rapid prototyping |
| **Calibration** | Against gem5/hardware | Self-contained | ✅ Accuracy when needed |

## 🔧 Configuration Guide

### Production Profiling Configuration
```yaml
workloads:
  - name: "my_app"
    binary: "/path/to/application"
    args: ["--config", "production.conf"]
    timeout: 300

cxl_configurations:
  - name: "baseline"
    capacity: [100, 0]  # All local
  - name: "cxl_50_50"
    capacity: [50, 50]
    latency: [150, 180]
    bandwidth: [30000, 25000]
```

### Procurement Analysis Configuration
```yaml
hardware_configurations:
  - name: "config_name"
    local_memory_gb: 256
    cxl_memory_gb: 256
    cxl_latency_ns: 150
    cxl_bandwidth_gbps: 64
    base_system_cost: 8000
    cxl_cost_per_gb: 4

requirements:
  max_budget: 25000
  min_performance: 5.0
  optimization_weights:
    performance: 0.4
    cost: 0.4
    power: 0.2
```

### Tiering Policy Configuration
```yaml
workloads:
  - name: "workload_name"
    type: "database"  # or "analytics", "web"
    binary: "./my_app"
    tier_capacities: [128, 128]  # GB per tier

policies_to_evaluate:
  - "static_balanced"
  - "hotness_based"
  - "ml_based"
  - "adaptive"
```

## 📈 Interpreting Results

### Production Profiling Results
- **Performance Trends**: Identify optimal CXL configurations per workload
- **Regression Detection**: Catch performance degradations in CI
- **Scaling Insights**: Understand which applications benefit from CXL

### Procurement Analysis Results
- **Cost Efficiency**: Find sweet spot between performance and cost
- **TCO Projections**: Include electricity and maintenance costs
- **Recommendation Engine**: Data-driven hardware selection

### Tiering Policy Results
- **Policy Performance**: Compare static vs adaptive vs ML approaches
- **Workload Sensitivity**: Understand which policies work for which workloads
- **Optimization Opportunities**: Identify areas for custom policy development

## 🧪 Validation and Calibration

### Calibrating Against gem5
```bash
# Generate calibration data with gem5 (small workloads)
python3 script/calibrate_memory_latency.py \
    --gem5-binary /path/to/gem5 \
    --workload ./microbench/simple \
    --output calibration.json

# Use calibration in CXLMemSim
python3 production_profiler.py \
    --calibration calibration.json \
    --cxlmemsim ./build/CXLMemSim
```

### Validating Against Real Hardware
When CXL hardware is available, validate CXLMemSim predictions:
1. Run identical workloads on real CXL systems
2. Compare latency and bandwidth characteristics
3. Adjust CXLMemSim parameters for better accuracy

## 🚦 Integration Examples

### GitHub Actions CI/CD
```yaml
name: CXL Performance Regression
on: [push, pull_request]
jobs:
  cxl-performance:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v3
      - name: Run CXL profiling
        run: ./use_cases/production_profiling/ci_profiling.sh
      - name: Upload results
        uses: actions/upload-artifact@v3
        with:
          name: cxl-performance-results
          path: use_cases/production_profiling/ci_results/
```

### Jenkins Pipeline
```groovy
pipeline {
    agent any
    stages {
        stage('CXL Performance Test') {
            steps {
                sh '''
                    cd use_cases/production_profiling
                    python3 production_profiler.py \
                        --cxlmemsim ../../build/CXLMemSim \
                        --config ci_configs/regression_suite.yaml \
                        --output ${WORKSPACE}/cxl_results
                '''
            }
            post {
                always {
                    archiveArtifacts artifacts: 'cxl_results/**/*'
                }
            }
        }
    }
}
```

## 🔍 Troubleshooting

### Common Issues

1. **Simulation Timeout**: Increase timeout values in configuration
2. **Missing Dependencies**: Install required Python packages
3. **Permission Errors**: Ensure CXLMemSim binary is executable
4. **Configuration Errors**: Validate YAML syntax and parameter ranges

### Performance Tips

1. **Parallel Execution**: Use `parallel_jobs` parameter for faster evaluation
2. **Shorter Intervals**: Reduce simulation intervals for quicker feedback
3. **Selective Policies**: Focus on most promising policies first
4. **Incremental Analysis**: Start with small workloads, scale up gradually

### Getting Help

- Check CXLMemSim logs with `SPDLOG_LEVEL=debug`  
- Validate configurations with smaller test workloads first
- Compare results with known baselines
- File issues with reproduction steps and configuration files

## 📚 Further Reading

- [CXLMemSim Paper](https://arxiv.org/abs/2303.06153) - Original research paper
- [CXL Specification](https://computeexpresslink.org/) - CXL protocol details
- [Memory Tiering Best Practices](docs/memory_tiering_guide.md) - Advanced policy development
- [Hardware Selection Guide](docs/hardware_procurement_guide.md) - CXL hardware evaluation criteria