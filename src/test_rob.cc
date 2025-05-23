#include "policy.h"
#include "rob.h"
#include <iostream>

Helper helper{};
CXLController *controller;
Monitors *monitors;
int main() {
    // Create a controller with minimal configuration
    auto *policy1 = new InterleavePolicy();
    auto *controller = new CXLController({policy1}, 0, CACHELINE, 100, 110);

    // Create ROB with the controller
    Rob rob(controller, 512);

    // Create a non-memory instruction
    InstructionGroup ins;
    ins.instruction = "mov r1, r1";
    ins.address = 0; // No memory address
    ins.fetchTimestamp = 1000;
    ins.cycleCount = 1;
    ins.retireTimestamp = 0;

    // Issue the instruction
    bool issued = rob.issue(ins);
    std::cout << "Instruction issued: " << (issued ? "true" : "false") << std::endl;

    // Tick the ROB to process the instruction
    rob.tick();

    // Print stall statistics
    std::cout << "Stalls: " << rob.getStallCount() << std::endl;
    std::cout << "ROB Events: " << rob.getStallEventCount() << std::endl;

    return 0;
}