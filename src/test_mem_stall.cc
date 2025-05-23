#include "policy.h"
#include "rob.h"
#include <iostream>

Helper helper{};
CXLController *controller;
Monitors *monitors;

int main() {
    // Create a controller with minimal configuration
    auto *policy1 = new InterleavePolicy();
    controller = new CXLController({policy1}, 0, CACHELINE, 100, 110);

    // Create ROB with the controller
    Rob rob(controller, 512);

    // Create a memory instruction
    InstructionGroup mem_ins;
    mem_ins.instruction = "ld r1, [mem]";
    mem_ins.address = 0x1000; // Memory address
    mem_ins.fetchTimestamp = 1000;
    mem_ins.cycleCount = 1;
    mem_ins.retireTimestamp = 0;

    // Create a non-memory instruction for comparison
    InstructionGroup nonmem_ins;
    nonmem_ins.instruction = "add r1, r2";
    nonmem_ins.address = 0; // No memory address
    nonmem_ins.fetchTimestamp = 1000;
    nonmem_ins.cycleCount = 1;
    nonmem_ins.retireTimestamp = 0;

    // Issue the memory instruction
    std::cout << "Issuing memory instruction...\n";
    bool issued = rob.issue(mem_ins);
    std::cout << "Memory instruction issued: " << (issued ? "true" : "false") << std::endl;

    // Tick the ROB until the instruction retires
    int ticks = 0;
    while (!rob.queue_.empty() && ticks < 1000) {
        rob.tick();
        ticks++;

        if (ticks % 10 == 0) {
            std::cout << "Ticks: " << ticks << ", Stalls: " << rob.getStallCount()
                      << ", ROB Events: " << rob.getStallEventCount() << std::endl;
        }
    }
    std::cout << "Memory instruction took " << ticks << " cycles to retire\n";
    std::cout << "Final stall count: " << rob.getStallCount() << std::endl;
    std::cout << "Final ROB events: " << rob.getStallEventCount() << std::endl;

    // Reset stall counters
    rob.resetCounters();

    // Now test the non-memory instruction
    std::cout << "\nIssuing non-memory instruction...\n";
    issued = rob.issue(nonmem_ins);
    std::cout << "Non-memory instruction issued: " << (issued ? "true" : "false") << std::endl;

    // Tick the ROB until the instruction retires
    ticks = 0;
    while (!rob.queue_.empty() && ticks < 1000) {
        rob.tick();
        ticks++;

        if (ticks % 10 == 0) {
            std::cout << "Ticks: " << ticks << ", Stalls: " << rob.getStallCount()
                      << ", ROB Events: " << rob.getStallEventCount() << std::endl;
        }
    }
    std::cout << "Non-memory instruction took " << ticks << " cycles to retire\n";
    std::cout << "Final stall count: " << rob.getStallCount() << std::endl;
    std::cout << "Final ROB events: " << rob.getStallEventCount() << std::endl;

    return 0;
}