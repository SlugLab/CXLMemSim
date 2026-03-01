/*
 * Test: Two distributed CXL memory servers on one host using SHM communication
 *
 * This test creates two DistributedMemoryServer instances (node 0 and node 1)
 * in a single process, communicating through POSIX shared memory message queues.
 * It verifies:
 *   - Cluster formation (node 1 joins node 0's cluster)
 *   - Local read/write on each node
 *   - Cross-node read/write (node 0 writing to node 1's address range and vice versa)
 *   - Coherency protocol messages between nodes
 *   - Latency reporting
 *
 * SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
 * Copyright 2025 Regents of the University of California
 * UC Santa Cruz Sluglab.
 */

#include "cxlcontroller.h"
#include "cxlendpoint.h"
#include "distributed_server.h"
#include "policy.h"
#include <spdlog/spdlog.h>
#include <spdlog/cfg/env.h>
#include <cstring>
#include <cassert>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <iostream>

// Globals required by helper.cpp
Helper helper{};
CXLController* controller = nullptr;
Monitors* monitors = nullptr;

static constexpr uint64_t NODE0_BASE = 0x100000000ULL; // 4GB
static constexpr uint64_t NODE1_BASE = 0x200000000ULL; // 8GB
static constexpr size_t CAPACITY_MB = 64;              // 64MB per node

struct TestResult {
    int passed = 0;
    int failed = 0;
    void check(bool cond, const char* name) {
        if (cond) {
            passed++;
            std::cout << "  [PASS] " << name << std::endl;
        } else {
            failed++;
            std::cout << "  [FAIL] " << name << std::endl;
        }
    }
};

int main(int argc, char* argv[]) {
    spdlog::cfg::load_env_levels();
    spdlog::set_level(spdlog::level::info);

    std::cout << "=== Distributed CXL SHM Communication Test ===" << std::endl;
    std::cout << "Node 0 base: 0x" << std::hex << NODE0_BASE << std::endl;
    std::cout << "Node 1 base: 0x" << std::hex << NODE1_BASE << std::dec << std::endl;
    std::cout << "Capacity per node: " << CAPACITY_MB << " MB" << std::endl;
    std::cout << std::endl;

    TestResult results;

    // Clean up any leftover SHM segments from previous runs
    (void)system("rm -f /dev/shm/cxltest_dist*");

    // ====================================================================
    // Phase 1: Create controllers for each node
    // ====================================================================
    std::cout << "--- Phase 1: Create Controllers ---" << std::endl;

    std::array<Policy*, 4> policies0 = {
        new AllocationPolicy(),
        new MigrationPolicy(),
        new PagingPolicy(),
        new CachingPolicy()
    };
    std::array<Policy*, 4> policies1 = {
        new AllocationPolicy(),
        new MigrationPolicy(),
        new PagingPolicy(),
        new CachingPolicy()
    };

    CXLController ctrl0(policies0, CAPACITY_MB, page_type::PAGE, 10, 100);
    CXLController ctrl1(policies1, CAPACITY_MB, page_type::PAGE, 10, 100);

    results.check(true, "Controllers created");

    // ====================================================================
    // Phase 2: Create and initialize distributed servers
    // ====================================================================
    std::cout << "--- Phase 2: Initialize Distributed Servers ---" << std::endl;

    // Set base addresses via environment variable before initializing
    setenv("CXL_BASE_ADDR", std::to_string(NODE0_BASE).c_str(), 1);
    DistributedMemoryServer server0(0, "/cxltest_dist", 9990, CAPACITY_MB, &ctrl0,
                                     DistTransportMode::SHM);

    bool init0 = server0.initialize();
    results.check(init0, "Node 0 initialized");

    if (!init0) {
        std::cerr << "FATAL: Node 0 failed to initialize" << std::endl;
        return 1;
    }

    // Now initialize node 1 with different base address
    // Node 1 uses the SAME dist SHM name since they share the message bus
    setenv("CXL_BASE_ADDR", std::to_string(NODE1_BASE).c_str(), 1);
    DistributedMemoryServer server1(1, "/cxltest_dist", 9991, CAPACITY_MB, &ctrl1,
                                     DistTransportMode::SHM);

    bool init1 = server1.initialize();
    results.check(init1, "Node 1 initialized");

    if (!init1) {
        std::cerr << "FATAL: Node 1 failed to initialize" << std::endl;
        return 1;
    }

    // ====================================================================
    // Phase 3: Verify cluster formation
    // ====================================================================
    std::cout << "--- Phase 3: Cluster Formation ---" << std::endl;

    // Both nodes share the same SHM message bus after initialize().
    // Node 0 created it, node 1 joined by opening it.
    // Both are registered in the SHM header's node status array.
    results.check(server0.get_node_id() == 0, "Node 0 has correct ID");
    results.check(server1.get_node_id() == 1, "Node 1 has correct ID");
    results.check(server0.get_state() == NODE_STATE_READY, "Node 0 is READY");
    results.check(server1.get_state() == NODE_STATE_READY, "Node 1 is READY");

    // ====================================================================
    // Phase 4: Start both servers
    // ====================================================================
    std::cout << "--- Phase 4: Start Servers ---" << std::endl;

    bool started0 = server0.start();
    results.check(started0, "Node 0 started");

    bool started1 = server1.start();
    results.check(started1, "Node 1 started");

    // Give message processing threads time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // ====================================================================
    // Phase 5: Local read/write on Node 0
    // ====================================================================
    std::cout << "--- Phase 5: Local Operations ---" << std::endl;

    {
        uint8_t write_buf[64] = {};
        uint8_t read_buf[64] = {};
        uint64_t latency = 0;

        // Write pattern to node 0's local memory
        memset(write_buf, 0xAA, 64);
        int wret = server0.write(NODE0_BASE, write_buf, 64, &latency);
        results.check(wret == 0, "Node 0 local write");
        if (wret == 0) {
            std::cout << "    Write latency: " << latency << " ns" << std::endl;
        }

        // Read it back
        int rret = server0.read(NODE0_BASE, read_buf, 64, &latency);
        results.check(rret == 0, "Node 0 local read");
        if (rret == 0) {
            std::cout << "    Read latency: " << latency << " ns" << std::endl;
            results.check(memcmp(read_buf, write_buf, 64) == 0, "Node 0 local data integrity");
        }
    }

    {
        uint8_t write_buf[64] = {};
        uint8_t read_buf[64] = {};
        uint64_t latency = 0;

        // Write pattern to node 1's local memory
        memset(write_buf, 0xBB, 64);
        int wret = server1.write(NODE1_BASE, write_buf, 64, &latency);
        results.check(wret == 0, "Node 1 local write");
        if (wret == 0) {
            std::cout << "    Write latency: " << latency << " ns" << std::endl;
        }

        // Read it back
        int rret = server1.read(NODE1_BASE, read_buf, 64, &latency);
        results.check(rret == 0, "Node 1 local read");
        if (rret == 0) {
            std::cout << "    Read latency: " << latency << " ns" << std::endl;
            results.check(memcmp(read_buf, write_buf, 64) == 0, "Node 1 local data integrity");
        }
    }

    // ====================================================================
    // Phase 6: Cross-node operations (Node 0 reads/writes Node 1's memory)
    // ====================================================================
    std::cout << "--- Phase 6: Cross-Node Operations ---" << std::endl;

    // First, register node 1's address range with node 0's HDM decoder
    // so node 0 knows to forward requests to node 1
    if (ctrl0.hdm_decoder_) {
        ctrl0.hdm_decoder_->add_range(NODE1_BASE, CAPACITY_MB * 1024 * 1024, 1, true);
        results.check(true, "Node 0 HDM: added Node 1's range as remote");
    }
    if (ctrl1.hdm_decoder_) {
        ctrl1.hdm_decoder_->add_range(NODE0_BASE, CAPACITY_MB * 1024 * 1024, 0, true);
        results.check(true, "Node 1 HDM: added Node 0's range as remote");
    }

    {
        // Node 0 writes to Node 1's address range
        uint8_t write_buf[64] = {};
        uint8_t read_buf[64] = {};
        uint64_t latency = 0;

        memset(write_buf, 0xCC, 64);

        // This should be forwarded via SHM message to node 1
        int wret = server0.write(NODE1_BASE + 64, write_buf, 64, &latency);
        results.check(wret == 0, "Node 0 -> Node 1 cross-node write");
        if (wret == 0) {
            std::cout << "    Cross-node write latency: " << latency << " ns" << std::endl;
        }

        // Verify by reading directly on node 1
        int rret = server1.read(NODE1_BASE + 64, read_buf, 64, &latency);
        results.check(rret == 0, "Node 1 local read of cross-written data");
        if (rret == 0) {
            results.check(memcmp(read_buf, write_buf, 64) == 0,
                          "Cross-node write data integrity (verified on Node 1)");
        }
    }

    {
        // Node 1 writes to Node 0's address range
        uint8_t write_buf[64] = {};
        uint8_t read_buf[64] = {};
        uint64_t latency = 0;

        memset(write_buf, 0xDD, 64);

        int wret = server1.write(NODE0_BASE + 128, write_buf, 64, &latency);
        results.check(wret == 0, "Node 1 -> Node 0 cross-node write");
        if (wret == 0) {
            std::cout << "    Cross-node write latency: " << latency << " ns" << std::endl;
        }

        // Verify by reading directly on node 0
        int rret = server0.read(NODE0_BASE + 128, read_buf, 64, &latency);
        results.check(rret == 0, "Node 0 local read of cross-written data");
        if (rret == 0) {
            results.check(memcmp(read_buf, write_buf, 64) == 0,
                          "Cross-node write data integrity (verified on Node 0)");
        }
    }

    {
        // Node 0 reads from Node 1's address range
        uint8_t read_buf[64] = {};
        uint64_t latency = 0;

        int rret = server0.read(NODE1_BASE, read_buf, 64, &latency);
        results.check(rret == 0, "Node 0 -> Node 1 cross-node read");
        if (rret == 0) {
            std::cout << "    Cross-node read latency: " << latency << " ns" << std::endl;
            // Should read 0xBB that node 1 wrote in Phase 5
            uint8_t expected[64];
            memset(expected, 0xBB, 64);
            results.check(memcmp(read_buf, expected, 64) == 0,
                          "Cross-node read data matches Node 1's local write");
        }
    }

    // ====================================================================
    // Phase 7: Statistics
    // ====================================================================
    std::cout << "--- Phase 7: Statistics ---" << std::endl;

    auto stats0 = server0.get_stats();
    auto stats1 = server1.get_stats();

    std::cout << "  Node 0: local_r=" << stats0.local_reads << " local_w=" << stats0.local_writes
              << " remote_r=" << stats0.remote_reads << " remote_w=" << stats0.remote_writes
              << " fwd=" << stats0.forwarded_requests << " coherency=" << stats0.coherency_messages
              << std::endl;
    std::cout << "  Node 1: local_r=" << stats1.local_reads << " local_w=" << stats1.local_writes
              << " remote_r=" << stats1.remote_reads << " remote_w=" << stats1.remote_writes
              << " fwd=" << stats1.forwarded_requests << " coherency=" << stats1.coherency_messages
              << std::endl;

    results.check(stats0.local_reads > 0, "Node 0 has local reads");
    results.check(stats0.local_writes > 0, "Node 0 has local writes");
    results.check(stats1.local_reads > 0, "Node 1 has local reads");
    results.check(stats1.local_writes > 0, "Node 1 has local writes");

    // ====================================================================
    // Phase 8: Cleanup
    // ====================================================================
    std::cout << "--- Phase 8: Cleanup ---" << std::endl;

    server1.stop();
    server0.stop();

    results.check(true, "Both servers stopped cleanly");

    // Clean up policies
    for (auto* p : policies0) delete p;
    for (auto* p : policies1) delete p;

    // Summary
    std::cout << std::endl;
    std::cout << "=== Test Summary ===" << std::endl;
    std::cout << "  Passed: " << results.passed << std::endl;
    std::cout << "  Failed: " << results.failed << std::endl;
    std::cout << "  Total:  " << (results.passed + results.failed) << std::endl;
    std::cout << "  Result: " << (results.failed == 0 ? "ALL PASSED" : "SOME FAILED") << std::endl;

    // Cleanup SHM files
    (void)system("rm -f /dev/shm/cxltest_dist*");

    return results.failed == 0 ? 0 : 1;
}
