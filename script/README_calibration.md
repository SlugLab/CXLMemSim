# CXLMemSim Memory Latency Calibration

This directory contains scripts to calibrate CXLMemSim's RoBSim memory access latency parameters using gem5 simulation data as ground truth.

## Problem Statement

The current CXLMemSim implementation shows memory access latency averages that are smaller than the expected `congestion + actual memory access` latency. This calibration workflow helps tune the RoBSim parameters to match gem5 simulation results.

## Scripts Overview

### 1. `calibrate_memory_latency.py`
Main calibration script that analyzes gem5 and CXLMemSim trace files to calculate optimal parameters.

**Features:**
- Parses O3PipeView trace format from both gem5 and CXLMemSim
- Calculates latency statistics and distributions
- Determines calibration parameters to match target latency ratios
- Generates comparison plots and patch files

**Usage:**
```bash
python3 calibrate_memory_latency.py \
    --gem5-trace <gem5_trace.out> \
    --cxlmemsim-trace <cxlmemsim_trace.out> \
    --output-config <calibrated_params.json> \
    --target-ratio 1.0 \
    --output-plot <comparison.png> \
    --output-patch <rob_patch.patch>
```

### 2. `apply_calibration.py`
Applies calibrated parameters to the CXLMemSim source code.

**Usage:**
```bash
python3 apply_calibration.py \
    --config calibrated_params.json \
    --rob-file ../src/rob.cpp \
    [--dry-run]
```

### 3. `calibrate_example.sh`
Example workflow script showing how to use the calibration tools.

## Calibration Workflow

### Step 1: Generate Traces
1. Run gem5 simulation to generate O3PipeView trace:
   ```bash
   # Example gem5 command (adjust for your setup)
   gem5.opt configs/example/se.py --cpu-type=O3CPU --pipeview-trace=gem5_trace.out --cmd=<your_benchmark>
   ```

2. Run CXLMemSim to generate its trace:
   ```bash
   ./CXLMemSimRoB -t <input_trace> --output delayed_trace.out
   ```

### Step 2: Calibrate Parameters
```bash
python3 script/calibrate_memory_latency.py \
    --gem5-trace gem5_trace.out \
    --cxlmemsim-trace delayed_trace.out \
    --output-config calibrated_params.json \
    --target-ratio 1.0 \
    --output-plot latency_comparison.png
```

### Step 3: Apply Calibration
```bash
# Dry run to see what would change
python3 script/apply_calibration.py \
    --config calibrated_params.json \
    --rob-file src/rob.cpp \
    --dry-run

# Apply changes
python3 script/apply_calibration.py \
    --config calibrated_params.json \
    --rob-file src/rob.cpp
```

### Step 4: Rebuild and Test
```bash
# Rebuild CXLMemSim
cd cmake-build-debug && make

# Test with the same workload
./CXLMemSimRoB -t <input_trace> --output delayed_trace_calibrated.out

# Compare results
python3 script/calibrate_memory_latency.py \
    --gem5-trace gem5_trace.out \
    --cxlmemsim-trace delayed_trace_calibrated.out \
    --output-config verification_params.json \
    --output-plot verification_plot.png
```

## Configuration Parameters

The calibration generates a JSON configuration file with the following parameters:

- **`base_latency_multiplier`**: Multiplier for the base memory latency calculation
- **`congestion_factor`**: Factor for congestion-related latency (currently kept at 1.0)
- **`instruction_latency_adjustment`**: Per-instruction type latency adjustments
- **`min_latency_threshold`**: Minimum latency threshold for memory accesses
- **`stall_multiplier`**: Multiplier for ROB stall counting
- **`target_ratio`**: Target ratio of CXLMemSim/gem5 latency

## Key Areas Modified in RoBSim

The calibration modifies the following aspects of `rob.cpp`:

1. **Base Latency Calculation** (`canRetire` function):
   - Applies multiplier to `controller_->calculate_latency()` result
   - Adjusts minimum latency threshold

2. **Instruction-Specific Latencies**:
   - Adds per-instruction type adjustment factors
   - Modifies the instruction latency map lookup

3. **Stall Counting**:
   - Applies calibrated multiplier to stall count increments
   - Ensures stall events match expected congestion behavior

## Understanding the Calibration

### Memory Access Latency Components
```
Total Latency = Base Latency + Congestion + Instruction Overhead
```

Where:
- **Base Latency**: Fundamental memory access time (DRAM, CXL, etc.)
- **Congestion**: Network/controller congestion delays
- **Instruction Overhead**: CPU pipeline and instruction-specific delays

### Calibration Strategy
1. **Analyze gem5 Results**: Extract memory access latencies from gem5 traces
2. **Compare with CXLMemSim**: Identify discrepancies in latency distributions
3. **Calculate Adjustments**: Determine multipliers and offsets to match gem5
4. **Validate**: Apply parameters and verify improved accuracy

## Dependencies

- Python 3.6+
- NumPy
- Matplotlib
- Standard library modules: re, json, pathlib, argparse, logging

## Troubleshooting

### Common Issues

1. **Trace Parsing Errors**: Ensure trace files are in O3PipeView format
2. **No Memory Instructions**: Check that traces contain memory access instructions
3. **Compilation Errors**: Verify rob.cpp syntax after applying calibration

### Debug Tips

- Use `--dry-run` with `apply_calibration.py` to preview changes
- Check backup files (`.cpp.backup`) if issues occur
- Use smaller trace files for initial testing
- Enable debug logging by modifying log level in scripts

## Examples

### Basic Calibration
```bash
# Simple calibration for exact match to gem5
python3 calibrate_memory_latency.py \
    --gem5-trace gem5.out \
    --cxlmemsim-trace cxlmemsim.out \
    --output-config params.json \
    --target-ratio 1.0
```

### Conservative Calibration
```bash
# 10% higher latency than gem5 (more conservative)
python3 calibrate_memory_latency.py \
    --gem5-trace gem5.out \
    --cxlmemsim-trace cxlmemsim.out \
    --output-config params.json \
    --target-ratio 1.1
```

### Full Analysis with Plots
```bash
# Complete analysis with visualization
python3 calibrate_memory_latency.py \
    --gem5-trace gem5.out \
    --cxlmemsim-trace cxlmemsim.out \
    --output-config params.json \
    --target-ratio 1.0 \
    --output-plot analysis.png \
    --output-patch rob.patch
```

## Contact

For questions or issues with the calibration scripts, please refer to the main CXLMemSim documentation or submit an issue to the project repository.