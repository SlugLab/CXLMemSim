/*
 * CXLMemSim rob
 *
 *  By: Andrew Quinn
 *      Yiwei Yang
 *      Brian Zhao
 *  SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
 *  Copyright 2025 Regents of the University of California
 *  UC Santa Cruz Sluglab.
 */

#include "rob.h"
#include <fstream>

std::unordered_map<std::string, int> instructionLatencyMap = {
    // ALU (IntAlu) - 1 (matching gem5's IntAluOp)
    {"mov", 1}, // MOV_R_R:  - matches gem5 exactly
    {"add", 1}, // ADD_R_R, ADD_R_I:
    {"sub", 1}, // SUB_R_I:
    {"and", 1}, // TEST_R_R:
    {"or", 1},
    {"xor", 1},
    {"cmp", 1}, // CMP_M_I:  (ALU)
    {"limm", 1},
    {"rdip", 1},
    {"wrip", 1}, //  ()

    //  (IntMult) - 3
    {"mul", 3},
    {"imul", 3},
    //  (IntDiv) - x861
    {"div", 1}, // x86
    {"idiv", 1}, // x86

    //  -
    {"ld", 1}, //  (miss)
    {"st", 1},
    //  -
    {"jz", 1}, // JZ_I:
    {"jnz", 1}, // JNZ_I:
    {"jmp", 1},
    {"je", 1},
    {"jne", 1},
    {"jl", 1},
    {"jg", 1},
    {"jle", 1},
    {"jge", 1},
    //  (FP_ALU) - 2
    {"fadd", 2}, // FloatAdd:
    {"fsub", 2}, // FloatAdd:
    {"fcmp", 2}, // FloatCmp:
    {"fcvt", 2}, // FloatCvt:

    //  (FP_MultDiv)
    {"fmul", 4}, // FloatMult:  - 4
    {"fmadd", 5}, // FloatMultAcc:  - 5
    {"fdiv", 12}, // FloatDiv:  - 12
    {"fsqrt", 24}, // FloatSqrt:  - 24
    {"fmisc", 3}, // FloatMisc:  - 3

    // SIMD - 1
    {"simd_add", 1}, // SIMD
    {"simd_mul", 1}, // SIMD
    {"simd_cmp", 1}, // SIMD
    {"simd_cvt", 1}, // SIMD
    {"simd_misc", 1}, // SIMD

    // x86
    {"lea", 1},
    {"nop", 1},
    {"push", 1}, //  ()
    {"pop", 1}, //  ()
    {"call", 1}, //  ()
    {"ret", 1}, //  ()

    {"shl", 1},
    {"shr", 1},
    {"sar", 1},
    {"rol", 1},
    {"ror", 1},
    // x86
    {"inc", 1},
    {"dec", 1},
    {"neg", 1},
    {"not", 1},
    {"test", 1}, //  ()
    {"cmov", 1},
    //  ()
    {"movs", 1},
    {"stos", 1},
    {"lods", 1},
    {"scas", 1},
    {"cmps", 1},
    {"cpuid", 20}, // CPU -
    {"rdtsc", 3},
    {"mfence", 1},
    {"sfence", 1},
    {"lfence", 1}};
// ROB
// issue
bool Rob::issue(const InstructionGroup &ins) {
    if (queue_.size() >= maxSize_) {
        stallCount_++;
        stallEventCount_++;
        return false; // ROB
    }

    // ROB
    queue_.push_back(ins);

    if (ins.address != 0) {
        counter++;
        auto lbrs = std::vector<lbr>();
        lbrs.resize(32);
        controller_->insert(ins.retireTimestamp, 0, ins.address, 0, counter); // congestion
        controller_->insert(ins.retireTimestamp, 1, lbrs.data(), {});
    }

    // Remove artificial issue stalls to match gem5's high-throughput design
    // gem5 can issue up to 8 instructions per cycle without artificial delays

    return true;
}

// canRetire
bool Rob::canRetire(const InstructionGroup &ins) {
    // Fast path for non-memory instructions (like gem5's register operations)
    if (ins.address == 0) {
        // For register-only operations, apply minimal latency like gem5
        if (ins.instruction.find("mov") != std::string::npos) {
            // MOV register operations are 1-cycle like gem5's IntAluOp
            return true;
        }
        return true;
    }

    if (cur_latency == 0) {
        auto allAccess = controller_->get_access(ins.retireTimestamp);
        double baseLatency = controller_->calculate_latency(allAccess, 80.);

        // Check if this is a simple register operation (like MOV)
        bool isSimpleRegOp = ins.instruction.find("mov") != std::string::npos ||
                             ins.instruction.find("add") != std::string::npos ||
                             ins.instruction.find("sub") != std::string::npos;

        if (isSimpleRegOp && ins.address == 0) {
            // For register-only operations, use fixed 1-cycle latency like gem5
            cur_latency = 1;
        } else {
            // For memory operations, apply pipeline-aware latency calculation
            // The CXL pipeline model now handles overlapping requests
            baseLatency *= 0.08; // Reduced further to account for pipeline benefits

            // Check for pipeline optimization scenarios
            if (queue_.size() > 1) {
                // Multiple instructions in flight, benefit from pipeline
                double pipeline_factor = std::min(0.7, 0.9 - (queue_.size() * 0.05));
                baseLatency *= pipeline_factor;
            }

            // Apply corrected minimum threshold
            cur_latency = std::max(1.0, baseLatency);

            // For memory operations, the baseLatency already includes CXL overhead
            // No need to add instruction latency again for memory ops
        }

        // Apply proportional stall count (no excessive multiplier)
        stallCount_ += cur_latency;
        if (stallCount_ % 2 == 0) {
            stallEventCount_ += 1;
        }

        currentCycle_ += cur_latency;
        const_cast<InstructionGroup &>(ins).retireTimestamp += cur_latency;
    } else {
        // For already calculated latency, just apply minimal additional cycles
        cur_latency = 1; // Minimal additional latency for subsequent processing
        currentCycle_ += cur_latency;
    }

    // Simplified wait time logic to match gem5's behavior
    // Only stall if there's a real dependency or resource conflict
    uint64_t waitTime = currentCycle_ - ins.cycleCount;

    // More lenient stall condition - only stall for significant delays
    if (waitTime < (cur_latency / 2)) { // Reduced stall threshold
        stallCount_++;
        stallEventCount_++;
        return false;
    }

    cur_latency = 0;
    return true;
}

// tick
void Rob::tick() {
    currentCycle_++;

    if (currentCycle_ % 5000 == 0) {
        SPDLOG_INFO("Cycle {}: Queue size {}, Stalls {}, ROB Events {}", currentCycle_, queue_.size(), stallCount_,
                    stallEventCount_);
    }

    // Increase retirement throughput to match gem5's out-of-order capabilities
    // gem5 can retire multiple instructions per cycle
    const int MAX_RETIRE_PER_CYCLE = 4;

    for (int i = 0; i < MAX_RETIRE_PER_CYCLE && !queue_.empty(); i++) {
        auto &oldestIns = queue_.front();
        if (!canRetire(oldestIns)) {
            break;
        }
        queue_.pop_front();
    }

    // Remove periodic artificial stalls to match gem5's performance
    // Only stall when there are real resource constraints or dependencies
}

bool Rob::tryAlternativeRetire() {
    auto it = queue_.begin();
    ++it;
    for (int i = 0; i < 5 && it != queue_.end(); ++i, ++it) {
        if (it->address == 0) {
            queue_.erase(it);
            return true;
        }
    }

    stallCount_++;
    stallEventCount_++;
    return false;
}

// Add a method to save trace periodically during simulation
void Rob::saveInstructionTrace(const std::vector<InstructionGroup> &instructions, const std::string &outputFile,
                               bool append) {
    std::ofstream outFile;
    if (append) {
        outFile.open(outputFile, std::ios::app);
    } else {
        outFile.open(outputFile);
    }

    if (!outFile) {
        SPDLOG_ERROR("Failed to open trace output file: {}", outputFile);
        return;
    }

    // Filter instructions that are ready to be saved (have been processed)
    for (const auto &ins : instructions) {
        // Skip instructions that haven't been processed yet
        if (ins.retireTimestamp == 0)
            continue;

        // Generate trace format for this instruction
        long long fetchTS = ins.fetchTimestamp;
        long long decodeTS = fetchTS + 500;
        long long renameTS = fetchTS + 1000;
        long long dispatchTS = fetchTS + 1500;
        long long issueTS = fetchTS + 1500;
        long long completeTS = ins.retireTimestamp - 500;
        long long retireTS = ins.retireTimestamp;

        // Write pipeline stages
        outFile << "O3PipeView:fetch:" << fetchTS << ":" << std::hex << "0x" << ins.address << std::dec
                << ":0:" << ins.cycleCount << ":  " << ins.instruction << std::endl;
        outFile << "O3PipeView:decode:" << decodeTS << std::endl;
        outFile << "O3PipeView:rename:" << renameTS << std::endl;
        outFile << "O3PipeView:dispatch:" << dispatchTS << std::endl;
        outFile << "O3PipeView:issue:" << issueTS << std::endl;
        outFile << "O3PipeView:complete:" << completeTS << std::endl;

        // Handle memory/non-memory instructions
        if (ins.address != 0) {
            std::string memOpType = "store";
            if (ins.instruction.find("ld") != std::string::npos || ins.instruction.find("load") != std::string::npos) {
                memOpType = "load";
            }

            outFile << "O3PipeView:retire:" << retireTS << ":" << memOpType << ":" << (retireTS + 1000) << std::endl;
            outFile << "O3PipeView:address:" << ins.address << std::endl;
        } else {
            outFile << "O3PipeView:retire:" << retireTS << ":store:0" << std::endl;
        }
    }

    outFile.close();
}
