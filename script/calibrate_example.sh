#!/bin/bash
# Example usage of the memory latency calibration script

# Set paths (adjust these to your actual file locations)
GEM5_TRACE="./result/trace_simple_loop_no_dep.out"  # gem5 trace file 
CXLMEMSIM_TRACE="./result/delayed_trace.out"  # CXLMemSim output trace
OUTPUT_CONFIG="calibrated_params.json"
OUTPUT_PLOT="latency_comparison.png"
OUTPUT_PATCH="rob_calibrated.patch"

# Run calibration with target ratio of 1.0 (exact match to gem5)
python3 ./CXLMemSim/script/calibrate_memory_latency.py \
    --gem5-trace "$GEM5_TRACE" \
    --cxlmemsim-trace "$CXLMEMSIM_TRACE" \
    --output-config "$OUTPUT_CONFIG" \
    --target-ratio 1.0 \
    --output-plot "$OUTPUT_PLOT" \
    --output-patch "$OUTPUT_PATCH"

echo "Calibration complete!"
echo "Results:"
echo "  - Parameters: $OUTPUT_CONFIG"
echo "  - Comparison plot: $OUTPUT_PLOT"
echo "  - ROB patch: $OUTPUT_PATCH"

# Show the calibrated parameters
echo ""
echo "Calibrated parameters:"
cat "$OUTPUT_CONFIG"