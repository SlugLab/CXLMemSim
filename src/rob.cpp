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
    // 基本整数ALU操作 (IntAlu) - 1周期
    {"mov", 1}, // MOV_R_R: 寄存器间移动
    {"add", 1}, // ADD_R_R, ADD_R_I: 加法操作
    {"sub", 1}, // SUB_R_I: 减法操作
    {"and", 1}, // TEST_R_R: 位与操作
    {"or", 1}, // 位或操作
    {"xor", 1}, // 位异或操作
    {"cmp", 1}, // CMP_M_I: 比较操作 (ALU部分)
    {"limm", 1}, // 立即数加载
    {"rdip", 1}, // 读取指令指针
    {"wrip", 1}, // 写入指令指针 (控制转移)

    // 整数乘法 (IntMult) - 3周期
    {"mul", 3}, // 整数乘法
    {"imul", 3}, // 有符号整数乘法

    // 整数除法 (IntDiv) - x86特殊优化为1周期
    {"div", 1}, // x86除法微操作已优化
    {"idiv", 1}, // x86有符号除法微操作已优化

    // 内存访问操作 - 延迟主要由缓存决定，这里是基础延迟
    {"ld", 1}, // 内存加载基础延迟 (不包括缓存miss)
    {"st", 1}, // 内存存储基础延迟

    // 分支操作 - 主要是条件码设置
    {"jz", 1}, // JZ_I: 零标志跳转的准备工作
    {"jnz", 1}, // JNZ_I: 非零标志跳转的准备工作
    {"jmp", 1}, // 无条件跳转
    {"je", 1}, // 相等跳转
    {"jne", 1}, // 不等跳转
    {"jl", 1}, // 小于跳转
    {"jg", 1}, // 大于跳转
    {"jle", 1}, // 小于等于跳转
    {"jge", 1}, // 大于等于跳转

    // 浮点运算 (FP_ALU) - 2周期
    {"fadd", 2}, // FloatAdd: 浮点加法
    {"fsub", 2}, // FloatAdd: 浮点减法
    {"fcmp", 2}, // FloatCmp: 浮点比较
    {"fcvt", 2}, // FloatCvt: 浮点转换

    // 浮点乘除法 (FP_MultDiv)
    {"fmul", 4}, // FloatMult: 浮点乘法 - 4周期
    {"fmadd", 5}, // FloatMultAcc: 浮点累乘 - 5周期
    {"fdiv", 12}, // FloatDiv: 浮点除法 - 12周期，非流水线
    {"fsqrt", 24}, // FloatSqrt: 浮点开方 - 24周期，非流水线
    {"fmisc", 3}, // FloatMisc: 其他浮点操作 - 3周期

    // SIMD操作 - 多数为1周期
    {"simd_add", 1}, // SIMD加法
    {"simd_mul", 1}, // SIMD乘法
    {"simd_cmp", 1}, // SIMD比较
    {"simd_cvt", 1}, // SIMD转换
    {"simd_misc", 1}, // SIMD杂项操作

    // x86特有微操作
    {"lea", 1}, // 有效地址计算
    {"nop", 1}, // 空操作
    {"push", 1}, // 压栈 (不包括内存访问延迟)
    {"pop", 1}, // 出栈 (不包括内存访问延迟)
    {"call", 1}, // 函数调用 (不包括内存访问延迟)
    {"ret", 1}, // 函数返回 (不包括内存访问延迟)

    // 位操作
    {"shl", 1}, // 逻辑左移
    {"shr", 1}, // 逻辑右移
    {"sar", 1}, // 算术右移
    {"rol", 1}, // 循环左移
    {"ror", 1}, // 循环右移

    // 其他常见x86指令
    {"inc", 1}, // 自增
    {"dec", 1}, // 自减
    {"neg", 1}, // 取负
    {"not", 1}, // 位取反
    {"test", 1}, // 测试 (设置标志位)
    {"cmov", 1}, // 条件移动

    // 字符串操作 (简化)
    {"movs", 1}, // 字符串移动
    {"stos", 1}, // 字符串存储
    {"lods", 1}, // 字符串加载
    {"scas", 1}, // 字符串扫描
    {"cmps", 1}, // 字符串比较

    // 特殊指令
    {"cpuid", 20}, // CPU信息查询 - 较高延迟
    {"rdtsc", 3}, // 读取时间戳计数器
    {"mfence", 1}, // 内存栅栏
    {"sfence", 1}, // 存储栅栏
    {"lfence", 1} // 加载栅栏
};
// 发射指令到ROB
// 修改issue函数，调整发射逻辑
bool Rob::issue(const InstructionGroup &ins) {
    // 保持原有的队列满检查
    if (queue_.size() >= maxSize_) {
        stallCount_++;
        stallEventCount_++;
        return false; // ROB已满，停顿
    }

    // 将指令加入ROB尾部
    queue_.push_back(ins);

    // 对于内存访问指令，通知控制器
    if (ins.address != 0) {
        counter++;
        auto lbrs = std::vector<lbr>();
        lbrs.resize(32);
        controller_->insert(ins.retireTimestamp, 0, ins.address, 0, counter);
        // controller_->insert(ins.retireTimestamp, 1, lbrs.data(), {});
    }

    // 关键：添加批次控制，每次最多发射N条指令
    // 这会使队列有机会累积多条指令
    static int issueCount = 0;
    if (++issueCount % 4 == 0) { // 每发射4条后暂停一个周期
        issueCount = 0;
        return false; // 示意需要暂停发射
    }

    return true;
}

// 修改canRetire函数，确保内存指令有足够的延迟
bool Rob::canRetire(const InstructionGroup &ins) {
    // 非内存指令可以立即退休，不需要等待
    if (ins.address == 0) {
        return true;
    }

    // 内存指令的退休逻辑
    if (cur_latency == 0) {
        auto allAccess = controller_->get_access(ins.retireTimestamp);
        // 确保延迟不会被过度缩减
        double baseLatency = controller_->calculate_latency(allAccess, 80.);
        // 关键：设置最小延迟阈值
        cur_latency = std::max(10L, static_cast<long>(baseLatency));
        for (const auto &[instr, latency] : instructionLatencyMap) {
            if (ins.instruction.find(instr) != std::string::npos) {
                cur_latency += latency;
                break;
            }
        }

        // 关键修改：为内存访问延迟的每个周期增加一次stall计数
        // 这模拟了传统意义上的ROB停顿
        stallCount_ += cur_latency;
        stallEventCount_ += 1; // 事件计数略少于实际停顿

        currentCycle_ += cur_latency;

        // Update the retire timestamp directly to reflect the calculated latency
        const_cast<InstructionGroup &>(ins).retireTimestamp += cur_latency;
    } else {
        for (const auto &[instr, latency] : instructionLatencyMap) {
            if (ins.instruction.find(instr) != std::string::npos) {
                cur_latency += latency;
                break;
            }
        }

        currentCycle_ += cur_latency;
    }

    uint64_t waitTime = currentCycle_ - ins.cycleCount;

    // 延迟期间增加停顿计数 - 只有在特殊情况下才会触发
    if (waitTime < cur_latency) {
        stallCount_++;
        stallEventCount_++;
        return false;
    }

    cur_latency = 0;
    return true;
}

// 修改tick函数，限制每周期退休指令数量
void Rob::tick() {
    currentCycle_++;

    // 打印队列状态，更频繁地显示
    if (currentCycle_ % 5000 == 0) {
        SPDLOG_INFO("Cycle {}: Queue size {}, Stalls {}, ROB Events {}", currentCycle_, queue_.size(), stallCount_,
                    stallEventCount_);
    }

    // 关键：限制每个周期最多退休的指令数量
    // 这确保队列中的指令不会被过快清空
    const int MAX_RETIRE_PER_CYCLE = 1;

    for (int i = 0; i < MAX_RETIRE_PER_CYCLE && !queue_.empty(); i++) {
        auto &oldestIns = queue_.front();
        if (!canRetire(oldestIns)) {
            break; // 遇到不能退休的指令就停止
        }
        queue_.pop_front();
    }

    // 周期性放慢退休速度，更频繁地模拟处理器后端压力
    // 当队列积累超过8条指令时，开始增加额外停顿
    if (currentCycle_ % 32 == 0 && queue_.size() > 8) {
        stallCount_ += 2; // 额外停顿增加积压
        stallEventCount_++;
        if (currentCycle_ % 1000 == 0) {
            SPDLOG_INFO("Added periodic stall at cycle {}, queue size {}", currentCycle_, queue_.size());
        }
    }
}

// 添加新函数来处理非常规退休（确保计数器正确更新）
bool Rob::tryAlternativeRetire() {
    // 检查队列中的非头部指令
    auto it = queue_.begin();
    ++it; // 跳过队列头部

    for (int i = 0; i < 5 && it != queue_.end(); ++i, ++it) {
        if (it->address == 0) { // 如果是非内存指令
            queue_.erase(it);
            return true;
        }
    }

    // 如果找不到可以退休的指令，记录一次停顿
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