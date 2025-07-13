#!/usr/bin/env python3
"""
CXLMemSim Memory Access Latency Calibration Script

This script calibrates the RoBSim memory access latency parameters based on
gem5 simulation data to ensure that the average memory access latency matches
the expected congestion + actual memory access latency.

Usage:
    python calibrate_memory_latency.py --gem5-trace <gem5_trace.out> 
                                       --cxlmemsim-trace <cxlmemsim_trace.out>
                                       --output-config <calibrated_params.json>
                                       [--target-ratio <ratio>]

Author: Generated for CXLMemSim calibration
"""

import argparse
import json
import re
import numpy as np
import matplotlib.pyplot as plt
from pathlib import Path
from typing import Dict, List, Tuple, Optional
from dataclasses import dataclass
from collections import defaultdict
import logging

# Configure logging
logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(levelname)s - %(message)s')
logger = logging.getLogger(__name__)

@dataclass
class MemoryAccess:
    """Memory access instruction data"""
    fetch_timestamp: int
    retire_timestamp: int
    address: int
    instruction: str
    cycle_count: int
    mem_type: str  # 'load' or 'store'
    
    @property
    def latency(self) -> int:
        """Calculate the memory access latency"""
        return self.retire_timestamp - self.fetch_timestamp

@dataclass
class CalibrationParams:
    """Calibration parameters for RoBSim"""
    base_latency_multiplier: float = 1.0
    congestion_factor: float = 1.0
    instruction_latency_adjustment: Dict[str, float] = None
    min_latency_threshold: int = 10
    stall_multiplier: float = 1.0
    
    def __post_init__(self):
        if self.instruction_latency_adjustment is None:
            self.instruction_latency_adjustment = {}

class TraceParser:
    """Parser for O3PipeView trace format"""
    
    @staticmethod
    def parse_trace_file(trace_file: Path) -> List[MemoryAccess]:
        """
        Parse O3PipeView trace file and extract memory access instructions
        
        Args:
            trace_file: Path to the trace file
            
        Returns:
            List of MemoryAccess objects
        """
        accesses = []
        current_instruction = None
        
        with open(trace_file, 'r') as f:
            for line in f:
                line = line.strip()
                
                # Parse fetch line: O3PipeView:fetch:timestamp:address:0:cycle_count:instruction
                if line.startswith('O3PipeView:fetch:'):
                    parts = line.split(':', 6)
                    if len(parts) >= 7:
                        fetch_ts = int(parts[2])
                        address_str = parts[3]
                        cycle_count = int(parts[5])
                        instruction = parts[6].strip()
                        
                        # Convert address
                        if address_str.startswith('0x'):
                            address = int(address_str, 16)
                        else:
                            address = int(address_str) if address_str != '0x0' else 0
                        
                        current_instruction = {
                            'fetch_timestamp': fetch_ts,
                            'address': address,
                            'instruction': instruction,
                            'cycle_count': cycle_count
                        }
                
                # Parse retire line: O3PipeView:retire:timestamp:mem_type:extra
                elif line.startswith('O3PipeView:retire:') and current_instruction:
                    parts = line.split(':')
                    if len(parts) >= 4:
                        retire_ts = int(parts[2])
                        mem_type = parts[3]
                        
                        # Only include memory instructions (non-zero address)
                        if current_instruction['address'] != 0:
                            access = MemoryAccess(
                                fetch_timestamp=current_instruction['fetch_timestamp'],
                                retire_timestamp=retire_ts,
                                address=current_instruction['address'],
                                instruction=current_instruction['instruction'],
                                cycle_count=current_instruction['cycle_count'],
                                mem_type=mem_type
                            )
                            accesses.append(access)
                    
                    current_instruction = None
        
        logger.info(f"Parsed {len(accesses)} memory accesses from {trace_file}")
        return accesses

class LatencyAnalyzer:
    """Analyze memory access latencies and calculate calibration parameters"""
    
    def __init__(self):
        self.gem5_accesses: List[MemoryAccess] = []
        self.cxlmemsim_accesses: List[MemoryAccess] = []
    
    def load_traces(self, gem5_trace: Path, cxlmemsim_trace: Path):
        """Load and parse both gem5 and CXLMemSim traces"""
        self.gem5_accesses = TraceParser.parse_trace_file(gem5_trace)
        self.cxlmemsim_accesses = TraceParser.parse_trace_file(cxlmemsim_trace)
    
    def calculate_latency_statistics(self, accesses: List[MemoryAccess]) -> Dict:
        """Calculate latency statistics for a list of memory accesses"""
        if not accesses:
            return {'mean': 0, 'median': 0, 'std': 0, 'min': 0, 'max': 0, 'count': 0}
        
        latencies = [access.latency for access in accesses]
        
        return {
            'mean': np.mean(latencies),
            'median': np.median(latencies),
            'std': np.std(latencies),
            'min': np.min(latencies),
            'max': np.max(latencies),
            'count': len(latencies)
        }
    
    def analyze_instruction_types(self, accesses: List[MemoryAccess]) -> Dict[str, Dict]:
        """Analyze latencies by instruction type"""
        instruction_groups = defaultdict(list)
        
        for access in accesses:
            # Extract instruction opcode (first word)
            instr_parts = access.instruction.split()
            if instr_parts:
                opcode = instr_parts[0].lower()
                # Group similar instructions
                if 'ld' in opcode or 'mov_r_m' in opcode:
                    instruction_groups['load'].append(access.latency)
                elif 'st' in opcode or 'mov_m_r' in opcode:
                    instruction_groups['store'].append(access.latency)
                else:
                    instruction_groups['other'].append(access.latency)
        
        stats = {}
        for instr_type, latencies in instruction_groups.items():
            if latencies:
                stats[instr_type] = {
                    'mean': np.mean(latencies),
                    'median': np.median(latencies),
                    'std': np.std(latencies),
                    'count': len(latencies)
                }
        
        return stats
    
    def calculate_calibration_params(self, target_ratio: float = 1.0) -> CalibrationParams:
        """
        Calculate calibration parameters to match gem5 latencies
        
        Args:
            target_ratio: Target ratio of CXLMemSim/gem5 latency (default: 1.0 for exact match)
            
        Returns:
            CalibrationParams object with suggested parameters
        """
        gem5_stats = self.calculate_latency_statistics(self.gem5_accesses)
        cxlmemsim_stats = self.calculate_latency_statistics(self.cxlmemsim_accesses)
        
        if gem5_stats['mean'] == 0 or cxlmemsim_stats['mean'] == 0:
            logger.warning("One of the traces has zero mean latency, using default parameters")
            return CalibrationParams()
        
        # Calculate the ratio between current CXLMemSim and gem5 latencies
        current_ratio = cxlmemsim_stats['mean'] / gem5_stats['mean']
        logger.info(f"Current CXLMemSim/gem5 latency ratio: {current_ratio:.3f}")
        
        # Calculate adjustment factor to reach target ratio
        adjustment_factor = (target_ratio / current_ratio)
        
        # Analyze instruction-specific adjustments
        gem5_instr_stats = self.analyze_instruction_types(self.gem5_accesses)
        cxlmemsim_instr_stats = self.analyze_instruction_types(self.cxlmemsim_accesses)
        
        instruction_adjustments = {}
        for instr_type in gem5_instr_stats:
            if instr_type in cxlmemsim_instr_stats:
                gem5_mean = gem5_instr_stats[instr_type]['mean']
                cxlmemsim_mean = cxlmemsim_instr_stats[instr_type]['mean']
                if cxlmemsim_mean > 0:
                    instr_ratio = gem5_mean / cxlmemsim_mean
                    instruction_adjustments[instr_type] = instr_ratio * target_ratio
        
        # Calculate minimum latency threshold based on gem5 data
        min_threshold = max(10, int(gem5_stats['min'] * 0.8))
        
        # Calculate stall multiplier based on the congestion difference
        # This is an estimate - in practice, you'd want to analyze actual congestion data
        stall_multiplier = adjustment_factor
        
        params = CalibrationParams(
            base_latency_multiplier=adjustment_factor,
            congestion_factor=1.0,  # Keep congestion factor at 1.0 initially
            instruction_latency_adjustment=instruction_adjustments,
            min_latency_threshold=min_threshold,
            stall_multiplier=stall_multiplier
        )
        
        logger.info(f"Calculated calibration parameters:")
        logger.info(f"  Base latency multiplier: {params.base_latency_multiplier:.3f}")
        logger.info(f"  Min latency threshold: {params.min_latency_threshold}")
        logger.info(f"  Stall multiplier: {params.stall_multiplier:.3f}")
        logger.info(f"  Instruction adjustments: {params.instruction_latency_adjustment}")
        
        return params
    
    def generate_comparison_plot(self, output_path: Path):
        """Generate comparison plots of latency distributions"""
        if not self.gem5_accesses or not self.cxlmemsim_accesses:
            logger.warning("No data available for plotting")
            return
        
        gem5_latencies = [access.latency for access in self.gem5_accesses]
        cxlmemsim_latencies = [access.latency for access in self.cxlmemsim_accesses]
        
        fig, (ax1, ax2, ax3) = plt.subplots(1, 3, figsize=(15, 5))
        
        # Histogram comparison
        ax1.hist(gem5_latencies, bins=50, alpha=0.7, label='gem5', density=True)
        ax1.hist(cxlmemsim_latencies, bins=50, alpha=0.7, label='CXLMemSim', density=True)
        ax1.set_xlabel('Memory Access Latency (cycles)')
        ax1.set_ylabel('Density')
        ax1.set_title('Latency Distribution Comparison')
        ax1.legend()
        ax1.set_xlim(0, np.percentile(gem5_latencies + cxlmemsim_latencies, 95))
        
        # Box plot comparison
        ax2.boxplot([gem5_latencies, cxlmemsim_latencies], labels=['gem5', 'CXLMemSim'])
        ax2.set_ylabel('Memory Access Latency (cycles)')
        ax2.set_title('Latency Box Plot Comparison')
        
        # Scatter plot (if similar number of accesses)
        min_len = min(len(gem5_latencies), len(cxlmemsim_latencies))
        if min_len > 0:
            gem5_sample = np.array(gem5_latencies[:min_len])
            cxlmemsim_sample = np.array(cxlmemsim_latencies[:min_len])
            ax3.scatter(gem5_sample, cxlmemsim_sample, alpha=0.5)
            ax3.plot([0, max(gem5_sample)], [0, max(gem5_sample)], 'r--', label='y=x')
            ax3.set_xlabel('gem5 Latency (cycles)')
            ax3.set_ylabel('CXLMemSim Latency (cycles)')
            ax3.set_title('Latency Correlation')
            ax3.legend()
        
        plt.tight_layout()
        plt.savefig(output_path, dpi=300, bbox_inches='tight')
        logger.info(f"Comparison plot saved to {output_path}")

def generate_rob_patch(params: CalibrationParams, output_path: Path):
    """
    Generate a patch file for rob.cpp with the calibrated parameters
    """
    patch_content = f"""
// Calibrated parameters for RoBSim
// Generated by calibrate_memory_latency.py

// In canRetire function, replace the latency calculation section with:
bool Rob::canRetire(const InstructionGroup &ins) {{
    if (ins.address == 0) {{
        return true;
    }}

    if (cur_latency == 0) {{
        auto allAccess = controller_->get_access(ins.retireTimestamp);
        double baseLatency = controller_->calculate_latency(allAccess, 80.);
        
        // Apply calibrated base latency multiplier
        baseLatency *= {params.base_latency_multiplier:.6f};
        
        // Apply calibrated minimum threshold
        cur_latency = std::max({params.min_latency_threshold}L, static_cast<long>(baseLatency));
        
        // Apply instruction-specific adjustments
        for (const auto &[instr, latency] : instructionLatencyMap) {{
            if (ins.instruction.find(instr) != std::string::npos) {{
                double adjustment = 1.0;
                // Apply calibrated instruction adjustments
"""
    
    for instr_type, adjustment in params.instruction_latency_adjustment.items():
        patch_content += f"""                if (ins.instruction.find("{instr_type}") != std::string::npos) {{
                    adjustment = {adjustment:.6f};
                }}
"""
    
    patch_content += f"""                cur_latency += static_cast<long>(latency * adjustment);
                break;
            }}
        }}

        // Apply calibrated stall multiplier
        stallCount_ += static_cast<long>(cur_latency * {params.stall_multiplier:.6f});
        if (stallCount_ % 2 == 0) {{ stallEventCount_ += 1; }}

        currentCycle_ += cur_latency;
        const_cast<InstructionGroup &>(ins).retireTimestamp += cur_latency;
    }} else {{
        // ... rest of the function remains the same
    }}
    
    // ... rest of canRetire function
}}
"""
    
    with open(output_path, 'w') as f:
        f.write(patch_content)
    
    logger.info(f"ROB patch file generated: {output_path}")

def main():
    parser = argparse.ArgumentParser(description='Calibrate CXLMemSim memory access latency')
    parser.add_argument('--gem5-trace', type=Path, required=True,
                        help='Path to gem5 O3PipeView trace file')
    parser.add_argument('--cxlmemsim-trace', type=Path, required=True,
                        help='Path to CXLMemSim trace file')
    parser.add_argument('--output-config', type=Path, required=True,
                        help='Path to output calibrated parameters JSON file')
    parser.add_argument('--target-ratio', type=float, default=1.0,
                        help='Target ratio of CXLMemSim/gem5 latency (default: 1.0)')
    parser.add_argument('--output-plot', type=Path,
                        help='Path to output comparison plot (optional)')
    parser.add_argument('--output-patch', type=Path,
                        help='Path to output ROB patch file (optional)')
    
    args = parser.parse_args()
    
    # Validate input files
    if not args.gem5_trace.exists():
        logger.error(f"gem5 trace file not found: {args.gem5_trace}")
        return 1
    
    if not args.cxlmemsim_trace.exists():
        logger.error(f"CXLMemSim trace file not found: {args.cxlmemsim_trace}")
        return 1
    
    # Create output directory if needed
    args.output_config.parent.mkdir(parents=True, exist_ok=True)
    
    # Initialize analyzer
    analyzer = LatencyAnalyzer()
    
    # Load and analyze traces
    logger.info("Loading traces...")
    analyzer.load_traces(args.gem5_trace, args.cxlmemsim_trace)
    
    # Calculate calibration parameters
    logger.info("Calculating calibration parameters...")
    params = analyzer.calculate_calibration_params(args.target_ratio)
    
    # Save parameters to JSON
    params_dict = {
        'base_latency_multiplier': params.base_latency_multiplier,
        'congestion_factor': params.congestion_factor,
        'instruction_latency_adjustment': params.instruction_latency_adjustment,
        'min_latency_threshold': params.min_latency_threshold,
        'stall_multiplier': params.stall_multiplier,
        'target_ratio': args.target_ratio
    }
    
    with open(args.output_config, 'w') as f:
        json.dump(params_dict, f, indent=2)
    
    logger.info(f"Calibration parameters saved to: {args.output_config}")
    
    # Generate comparison plot if requested
    if args.output_plot:
        logger.info("Generating comparison plot...")
        analyzer.generate_comparison_plot(args.output_plot)
    
    # Generate ROB patch if requested
    if args.output_patch:
        logger.info("Generating ROB patch file...")
        generate_rob_patch(params, args.output_patch)
    
    # Print summary statistics
    gem5_stats = analyzer.calculate_latency_statistics(analyzer.gem5_accesses)
    cxlmemsim_stats = analyzer.calculate_latency_statistics(analyzer.cxlmemsim_accesses)
    
    print("\\n=== Latency Analysis Summary ===")
    print(f"gem5 latency:      {gem5_stats['mean']:.2f} ± {gem5_stats['std']:.2f} cycles")
    print(f"CXLMemSim latency: {cxlmemsim_stats['mean']:.2f} ± {cxlmemsim_stats['std']:.2f} cycles")
    print(f"Current ratio:     {cxlmemsim_stats['mean']/gem5_stats['mean']:.3f}")
    print(f"Target ratio:      {args.target_ratio:.3f}")
    print(f"Suggested adjustment: {params.base_latency_multiplier:.3f}")
    
    return 0

if __name__ == '__main__':
    exit(main())