#!/bin/bash

# Test script for endpoint hotness-aware memory tiering

echo "Testing endpoint hotness-aware memory tiering..."

# Check if CXLMemSim binary exists
CXLMEMSIM_PATH="../../build/cxlmemsim"
if [ ! -f "$CXLMEMSIM_PATH" ]; then
    echo "Error: CXLMemSim binary not found at $CXLMEMSIM_PATH"
    echo "Please build CXLMemSim first"
    exit 1
fi

# Create output directory
OUTPUT_DIR="./test_hotness_results"
mkdir -p $OUTPUT_DIR

# Run test with test configuration
echo "Running test with endpoint hotness demo configuration..."
python3 tiering_policy_engine.py \
    --cxlmemsim $CXLMEMSIM_PATH \
    --config endpoint_hotness_demo_config.yaml \
    --output $OUTPUT_DIR

# Check if output files were generated
if [ -f "$OUTPUT_DIR/policy_comparison.png" ] && [ -f "$OUTPUT_DIR/endpoint_hotness_analysis.png" ]; then
    echo "Success! Test completed successfully."
    echo "Results saved to $OUTPUT_DIR"
    echo ""
    echo "Generated files:"
    ls -la $OUTPUT_DIR
else
    echo "Error: Expected output files not generated"
    exit 1
fi