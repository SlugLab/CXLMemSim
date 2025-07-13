#!/usr/bin/env python3
"""
Apply calibrated parameters to CXLMemSim RoBSim

This script modifies the rob.cpp file to apply calibrated memory latency parameters.

Usage:
    python apply_calibration.py --config calibrated_params.json --rob-file ../src/rob.cpp
"""

import argparse
import json
import re
from pathlib import Path
import logging

logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(levelname)s - %(message)s')
logger = logging.getLogger(__name__)

def apply_calibration_to_rob(rob_file: Path, config: dict) -> bool:
    """
    Apply calibration parameters to rob.cpp file
    
    Args:
        rob_file: Path to rob.cpp file
        config: Dictionary with calibration parameters
        
    Returns:
        True if successful, False otherwise
    """
    if not rob_file.exists():
        logger.error(f"ROB file not found: {rob_file}")
        return False
    
    # Read the current rob.cpp content
    with open(rob_file, 'r') as f:
        content = f.read()
    
    # Create backup
    backup_file = rob_file.with_suffix('.cpp.backup')
    with open(backup_file, 'w') as f:
        f.write(content)
    logger.info(f"Backup created: {backup_file}")
    
    # Apply modifications
    modified_content = content
    
    # 1. Update base latency multiplier in canRetire function
    base_multiplier = config.get('base_latency_multiplier', 1.0)
    
    # Find and replace the base latency calculation
    base_latency_pattern = r'(cur_latency = std::max\(\s*)(\d+)(L,\s*static_cast<long>\(baseLatency\)\);)'
    min_threshold = config.get('min_latency_threshold', 10)
    
    if re.search(base_latency_pattern, modified_content):
        modified_content = re.sub(
            base_latency_pattern,
            f'\\1{min_threshold}\\3',
            modified_content
        )
        logger.info(f"Updated minimum latency threshold to {min_threshold}")
    
    # 2. Add base latency multiplier
    baselatency_calc_pattern = r'(double baseLatency = controller_->calculate_latency\(allAccess, 80\.\);)'
    replacement = f'\\1\\n        baseLatency *= {base_multiplier:.6f}; // Calibrated multiplier'
    
    if re.search(baselatency_calc_pattern, modified_content):
        modified_content = re.sub(baselatency_calc_pattern, replacement, modified_content)
        logger.info(f"Added base latency multiplier: {base_multiplier:.6f}")
    
    # 3. Update stall multiplier
    stall_multiplier = config.get('stall_multiplier', 1.0)
    stall_pattern = r'(stallCount_ \+= )(cur_latency);'
    stall_replacement = f'\\1static_cast<long>(\\2 * {stall_multiplier:.6f}); // Calibrated stall multiplier'
    
    if re.search(stall_pattern, modified_content):
        modified_content = re.sub(stall_pattern, stall_replacement, modified_content)
        logger.info(f"Updated stall multiplier: {stall_multiplier:.6f}")
    
    # 4. Add instruction-specific adjustments (if any)
    instr_adjustments = config.get('instruction_latency_adjustment', {})
    if instr_adjustments:
        # Find the instruction latency loop
        instr_loop_pattern = r'(for \(const auto &\[instr, latency\] : instructionLatencyMap\) \{[\s\S]*?)(cur_latency \+= latency;)([\s\S]*?\})'
        
        if re.search(instr_loop_pattern, modified_content):
            # Create adjustment code
            adjustment_code = "double adjustment = 1.0;\n"
            for instr_type, adj_factor in instr_adjustments.items():
                adjustment_code += f'                if (ins.instruction.find("{instr_type}") != std::string::npos) {{ adjustment = {adj_factor:.6f}; }}\n'
            adjustment_code += "                cur_latency += static_cast<long>(latency * adjustment);"
            
            modified_content = re.sub(
                instr_loop_pattern,
                f'\\1{adjustment_code}\\3',
                modified_content
            )
            logger.info(f"Added instruction-specific adjustments for {len(instr_adjustments)} instruction types")
    
    # Write the modified content back
    with open(rob_file, 'w') as f:
        f.write(modified_content)
    
    logger.info(f"Successfully applied calibration to {rob_file}")
    return True

def main():
    parser = argparse.ArgumentParser(description='Apply calibrated parameters to CXLMemSim RoBSim')
    parser.add_argument('--config', type=Path, required=True,
                        help='Path to calibrated parameters JSON file')
    parser.add_argument('--rob-file', type=Path, required=True,
                        help='Path to rob.cpp file to modify')
    parser.add_argument('--dry-run', action='store_true',
                        help='Show what would be changed without modifying files')
    
    args = parser.parse_args()
    
    # Validate input files
    if not args.config.exists():
        logger.error(f"Config file not found: {args.config}")
        return 1
    
    if not args.rob_file.exists():
        logger.error(f"ROB file not found: {args.rob_file}")
        return 1
    
    # Load configuration
    with open(args.config, 'r') as f:
        config = json.load(f)
    
    logger.info("Loaded calibration parameters:")
    for key, value in config.items():
        logger.info(f"  {key}: {value}")
    
    if args.dry_run:
        logger.info("DRY RUN: Would apply the above parameters to rob.cpp")
        return 0
    
    # Apply calibration
    success = apply_calibration_to_rob(args.rob_file, config)
    
    if success:
        logger.info("Calibration applied successfully!")
        logger.info("Please rebuild CXLMemSim to use the new parameters.")
        return 0
    else:
        logger.error("Failed to apply calibration")
        return 1

if __name__ == '__main__':
    exit(main())