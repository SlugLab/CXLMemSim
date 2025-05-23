#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
Automated runner for GAPBS and other workloads
Run all programs and save logs to corresponding locations in artifact folder
"""

import os
import subprocess
import time
import argparse
import logging
import shutil
import itertools
from pathlib import Path
from datetime import datetime
import math
import statistics
# Configure logging format
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(levelname)s - %(message)s',
    datefmt='%Y-%m-%d %H:%M:%S'
)
logger = logging.getLogger(__name__)

# Define policy options
ALLOCATION_POLICIES = ["none", "interleave", "numa"]
MIGRATION_POLICIES = ["none", "heataware", "frequency", "loadbalance", "locality", "lifetime", "hybrid"]
PAGING_POLICIES = ["none", "hugepage", "pagetableaware"]
CACHING_POLICIES = ["none", "fifo", "frequency"]

# Define base paths
ARTIFACT_BASE = "../artifact"
CXL_MEM_SIM = "./CXLMemSim"

# Define workload configurations
WORKLOADS = {
    # "gapbs": {
    #     "path": "../workloads/gapbs",
    #     "programs": [
    #         "bc", "bfs", "cc", "pr", "sssp", "tc"  # All algorithms provided by GAPBS
    #     ],
    #     "args": "-f ../workloads/gapbs/test/graphs/4.el -n100000",  # Default arguments
    #     "env": {}  # Default environment variables
    # },
    # "memcached": {
    #     "path": "./workloads/memcached",
    #     "programs": ["memcached"],
    #     "args": "-u steve",
    #     "env": {}
    # },
    # "llama": {
    #     "path": "../workloads/llama.cpp/build/bin",
    #     "programs": ["llama-cli"],
    #     "args": "--model /storage/deepseek/DeepSeek-R1-Distill-Qwen-32B-IQ2_XXS.gguf --cache-type-k q2_0 --threads 12 --prompt '</think>Generate a poem about a cat in 200 words</think>' -no-cnv",
    #     "env": {}
    # },
    # "gromacs": {
    #     "path": "../workloads/gromacs/build/bin",
    #     "programs": ["gmx"],
    #     "args": "mdrun -s ../workloads/gromacs/build/topol.tpr -nsteps 4 -ntomp 1 -ntmpi 1",
    #     "env": {}
    # },
    # "vsag": {
    #     "path": "/usr/bin/",
    #     "programs": ["python3"],
    #     "args": "run_algorithm.py --dataset random-xs-20-angular --algorithm vsag --module ann_benchmarks.algorithms.vsag --constructor Vsag --runs 2 --count 10 --batch '[\'angular\', 20, {\'M\': 24, \'ef_construction\': 300, \'use_int8\': 4, \'rs\': 0.5}]' '[10]' '[20]' '[30]' '[40]' '[60]' '[80]' '[120]' '[200]' '[400]' '[600]' '[800]'",
    #     "env": {}
    # },
    "microbench": {
        "path": "./microbench",
        "programs": ["ld1","ld2","ld4","ld8","ld16","ld32","ld64","ld128","ld256","st1","st2","st4","st8","st16","st32","st64","st128","st256",],
        "args": "",
        "env": {}
    }
}

def ensure_directory(path):
    """Ensure directory exists, create if not"""
    os.makedirs(path, exist_ok=True)
    return path

def run_command(cmd, log_path=None, env=None, timeout=360000, shell=False):
    """
    Run command and capture output

    Args:
        cmd (list/str): Command list or string
        log_path (str, optional): Log save path
        env (dict, optional): Environment variables
        timeout (int, optional): Timeout in seconds
        shell (bool, optional): Use shell execution

    Returns:
        tuple: (return code, output)
    """
    cmd_str = cmd if isinstance(cmd, str) else ' '.join(cmd)
    logger.info(f"Running command: {cmd_str}")

    # Prepare environment variables
    run_env = os.environ.copy()
    if env:
        run_env.update(env)

    try:
        # Execute command
        process = subprocess.run(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            env=run_env,
            text=True,
            timeout=timeout,
            check=False,
            shell=shell
        )

        # Record output
        output = process.stdout
        if log_path:
            with open(log_path, 'w') as f:
                f.write(output)
            logger.info(f"Output saved to: {log_path}")

        return process.returncode, output

    except subprocess.TimeoutExpired:
        logger.error(f"Command timed out ({timeout}s): {cmd_str}")
        if log_path:
            with open(log_path, 'w') as f:
                f.write(f"TIMEOUT: Command timed out after {timeout} seconds\n")
        return -1, f"TIMEOUT: Command timed out after {timeout} seconds"

    except Exception as e:
        logger.error(f"Command execution failed: {e}")
        if log_path:
            with open(log_path, 'w') as f:
                f.write(f"ERROR: {str(e)}\n")
        return -2, f"ERROR: {str(e)}"

def run_with_setpci_loop(workload, program, args, base_dir):
    """Run workload with just 0x0000 and 0xffff, 10 runs each, compute avg & std-dev."""
    pci_device = "17:00.1"
    pci_offset = "0xe08.l"
    # Only the two values you want:
    pci_values = [0x0000, 0xffff]

    for val in pci_values:
        hex_val = f"0x{val:04x}"
        # First, set the PCI register once
        setpci_cmd = ["setpci", "-s", pci_device, f"{pci_offset}={hex_val}"]
        ret, output = run_command(setpci_cmd, log_path=None, shell=False)
        if ret != 0:
            logger.warning(f"Failed to set PCI value {hex_val} for {pci_device}, skipping")
            continue

        # Prepare to collect timings
        runtimes = []
        for i in range(10):
            # Build the actual command to time (we'll time in Python)
            program_path = os.path.join(WORKLOADS[workload]["path"], program)
            cmd = (
                f"{program_path} {args}"
                if args else
                program_path
            )
            start = time.time()
            ret, _ = run_command(cmd, log_path=None,
                                 env=WORKLOADS[workload].get("env"),
                                 shell=True)
            elapsed = time.time() - start

            if ret != 0:
                logger.warning(f"Run {i+1}/10 failed for PCI {hex_val}")
            runtimes.append(elapsed)
            logger.info(f"PCI {hex_val} run {i+1}: {elapsed:.4f}s")

        # Compute statistics
        avg = statistics.mean(runtimes)
        std = statistics.stdev(runtimes)
        logger.info(f"PCI {hex_val}: avg={avg:.4f}s, std={std:.4f}s")

        # Save detailed and summary logs
        summary_path = os.path.join(base_dir, f"summary_{hex_val}.txt")
        with open(summary_path, "w") as f:
            f.write(f"Runtimes (s): {', '.join(f'{t:.4f}' for t in runtimes)}\n")
            f.write(f"Average (s): {avg:.4f}\n")
            f.write(f"StdDev (s): {std:.4f}\n")

        # (Optional) If you still want per‐run logs, you could reopen run_command
        # with a log_path inside the inner loop instead of log_path=None.
def run_cxl_mem_sim(workload, program, args, base_dir, policy_combo=None, pebs_period=10,
                    latency="100,100,100,100,100,100", bandwidth="100,100,100,100,100,100"):
    """Run program with CXLMemSim"""
    program_path = os.path.join(WORKLOADS[workload]["path"], program)

    # Create specific log filename if policy combination provided
    if policy_combo:
        policy_str = '_'.join(policy_combo)
        log_path = os.path.join(base_dir, f"cxlmemsim_{policy_str}.txt")
    else:
        log_path = os.path.join(base_dir, "cxlmemsim.txt")

    # Build command with arguments
    target_with_args = f"{program_path} {args}" if args else program_path

    # Build base CXLMemSim command
    cmd = [
        CXL_MEM_SIM,
        "-t", target_with_args,
        "-p", str(pebs_period),
        "-l", latency,
        "-b", bandwidth
    ]

    # Add policy parameters if provided
    if policy_combo:
        cmd.extend(["-k", ",".join(policy_combo)])

    # Execute command
    return run_command(cmd, log_path, WORKLOADS[workload].get("env"))

def generate_policy_combinations(args):
    """Generate policy combinations"""
    # Use specified policies or defaults
    allocation_policies = args.allocation_policies if args.allocation_policies else ["none"]
    migration_policies = args.migration_policies if args.migration_policies else ["none"]
    paging_policies = args.paging_policies if args.paging_policies else ["none"]
    caching_policies = args.caching_policies if args.caching_policies else ["none"]

    # Generate all combinations
    return list(itertools.product(allocation_policies, migration_policies, paging_policies, caching_policies))

def run_all_workloads(args):
    """Run all workloads"""
    start_time = time.time()
    logger.info("Starting all workloads")

    # Create main artifact directory
    ensure_directory(ARTIFACT_BASE)

    # Collect system information
    if args.collect_system_info:
        logger.info("Collecting system information")
        run_command(["dmesg"], os.path.join(ARTIFACT_BASE, "dmesg.txt"))
        run_command(["dmidecode"], os.path.join(ARTIFACT_BASE, "dmidecode.txt"))
        run_command(["lspci", "-vvv"], os.path.join(ARTIFACT_BASE, "lspci.txt"))

    # Generate policy combinations if needed
    policy_combinations = generate_policy_combinations(args) if args.run_policy_combinations else [None]
    logger.info(f"Running tests with {len(policy_combinations)} policy combinations")

    # Process all workloads
    for workload_name, workload_config in WORKLOADS.items():
        if args.workloads and workload_name not in args.workloads:
            logger.info(f"Skipping workload: {workload_name}")
            continue

        logger.info(f"Processing workload: {workload_name}")

        # Process all programs in workload
        for program in workload_config["programs"]:
            if args.programs and program not in args.programs:
                logger.info(f"Skipping program: {program}")
                continue

            logger.info(f"Processing program: {program}")

            # Create program directory
            program_dir = ensure_directory(os.path.join(ARTIFACT_BASE, workload_name, program))

            # Run original program if required
            if args.run_original:
                logger.info(f"Running original program: {program}")
                run_with_setpci_loop(
                    workload_name,
                    program,
                    workload_config["args"],
                    program_dir
                )

    # Complete all workloads
    end_time = time.time()
    elapsed_time = end_time - start_time
    hours, remainder = divmod(elapsed_time, 3600)
    minutes, seconds = divmod(remainder, 60)

    logger.info(f"All workloads completed. Total time: {int(hours)}h {int(minutes)}m {int(seconds)}s")

    # Save run log to artifact directory
    if args.log_file:
        shutil.copy2(args.log_file, os.path.join(ARTIFACT_BASE, "run.log"))

def main():
    parser = argparse.ArgumentParser(description="Automated runner for GAPBS and other workloads")
    parser.add_argument("--workloads", nargs="+", help="Workloads to run, default all")
    parser.add_argument("--programs", nargs="+", help="Programs to run, default all")
    parser.add_argument("--run-original", action="store_true", help="Run original program")
    parser.add_argument("--run-cxlmemsim", action="store_true", help="Run with CXLMemSim")
    parser.add_argument("--collect-system-info", action="store_true", help="Collect system information")

    # CXLMemSim parameters
    parser.add_argument("--pebs-period", type=int, default=10, help="PEBS sampling period")
    parser.add_argument("--latency", default="200,250,200,250,200,250", help="CXLMemSim latency settings")
    parser.add_argument("--bandwidth", default="50,50,50,50,50,50", help="CXLMemSim bandwidth settings")

    # Policy combination parameters
    parser.add_argument("--run-policy-combinations", action="store_true", help="Run policy combination tests")
    parser.add_argument("--allocation-policies", nargs="+", choices=ALLOCATION_POLICIES,
                        help=f"Allocation policy options: {', '.join(ALLOCATION_POLICIES)}",default=ALLOCATION_POLICIES)
    parser.add_argument("--migration-policies", nargs="+", choices=MIGRATION_POLICIES,
                        help=f"Migration policy options: {', '.join(MIGRATION_POLICIES)}",default=MIGRATION_POLICIES)
    parser.add_argument("--paging-policies", nargs="+", choices=PAGING_POLICIES,
                        help=f"Paging policy options: {', '.join(PAGING_POLICIES)}",default=PAGING_POLICIES)
    parser.add_argument("--caching-policies", nargs="+", choices=CACHING_POLICIES,
                        help=f"Caching policy options: {', '.join(CACHING_POLICIES)}",default=CACHING_POLICIES)

    # Error handling
    parser.add_argument("--ignore-errors", action="store_true", help="Ignore errors and continue")
    parser.add_argument("--stop-on-error", action="store_true", help="Stop on first error")
    parser.add_argument("--log-file", default="run.log", help="Run log file")
    parser.add_argument("--timeout", type=int, default=3600, help="Command timeout in seconds")

    args = parser.parse_args()

    # Configure log file
    file_handler = logging.FileHandler(args.log_file)
    file_handler.setFormatter(logging.Formatter('%(asctime)s - %(levelname)s - %(message)s'))
    logger.addHandler(file_handler)

    # Default to run both if no type specified
    if not args.run_original and not args.run_cxlmemsim:
        args.run_original = True
        args.run_cxlmemsim = True

    # Execute all workloads
    run_all_workloads(args)

if __name__ == "__main__":
    main()