#!/bin/bash
# Continuous Integration Script for CXLMemSim Production Profiling

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CXLMEMSIM_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Build CXLMemSim if needed
if [ ! -f "$CXLMEMSIM_ROOT/build/CXLMemSim" ]; then
    echo "Building CXLMemSim..."
    cd "$CXLMEMSIM_ROOT"
    mkdir -p build && cd build
    cmake .. && make -j$(nproc)
fi

# Function to run profiling and check for regressions
run_profiling_suite() {
    local suite_config=$1
    local baseline_results=$2
    local output_dir=$3
    
    echo "Running production profiling suite..."
    python3 "$SCRIPT_DIR/production_profiler.py" \
        --cxlmemsim "$CXLMEMSIM_ROOT/build/CXLMemSim" \
        --config "$suite_config" \
        --output "$output_dir"
    
    # Check for performance regressions if baseline exists
    if [ -f "$baseline_results" ]; then
        python3 "$SCRIPT_DIR/regression_checker.py" \
            --baseline "$baseline_results" \
            --current "$output_dir/profiling_results_*.json" \
            --threshold 10  # 10% regression threshold
    fi
}

# Main CI workflow
main() {
    local timestamp=$(date +%Y%m%d_%H%M%S)
    local output_dir="$SCRIPT_DIR/ci_results/$timestamp"
    
    mkdir -p "$output_dir"
    
    # Run different profiling suites
    echo "Starting CI profiling at $timestamp"
    
    # Quick smoke test
    run_profiling_suite \
        "$SCRIPT_DIR/ci_configs/smoke_test.yaml" \
        "$SCRIPT_DIR/baselines/smoke_baseline.json" \
        "$output_dir/smoke"
    
    # Full regression suite (if not a PR)
    if [ "$CI_EVENT" != "pull_request" ]; then
        run_profiling_suite \
            "$SCRIPT_DIR/ci_configs/full_suite.yaml" \
            "$SCRIPT_DIR/baselines/full_baseline.json" \
            "$output_dir/full"
    fi
    
    # Generate CI report
    python3 "$SCRIPT_DIR/ci_report_generator.py" \
        --results "$output_dir" \
        --output "$output_dir/ci_report.html"
    
    echo "CI profiling completed. Results in $output_dir"
}

# Check dependencies
check_dependencies() {
    local deps=("python3" "cmake" "make")
    for dep in "${deps[@]}"; do
        if ! command -v "$dep" &> /dev/null; then
            echo "Error: $dep is required but not installed"
            exit 1
        fi
    done
    
    # Check Python packages
    python3 -c "import pandas, matplotlib, yaml" || {
        echo "Installing required Python packages..."
        pip3 install pandas matplotlib pyyaml
    }
}

check_dependencies
main "$@"