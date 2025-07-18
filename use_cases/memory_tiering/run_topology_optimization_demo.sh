#!/bin/bash
# Demo script for topology-aware hotness optimization

echo "=== CXLMemSim Topology Optimization Demo ==="
echo "This demonstrates how the system selects the best topology and strategy"
echo "based on hotness prediction, going beyond simple performance heat maps."
echo ""

# Check if CXLMemSim is built
if [ ! -f "../../build/CXLMemSim" ]; then
    echo "Error: CXLMemSim not found. Please build it first:"
    echo "  cd ../../ && mkdir -p build && cd build && cmake .. && make"
    exit 1
fi

# Create results directory
mkdir -p topology_optimization_demo_results

echo "Step 1: Running topology optimization analysis..."
echo "This will:"
echo "  - Predict hotness patterns for each workload"
echo "  - Evaluate multiple CXL topologies (flat, hierarchical, star, mesh)"
echo "  - Select the best memory management strategy"
echo "  - Provide recommendations with confidence scores"
echo ""

python3 topology_hotness_optimizer.py \
    --cxlmemsim ../../build/CXLMemSim \
    --config topology_optimization_config.yaml \
    --output ./topology_optimization_demo_results

echo ""
echo "Step 2: Demonstrating strategy selection logic..."
echo ""

python3 strategy_selector.py

echo ""
echo "=== Results Generated ==="
echo "Check the following files for detailed analysis:"
echo "  - topology_optimization_demo_results/recommendation_summary.png"
echo "  - topology_optimization_demo_results/performance_heatmap.png"
echo "  - topology_optimization_demo_results/topology_comparison.png"
echo "  - topology_optimization_demo_results/optimization_results.json"
echo ""
echo "Key Innovation: Instead of just showing a heat map of performance,"
echo "this system provides specific recommendations on:"
echo "  1. Which CXL topology to deploy"
echo "  2. Which memory management strategy to use"
echo "  3. Why these choices are optimal for your workload"
echo ""
echo "This enables data-driven deployment decisions rather than trial-and-error."