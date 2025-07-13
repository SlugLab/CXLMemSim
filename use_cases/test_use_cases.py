#!/usr/bin/env python3
"""
Integration tests for CXLMemSim use cases
Validates all three use case implementations
"""

import unittest
import tempfile
import json
import yaml
import subprocess
import os
import sys
from pathlib import Path
import shutil

# Add use case modules to path
USE_CASES_DIR = Path(__file__).parent
sys.path.insert(0, str(USE_CASES_DIR / "production_profiling"))
sys.path.insert(0, str(USE_CASES_DIR / "procurement_decision"))
sys.path.insert(0, str(USE_CASES_DIR / "memory_tiering"))

# Import use case modules
from production_profiler import ProductionProfiler
from procurement_analyzer import ProcurementAnalyzer
from tiering_policy_engine import MemoryTieringEngine

class TestCXLMemSimUseCases(unittest.TestCase):
    """Integration tests for CXLMemSim use cases"""
    
    @classmethod
    def setUpClass(cls):
        """Set up test environment"""
        cls.test_dir = Path(tempfile.mkdtemp(prefix="cxlmemsim_test_"))
        cls.mock_binary = cls.test_dir / "mock_cxlmemsim"
        
        # Create mock CXLMemSim binary that produces realistic output
        cls._create_mock_cxlmemsim()
        
        # Create test workload binary
        cls.test_workload = cls.test_dir / "test_workload"
        cls.test_workload.write_text("#!/bin/bash\nsleep 1\n")
        cls.test_workload.chmod(0o755)
        
    @classmethod
    def tearDownClass(cls):
        """Clean up test environment"""
        if cls.test_dir.exists():
            shutil.rmtree(cls.test_dir)
            
    @classmethod
    def _create_mock_cxlmemsim(cls):
        """Create mock CXLMemSim binary for testing"""
        mock_script = f"""#!/bin/bash
# Mock CXLMemSim for testing

echo "CXLMemSim Mock - Test Mode"
echo "Execution time: 2.5s"
echo "Local memory accesses: 1000000"
echo "Remote memory accesses: 500000"
echo "Average latency: 120.5ns"
echo "Bandwidth utilization: 45.2%"
echo "Throughput: 800.0 MB/s"

# Simulate some processing time
sleep 0.1

exit 0
"""
        cls.mock_binary.write_text(mock_script)
        cls.mock_binary.chmod(0o755)
        
    def setUp(self):
        """Set up individual test"""
        self.output_dir = self.test_dir / f"test_output_{self._testMethodName}"
        self.output_dir.mkdir(exist_ok=True)
        
    def test_production_profiler_initialization(self):
        """Test ProductionProfiler can be initialized"""
        profiler = ProductionProfiler(str(self.mock_binary), str(self.output_dir))
        self.assertIsInstance(profiler, ProductionProfiler)
        self.assertEqual(profiler.cxlmemsim_path, Path(self.mock_binary))
        
    def test_production_profiler_single_workload(self):
        """Test single workload profiling"""
        profiler = ProductionProfiler(str(self.mock_binary), str(self.output_dir))
        
        workload_config = {
            "name": "test_workload",
            "binary": str(self.test_workload),
            "interval": 1,
            "timeout": 10,
            "cpuset": "0",
            "dram_latency": 85
        }
        
        result = profiler.profile_workload(workload_config)
        
        self.assertIn("workload", result)
        self.assertIn("metrics", result)
        self.assertIn("execution_time", result)
        self.assertEqual(result["workload"], "test_workload")
        self.assertEqual(result["returncode"], 0)
        
    def test_production_profiler_suite_config(self):
        """Test production profiling suite configuration"""
        config_file = self.test_dir / "test_suite.yaml"
        
        suite_config = {
            "parallel_jobs": 1,
            "workloads": [{
                "name": "test_app",
                "binary": str(self.test_workload),
                "interval": 1,
                "timeout": 10
            }],
            "cxl_configurations": [{
                "name": "baseline",
                "dram_latency": 85,
                "capacity": [100]
            }, {
                "name": "cxl_test",
                "dram_latency": 85,
                "latency": [150, 180],
                "bandwidth": [30000, 25000],
                "capacity": [50, 50]
            }]
        }
        
        with open(config_file, 'w') as f:
            yaml.dump(suite_config, f)
            
        profiler = ProductionProfiler(str(self.mock_binary), str(self.output_dir))
        profiler.run_production_suite(str(config_file))
        
        # Check that results were generated
        self.assertTrue(len(profiler.results) > 0)
        self.assertEqual(len(profiler.results), 2)  # 1 workload * 2 configs
        
    def test_procurement_analyzer_initialization(self):
        """Test ProcurementAnalyzer can be initialized"""
        analyzer = ProcurementAnalyzer(str(self.mock_binary))
        self.assertIsInstance(analyzer, ProcurementAnalyzer)
        
    def test_procurement_analyzer_hardware_evaluation(self):
        """Test hardware configuration evaluation"""
        analyzer = ProcurementAnalyzer(str(self.mock_binary))
        
        hw_config = {
            "name": "test_config",
            "local_memory_gb": 128,
            "cxl_memory_gb": 128,
            "cxl_latency_ns": 150,
            "cxl_bandwidth_gbps": 32,
            "base_system_cost": 5000,
            "dram_cost_per_gb": 8,
            "cxl_cost_per_gb": 4,
            "topology": "(1,(2))",
            "memory_distribution": [50, 50]
        }
        
        workload = {
            "binary": str(self.test_workload),
            "interval": 1
        }
        
        result = analyzer.evaluate_hardware_config(hw_config, workload)
        
        self.assertIn("config", result)
        self.assertIn("metrics", result)
        self.assertIn("performance_score", result)
        self.assertIn("total_cost", result)
        self.assertGreater(result["total_cost"], 0)
        self.assertGreater(result["performance_score"], 0)
        
    def test_procurement_analyzer_full_analysis(self):
        """Test complete procurement analysis"""
        config_file = self.test_dir / "test_procurement.yaml"
        
        procurement_config = {
            "workloads": [{
                "name": "test_workload",
                "binary": str(self.test_workload),
                "interval": 1
            }],
            "hardware_configurations": [{
                "name": "baseline",
                "local_memory_gb": 128,
                "cxl_memory_gb": 0,
                "base_system_cost": 5000,
                "dram_cost_per_gb": 8,
                "topology": "(1)",
                "memory_distribution": [100]
            }, {
                "name": "cxl_config",
                "local_memory_gb": 64,
                "cxl_memory_gb": 64,
                "cxl_latency_ns": 150,
                "cxl_bandwidth_gbps": 32,
                "base_system_cost": 5000,
                "dram_cost_per_gb": 8,
                "cxl_cost_per_gb": 4,
                "cxl_device_cost": 500,
                "topology": "(1,(2))",
                "memory_distribution": [50, 50]
            }],
            "requirements": {
                "max_budget": 10000,
                "min_performance": 1.0
            },
            "tco_parameters": {
                "years": 3,
                "electricity_cost_per_kwh": 0.12
            }
        }
        
        with open(config_file, 'w') as f:
            yaml.dump(procurement_config, f)
            
        analyzer = ProcurementAnalyzer(str(self.mock_binary))
        recommendation = analyzer.run_procurement_analysis(str(config_file), str(self.output_dir))
        
        self.assertIn("recommended_configuration", recommendation)
        self.assertIn("reasoning", recommendation)
        
        # Check that analysis files were created
        self.assertTrue((self.output_dir / "procurement_analysis.json").exists())
        
    def test_memory_tiering_engine_initialization(self):
        """Test MemoryTieringEngine can be initialized"""
        engine = MemoryTieringEngine(str(self.mock_binary))
        self.assertIsInstance(engine, MemoryTieringEngine)
        
    def test_memory_tiering_policy_registration(self):
        """Test policy registration and execution"""
        engine = MemoryTieringEngine(str(self.mock_binary))
        
        # Test static policy
        static_policy = engine.create_static_policy([0.6, 0.4])
        engine.register_policy("test_static", static_policy)
        
        self.assertIn("test_static", engine.policies)
        
        # Test policy execution
        result = engine.policies["test_static"]({}, {})
        self.assertEqual(result, [0.6, 0.4])
        
    def test_memory_tiering_hotness_policy(self):
        """Test hotness-based tiering policy"""
        engine = MemoryTieringEngine(str(self.mock_binary))
        
        hotness_policy = engine.create_hotness_based_policy(0.7)
        
        # Test with high hotness
        high_hotness_pattern = {"hot_page_ratio": 0.9}
        result_hot = hotness_policy({}, high_hotness_pattern)
        self.assertEqual(len(result_hot), 2)
        self.assertGreater(result_hot[0], result_hot[1])  # More local for hot data
        
        # Test with low hotness
        low_hotness_pattern = {"hot_page_ratio": 0.3}
        result_cold = hotness_policy({}, low_hotness_pattern)
        self.assertEqual(len(result_cold), 2)
        
    def test_memory_tiering_ml_policy(self):
        """Test ML-based tiering policy"""
        engine = MemoryTieringEngine(str(self.mock_binary))
        
        # Create training data
        training_data = [
            {
                "memory_intensity": 0.8,
                "access_locality": 0.9,
                "read_write_ratio": 0.8,
                "working_set_size": 80,
                "cache_miss_rate": 0.15,
                "optimal_allocation": [0.7, 0.3]
            },
            {
                "memory_intensity": 0.4,
                "access_locality": 0.6,
                "read_write_ratio": 0.6,
                "working_set_size": 150,
                "cache_miss_rate": 0.25,
                "optimal_allocation": [0.3, 0.7]
            }
        ]
        
        ml_policy = engine.create_ml_policy(training_data)
        self.assertIsNotNone(engine.ml_model)
        
        # Test policy prediction
        test_workload = {"memory_intensity": 0.6, "working_set_size": 100}
        test_pattern = {"access_locality": 0.7, "read_write_ratio": 0.7, "cache_miss_rate": 0.2}
        
        result = ml_policy(test_workload, test_pattern)
        self.assertEqual(len(result), 2)
        self.assertAlmostEqual(sum(result), 1.0, places=2)
        
    def test_memory_tiering_policy_comparison(self):
        """Test complete policy comparison"""
        config_file = self.test_dir / "test_tiering.yaml"
        
        tiering_config = {
            "evaluation_duration": 10,  # Short duration for testing
            "workloads": [{
                "name": "test_memory_workload",
                "type": "general",
                "binary": str(self.test_workload),
                "interval": 1,
                "dram_latency": 85,
                "latency": [150, 180],
                "bandwidth": [30000, 25000],
                "tier_capacities": [64, 64],
                "topology": "(1,(2))",
                "read_write_ratio": 0.7,
                "working_set_size": 32
            }],
            "policies_to_evaluate": [
                "static_balanced",
                "static_local_heavy",
                "hotness_based"
            ],
            "ml_training_data": [{
                "memory_intensity": 0.5,
                "access_locality": 0.7,
                "read_write_ratio": 0.7,
                "working_set_size": 32,
                "cache_miss_rate": 0.2,
                "optimal_allocation": [0.5, 0.5]
            }]
        }
        
        with open(config_file, 'w') as f:
            yaml.dump(tiering_config, f)
            
        engine = MemoryTieringEngine(str(self.mock_binary))
        results = engine.run_policy_comparison(str(config_file), str(self.output_dir))
        
        self.assertIsInstance(results, list)
        self.assertGreater(len(results), 0)
        
        # Check that comparison files were created
        self.assertTrue((self.output_dir / "policy_comparison.json").exists())
        
    def test_integration_all_use_cases(self):
        """Integration test running all use cases together"""
        # This test ensures all use cases can run without conflicts
        
        # Create minimal configs for all use cases
        production_config = {
            "parallel_jobs": 1,
            "workloads": [{"name": "test", "binary": str(self.test_workload), "timeout": 5}],
            "cxl_configurations": [{"name": "baseline", "dram_latency": 85}]
        }
        
        procurement_config = {
            "workloads": [{"name": "test", "binary": str(self.test_workload)}],
            "hardware_configurations": [{
                "name": "test_hw",
                "local_memory_gb": 64,
                "base_system_cost": 3000,
                "dram_cost_per_gb": 8,
                "topology": "(1)"
            }],
            "requirements": {"max_budget": 5000}
        }
        
        tiering_config = {
            "evaluation_duration": 5,
            "workloads": [{
                "name": "test", "binary": str(self.test_workload),
                "tier_capacities": [32, 32], "topology": "(1,(2))"
            }],
            "policies_to_evaluate": ["static_balanced"]
        }
        
        # Write config files
        prod_file = self.output_dir / "prod_config.yaml"
        proc_file = self.output_dir / "proc_config.yaml"
        tier_file = self.output_dir / "tier_config.yaml"
        
        with open(prod_file, 'w') as f:
            yaml.dump(production_config, f)
        with open(proc_file, 'w') as f:
            yaml.dump(procurement_config, f)
        with open(tier_file, 'w') as f:
            yaml.dump(tiering_config, f)
            
        # Run all use cases
        profiler = ProductionProfiler(str(self.mock_binary), str(self.output_dir / "prod"))
        profiler.run_production_suite(str(prod_file))
        
        analyzer = ProcurementAnalyzer(str(self.mock_binary))
        analyzer.run_procurement_analysis(str(proc_file), str(self.output_dir / "proc"))
        
        engine = MemoryTieringEngine(str(self.mock_binary))
        engine.run_policy_comparison(str(tier_file), str(self.output_dir / "tier"))
        
        # Verify all completed without errors
        self.assertTrue(len(profiler.results) > 0)
        self.assertTrue((self.output_dir / "proc" / "procurement_analysis.json").exists())
        self.assertTrue((self.output_dir / "tier" / "policy_comparison.json").exists())
        
    def test_error_handling(self):
        """Test error handling in use cases"""
        
        # Test with non-existent binary
        fake_binary = self.test_dir / "nonexistent_binary"
        
        profiler = ProductionProfiler(str(fake_binary), str(self.output_dir))
        workload_config = {
            "name": "test_workload",
            "binary": str(fake_binary),
            "timeout": 5
        }
        
        result = profiler.profile_workload(workload_config)
        self.assertNotEqual(result["returncode"], 0)
        
        # Test with invalid configuration
        analyzer = ProcurementAnalyzer(str(self.mock_binary))
        invalid_hw_config = {}  # Empty config
        invalid_workload = {"binary": str(self.test_workload)}
        
        result = analyzer.evaluate_hardware_config(invalid_hw_config, invalid_workload)
        # Should handle gracefully without crashing
        self.assertIsInstance(result, dict)


class TestScriptExecution(unittest.TestCase):
    """Test script execution and CLI interfaces"""
    
    @classmethod
    def setUpClass(cls):
        cls.use_cases_dir = Path(__file__).parent
        cls.test_dir = Path(tempfile.mkdtemp(prefix="cxlmemsim_script_test_"))
        
    @classmethod
    def tearDownClass(cls):
        if cls.test_dir.exists():
            shutil.rmtree(cls.test_dir)
            
    def test_script_help_messages(self):
        """Test that all scripts have proper help messages"""
        scripts = [
            self.use_cases_dir / "production_profiling" / "production_profiler.py",
            self.use_cases_dir / "procurement_decision" / "procurement_analyzer.py",
            self.use_cases_dir / "memory_tiering" / "tiering_policy_engine.py"
        ]
        
        for script in scripts:
            if script.exists():
                result = subprocess.run(
                    [sys.executable, str(script), "--help"],
                    capture_output=True, text=True
                )
                self.assertEqual(result.returncode, 0, f"Help failed for {script}")
                self.assertIn("usage:", result.stdout.lower())
                
    def test_run_all_examples_script(self):
        """Test the comprehensive example runner script"""
        run_script = self.use_cases_dir / "run_all_examples.sh"
        
        if run_script.exists():
            # Test help message
            result = subprocess.run(
                [str(run_script), "help"],
                capture_output=True, text=True
            )
            self.assertEqual(result.returncode, 0)
            self.assertIn("Usage:", result.stdout)


def run_tests():
    """Run all tests with proper output"""
    
    # Create test suite
    loader = unittest.TestLoader()
    suite = unittest.TestSuite()
    
    # Add test classes
    suite.addTests(loader.loadTestsFromTestCase(TestCXLMemSimUseCases))
    suite.addTests(loader.loadTestsFromTestCase(TestScriptExecution))
    
    # Run tests
    runner = unittest.TextTestRunner(verbosity=2, stream=sys.stdout)
    result = runner.run(suite)
    
    return result.wasSuccessful()


if __name__ == "__main__":
    success = run_tests()
    sys.exit(0 if success else 1)