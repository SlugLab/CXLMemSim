#!/bin/bash
#
# Run Switch Accuracy Experiment (Section 5.2.3)
#
# Usage:
#   ./run_switch_experiment.sh [OPTIONS]
#
# Examples:
#   ./run_switch_experiment.sh                    # Full experiment
#   ./run_switch_experiment.sh --quick            # Quick test
#   ./run_switch_experiment.sh -t no_switch two_level -w ptr-chasing
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build"

echo "=============================================="
echo "CXLMemSim Switch Accuracy Experiment"
echo "Section 5.2.3: Switch Modeling Validation"
echo "=============================================="
echo ""

# Check if running as root (required for perf events)
if [ "$EUID" -ne 0 ]; then
    echo "Warning: Not running as root. Some features may not work."
    echo "Consider running with: sudo $0 $@"
    echo ""
fi

# Create build directory if needed
if [ ! -d "$BUILD_DIR" ]; then
    echo "Creating build directory..."
    mkdir -p "$BUILD_DIR"
fi

# Build if CXLMemSim doesn't exist
if [ ! -f "$BUILD_DIR/CXLMemSim" ]; then
    echo "Building CXLMemSim..."
    cd "$BUILD_DIR"
    cmake ..
    make -j$(nproc)
    cd "$SCRIPT_DIR"
fi

# Create results directory
mkdir -p "$SCRIPT_DIR/results"

# Run the Python experiment script
echo "Starting experiment..."
python3 "$SCRIPT_DIR/switch_accuracy.py" "$@"

echo ""
echo "=============================================="
echo "Experiment completed!"
echo "Results saved to: $SCRIPT_DIR/results/"
echo "=============================================="
