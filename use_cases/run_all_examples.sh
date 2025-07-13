#!/bin/bash
# Comprehensive example runner for all CXLMemSim use cases

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CXLMEMSIM_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CXLMEMSIM_BINARY="$CXLMEMSIM_ROOT/build/CXLMemSim"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

log_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Check prerequisites
check_prerequisites() {
    log_info "Checking prerequisites..."
    
    # Check CXLMemSim binary
    if [ ! -f "$CXLMEMSIM_BINARY" ]; then
        log_error "CXLMemSim binary not found at $CXLMEMSIM_BINARY"
        log_info "Building CXLMemSim..."
        cd "$CXLMEMSIM_ROOT"
        mkdir -p build && cd build
        cmake .. && make -j$(nproc)
        if [ ! -f "$CXLMEMSIM_BINARY" ]; then
            log_error "Failed to build CXLMemSim"
            exit 1
        fi
    fi
    
    # Check Python dependencies
    python3 -c "import pandas, matplotlib, numpy, yaml, sklearn" 2>/dev/null || {
        log_warning "Installing missing Python dependencies..."
        pip3 install pandas matplotlib numpy pyyaml scikit-learn
    }
    
    # Check for microbench binary
    if [ ! -f "$CXLMEMSIM_ROOT/microbench/ld" ]; then
        log_warning "Microbench binary not found. Some examples may not work."
    fi
    
    log_success "Prerequisites checked"
}

# Run production profiling example
run_production_profiling() {
    local output_dir="$SCRIPT_DIR/example_results/production_profiling"
    
    log_info "Running Production Workload Profiling example..."
    
    cd "$SCRIPT_DIR/production_profiling"
    
    # Create a minimal test configuration if microbench doesn't exist
    if [ ! -f "$CXLMEMSIM_ROOT/microbench/ld" ]; then
        log_warning "Creating simplified test configuration..."
        cat > test_suite.yaml << EOF
parallel_jobs: 2

workloads:
  - name: "simple_test"
    binary: "/bin/sleep"
    args: ["1"]
    interval: 1
    timeout: 10

cxl_configurations:
  - name: "baseline_dram"
    dram_latency: 85
    capacity: [100]
    topology: "(1)"
    
  - name: "cxl_basic"
    dram_latency: 85
    latency: [150, 180]
    bandwidth: [30000, 20000]
    capacity: [50, 50]
    topology: "(1,(2))"
EOF
        CONFIG_FILE="test_suite.yaml"
    else
        CONFIG_FILE="example_suite.yaml"
    fi
    
    mkdir -p "$output_dir"
    
    python3 production_profiler.py \
        --cxlmemsim "$CXLMEMSIM_BINARY" \
        --config "$CONFIG_FILE" \
        --output "$output_dir" || {
        log_error "Production profiling failed"
        return 1
    }
    
    log_success "Production profiling completed. Results in $output_dir"
}

# Run procurement decision example
run_procurement_decision() {
    local output_dir="$SCRIPT_DIR/example_results/procurement_decision"
    
    log_info "Running Hardware Procurement Decision Support example..."
    
    cd "$SCRIPT_DIR/procurement_decision"
    
    # Create simplified config if needed
    if [ ! -f "$CXLMEMSIM_ROOT/microbench/ld" ]; then
        log_warning "Creating simplified procurement test configuration..."
        cat > test_procurement.yaml << EOF
workloads:
  - name: "test_workload"
    binary: "/bin/sleep"
    args: ["2"]
    interval: 1
    metric_weights:
      execution_time: 0.5
      throughput: 0.3
      latency: 0.2

hardware_configurations:
  - name: "baseline_config"
    local_memory_gb: 128
    cxl_memory_gb: 0
    local_dram_latency: 85
    base_system_cost: 5000
    dram_cost_per_gb: 8
    base_power: 200
    topology: "(1)"
    memory_distribution: [100]
    
  - name: "cxl_config"
    local_memory_gb: 64
    cxl_memory_gb: 64
    local_dram_latency: 85
    cxl_latency_ns: 150
    cxl_bandwidth_gbps: 32
    base_system_cost: 5000
    dram_cost_per_gb: 8
    cxl_cost_per_gb: 4
    cxl_device_cost: 500
    base_power: 200
    topology: "(1,(2))"
    memory_distribution: [50, 50]

requirements:
  max_budget: 15000
  min_performance: 1.0
  optimization_weights:
    performance: 0.4
    cost: 0.4
    power: 0.2

tco_parameters:
  years: 3
  electricity_cost_per_kwh: 0.12
EOF
        CONFIG_FILE="test_procurement.yaml"
    else
        CONFIG_FILE="example_procurement.yaml"
    fi
    
    mkdir -p "$output_dir"
    
    python3 procurement_analyzer.py \
        --cxlmemsim "$CXLMEMSIM_BINARY" \
        --config "$CONFIG_FILE" \
        --output "$output_dir" || {
        log_error "Procurement analysis failed"
        return 1
    }
    
    log_success "Procurement analysis completed. Results in $output_dir"
}

# Run memory tiering example
run_memory_tiering() {
    local output_dir="$SCRIPT_DIR/example_results/memory_tiering"
    
    log_info "Running Dynamic Memory Tiering Policy example..."
    
    cd "$SCRIPT_DIR/memory_tiering"
    
    # Create simplified config if needed
    if [ ! -f "$CXLMEMSIM_ROOT/microbench/ld" ]; then
        log_warning "Creating simplified tiering test configuration..."
        cat > test_tiering_config.yaml << EOF
evaluation_duration: 30

workloads:
  - name: "test_memory_workload"
    type: "general"
    binary: "/bin/sleep"
    args: ["2"]
    interval: 2
    dram_latency: 85
    latency: [150, 180]
    bandwidth: [30000, 25000]
    tier_capacities: [64, 64]
    topology: "(1,(2))"
    read_write_ratio: 0.7
    working_set_size: 32

policies_to_evaluate:
  - "static_balanced"
  - "static_local_heavy"
  - "hotness_based"

ml_training_data:
  - memory_intensity: 0.5
    access_locality: 0.7
    read_write_ratio: 0.7
    working_set_size: 32
    cache_miss_rate: 0.2
    optimal_allocation: [0.5, 0.5]
EOF
        CONFIG_FILE="test_tiering_config.yaml"
    else
        CONFIG_FILE="example_tiering_config.yaml"
    fi
    
    mkdir -p "$output_dir"
    
    python3 tiering_policy_engine.py \
        --cxlmemsim "$CXLMEMSIM_BINARY" \
        --config "$CONFIG_FILE" \
        --output "$output_dir" || {
        log_error "Memory tiering evaluation failed"
        return 1
    }
    
    log_success "Memory tiering evaluation completed. Results in $output_dir"
}

# Generate summary report
generate_summary() {
    local results_dir="$SCRIPT_DIR/example_results"
    local summary_file="$results_dir/SUMMARY.md"
    
    log_info "Generating summary report..."
    
    mkdir -p "$results_dir"
    
    cat > "$summary_file" << EOF
# CXLMemSim Use Cases - Example Run Summary

Generated on: $(date)

## Use Case Results

### 1. Production Workload Profiling
**Status**: $([ -d "$results_dir/production_profiling" ] && echo "✅ Completed" || echo "❌ Failed")
**Location**: \`$results_dir/production_profiling/\`
**Key Outputs**:
- Performance comparison charts
- Profiling results JSON
- CI/CD integration examples

### 2. Hardware Procurement Decision Support  
**Status**: $([ -d "$results_dir/procurement_decision" ] && echo "✅ Completed" || echo "❌ Failed")
**Location**: \`$results_dir/procurement_decision/\`
**Key Outputs**:
- Cost vs performance analysis
- TCO calculations
- Hardware recommendations
- Power consumption estimates

### 3. Dynamic Memory Tiering Policies
**Status**: $([ -d "$results_dir/memory_tiering" ] && echo "✅ Completed" || echo "❌ Failed")
**Location**: \`$results_dir/memory_tiering/\`
**Key Outputs**:
- Policy performance comparisons
- ML-based optimization results
- Adaptive learning analysis

## Key Takeaways

1. **Speed Advantage**: CXLMemSim enables practical evaluation of production workloads
2. **Cost Analysis**: Data-driven hardware procurement decisions with TCO modeling
3. **Policy Innovation**: Rapid development and testing of memory tiering strategies

## Next Steps

1. Customize configurations for your specific workloads
2. Integrate profiling into your CI/CD pipeline
3. Calibrate against gem5 for critical accuracy requirements
4. Develop custom memory tiering policies for your applications

## Results Directory Structure

\`\`\`
example_results/
├── production_profiling/
│   ├── profiling_results_*.json
│   ├── profiling_report_*.png
│   └── profiling_summary_*.txt
├── procurement_decision/
│   ├── procurement_analysis.json
│   ├── procurement_analysis.png
│   ├── tco_analysis.png
│   └── tco_analysis.csv
└── memory_tiering/
    ├── policy_comparison.json
    ├── policy_comparison.png
    └── policy_summary_stats.csv
\`\`\`

For detailed information about each use case, see the individual README files and configuration examples.
EOF

    log_success "Summary report generated: $summary_file"
}

# Main execution
main() {
    echo "=================================================="
    echo "CXLMemSim Use Cases - Comprehensive Example Runner"
    echo "=================================================="
    echo
    
    local start_time=$(date +%s)
    local failed_tests=0
    
    check_prerequisites
    echo
    
    # Run all use cases
    log_info "Starting all use case examples..."
    echo
    
    run_production_profiling || ((failed_tests++))
    echo
    
    run_procurement_decision || ((failed_tests++))
    echo
    
    run_memory_tiering || ((failed_tests++))
    echo
    
    generate_summary
    echo
    
    local end_time=$(date +%s)
    local duration=$((end_time - start_time))
    
    echo "=================================================="
    if [ $failed_tests -eq 0 ]; then
        log_success "All use case examples completed successfully!"
        log_success "Total execution time: ${duration} seconds"
        log_success "Results available in: $SCRIPT_DIR/example_results/"
    else
        log_warning "$failed_tests use case(s) failed"
        log_info "Check individual error messages above for details"
        log_info "Partial results available in: $SCRIPT_DIR/example_results/"
    fi
    echo "=================================================="
    
    # Show available results
    if [ -d "$SCRIPT_DIR/example_results" ]; then
        echo
        log_info "Generated files:"
        find "$SCRIPT_DIR/example_results" -type f -name "*.png" -o -name "*.json" -o -name "*.csv" -o -name "*.md" | head -20
        if [ $(find "$SCRIPT_DIR/example_results" -type f | wc -l) -gt 20 ]; then
            log_info "... and more files in $SCRIPT_DIR/example_results/"
        fi
    fi
    
    return $failed_tests
}

# Handle command line arguments
case "${1:-}" in
    "production"|"profiling")
        check_prerequisites && run_production_profiling
        ;;
    "procurement"|"decision")
        check_prerequisites && run_procurement_decision
        ;;
    "tiering"|"memory")
        check_prerequisites && run_memory_tiering
        ;;
    "summary")
        generate_summary
        ;;
    "help"|"-h"|"--help")
        echo "Usage: $0 [use_case]"
        echo ""
        echo "Available use cases:"
        echo "  production    - Run production workload profiling example"
        echo "  procurement   - Run hardware procurement decision support example"  
        echo "  tiering       - Run memory tiering policy evaluation example"
        echo "  summary       - Generate summary report only"
        echo "  (no args)     - Run all examples"
        echo ""
        echo "Examples:"
        echo "  $0                    # Run all use cases"
        echo "  $0 production         # Run only production profiling"
        echo "  $0 procurement        # Run only procurement analysis"
        echo "  $0 tiering           # Run only memory tiering"
        ;;
    *)
        main "$@"
        ;;
esac