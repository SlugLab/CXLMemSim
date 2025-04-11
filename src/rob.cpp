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
        totalLatency_ += cur_latency;

        // Update the retire timestamp directly to reflect the calculated latency
        const_cast<InstructionGroup &>(ins).retireTimestamp += cur_latency;
    }

    uint64_t waitTime = currentCycle_ - ins.cycleCount;

    // 延迟期间增加停顿计数
    if (waitTime < cur_latency) {
        stallCount_++;

        // ROB事件与停顿计数差异化
        stallEventCount_++;
        return false;
    }

    cur_latency = 0;
    return true;
}

// 修改tick函数，限制每周期退休指令数量
void Rob::tick() {
    currentCycle_++;

    // 打印队列状态
    if (currentCycle_ % 10000 == 0) {
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

    // 关键：周期性放慢退休速度，模拟处理器后端压力
    if (currentCycle_ % 64 == 0 && !queue_.empty()) {
        stallCount_++; // 额外停顿增加积压
        if (stallCount_ % 10 == 0)
            stallEventCount_++;
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

// 并行处理所有指令
void ParallelRob::processInstructions(const std::vector<InstructionGroup> &instructions) {
    SPDLOG_INFO("Starting to process {} instructions across {} partitions", instructions.size(), NUM_PARTITIONS);

    // 第一步：预处理 - 将指令分配到不同分区
    std::vector<std::vector<InstructionGroup>> partitionedInstructions(NUM_PARTITIONS);
    for (const auto &ins : instructions) {
        size_t partIndex = getPartitionIndex(ins);
        partitionedInstructions[partIndex].push_back(ins);
    }

    SPDLOG_INFO("Instruction partitioning complete. Partition sizes:");
    for (int p = 0; p < NUM_PARTITIONS; p++) {
        SPDLOG_INFO("  Partition {}: {} instructions", p, partitionedInstructions[p].size());
    }

    // 为了跟踪处理进度
    std::atomic<uint64_t> globalProcessedCount{0};
    std::atomic<uint64_t> globalRetiredCount{0};

    // 第二步：并行处理每个分区的指令
    std::vector<std::thread> threads;
    std::atomic<uint64_t> globalMaxCycle{static_cast<uint64_t>(currentCycle_.load())};
    std::atomic<bool> allPartitionsProcessed{false};

    SPDLOG_INFO("Starting parallel processing phase...");

    for (int p = 0; p < NUM_PARTITIONS; p++) {
        threads.emplace_back([this, p, &partitionedInstructions, &globalMaxCycle, &allPartitionsProcessed,
                              &globalProcessedCount, &globalRetiredCount,
                              &instructions]() { // 添加instructions到捕获列表
            auto &partition = this->partitions_[p];
            auto &insForPartition = partitionedInstructions[p];
            const size_t totalInPartition = insForPartition.size();

            SPDLOG_INFO("Thread {} started processing {} instructions", p, totalInPartition);

            // 按照循环计数排序，确保正确的处理顺序
            std::sort(insForPartition.begin(), insForPartition.end(),
                      [](const InstructionGroup &a, const InstructionGroup &b) { return a.cycleCount < b.cycleCount; });

            uint64_t localCycle = this->currentCycle_.load();
            uint64_t localProcessed = 0;

            for (const auto &ins : insForPartition) {
                // 处理这条指令
                const int MAX_RETRIES = 100;
                int retries = 0;
                bool instructionQueued = false;

                while (!instructionQueued && retries < MAX_RETRIES) {
                    // 获取锁前先短暂等待，如果失败多次
                    if (retries > 10) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }

                    // 尝试获取锁
                    bool lockAcquired = partition.mutex.try_lock();
                    if (!lockAcquired) {
                        retries++;
                        if (retries % 20 == 0) {
                            SPDLOG_DEBUG("Thread {} waiting for lock, retry {}/{}", p, retries, MAX_RETRIES);
                        }
                        continue;
                    }
                    // 尝试加锁成功后的代码
                    std::unique_lock<std::mutex> lock(partition.mutex, std::adopt_lock);

                    // 检查队列是否有空间
                    if (partition.queue.size() < this->maxSize_ / NUM_PARTITIONS) {
                        // 发射指令
                        partition.queue.push_back(ins);
                        instructionQueued = true;

                        // 对于内存访问指令，通知控制器
                        if (ins.address != 0) {
                            int currentCounter = this->counter.fetch_add(1) + 1;
                            this->controller_->insert(ins.retireTimestamp, 0, ins.address, 0, currentCounter * 10);
                        }

                        // 尝试退休已完成的指令
                        auto it = partition.queue.begin();
                        while (it != partition.queue.end()) {
                            if (this->processRetirement(*it, partition)) {
                                it = partition.queue.erase(it);
                            } else {
                                ++it;
                            }
                        }

                        // 推进本地周期
                        localCycle++;
                        // 处理完一条指令，更新进度计数
                        localProcessed++;
                        uint64_t totalProcessed = globalProcessedCount.fetch_add(1) + 1;
                        if (totalProcessed % 10000 == 0) {
                            SPDLOG_INFO("Processed {} instructions ({:.1f}% of total)", totalProcessed,
                                        (totalProcessed * 100.0) / instructions.size());
                        }
                    } else {
                        // 队列已满，尝试退休一些指令
                        this->stallCount_++;
                        bool retired = false;
                        auto it = partition.queue.begin();
                        while (it != partition.queue.end()) {
                            if (this->processRetirement(*it, partition)) {
                                it = partition.queue.erase(it);
                                retired = true;
                            } else {
                                ++it;
                            }
                        }

                        // 如果没有能退休的指令，临时释放锁，允许其他线程工作
                        if (!retired) {
                            lock.unlock();
                            std::this_thread::yield(); // 让出CPU时间片
                            localCycle++; // 仍然推进本地周期
                        }
                    }

                    // 处理完一条指令
                    localProcessed++;
                    uint64_t totalProcessed = globalProcessedCount.fetch_add(1) + 1;

                    if (totalProcessed % 10000 == 0) {
                        SPDLOG_INFO("Processed {} instructions ({:.1f}% of total)", totalProcessed,
                                    (totalProcessed * 100.0) / instructions.size());
                    }
                }

                // 每处理1000条指令报告一次局部进度
                if (localProcessed % 1000 == 0) {
                    SPDLOG_INFO("Thread {} processed {}/{} instructions ({:.1f}%)", p, localProcessed, totalInPartition,
                                (localProcessed * 100.0) / totalInPartition);
                }
            }

            SPDLOG_INFO("Thread {} completed instruction processing phase", p);

            // 更新全局最大周期
            // 使用正确类型的变量
            auto current = globalMaxCycle.load();
            do {
                current = globalMaxCycle.load();
            } while (localCycle > current && !globalMaxCycle.compare_exchange_weak(current, localCycle));
        });
    }

    // 等待所有线程完成
    for (auto &thread : threads) {
        thread.join();
    }

    SPDLOG_INFO("All processing threads completed. Total processed: {}", globalProcessedCount.load());

    allPartitionsProcessed = true;

    // 更新全局周期
    currentCycle_.store(globalMaxCycle.load());
    SPDLOG_INFO("Global cycle updated to {}", currentCycle_.load());

    // 最后阶段：清空所有分区队列中的剩余指令
    SPDLOG_INFO("Starting cleanup phase...");

    threads.clear();

    // 设置超时机制检测清理阶段
    std::atomic<bool> cleanupComplete{false};
    std::thread timeoutThread([&cleanupComplete]() {
        for (int i = 0; i < 50; i++) { // 5秒超时，每100ms检查一次
            if (cleanupComplete) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!cleanupComplete) {
            SPDLOG_ERROR("WARNING: Cleanup phase timeout after 5 seconds, possible deadlock detected");
        }
    });

    for (int p = 0; p < NUM_PARTITIONS; p++) {
        threads.emplace_back([this, p, &globalRetiredCount]() {
            auto &partition = this->partitions_[p];
            uint64_t localCycle = this->currentCycle_.load();
            const int MAX_ITERATIONS = 1000;
            int iterations = 0;
            size_t initialQueueSize = 0;

            {
                std::lock_guard<std::mutex> lock(partition.mutex);
                initialQueueSize = partition.queue.size();
            }

            SPDLOG_INFO("Cleanup thread {} started, initial queue size: {}", p, initialQueueSize);
            uint64_t localRetired = 0;

            while (iterations < MAX_ITERATIONS) {
                bool allEmpty = true;
                bool lockAcquired = partition.mutex.try_lock();

                if (!lockAcquired) {
                    if (iterations % 100 == 0) {
                        SPDLOG_DEBUG("Cleanup thread {} waiting for lock, iteration {}", p, iterations);
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    iterations++;
                    continue;
                }

                {
                    std::unique_lock<std::mutex> lock(partition.mutex, std::adopt_lock);

                    size_t queueSizeBefore = partition.queue.size();
                    if (!partition.queue.empty()) {
                        allEmpty = false;
                        int retiredInThisIteration = 0;

                        auto it = partition.queue.begin();
                        while (it != partition.queue.end()) {
                            if (this->processRetirement(*it, partition)) {
                                it = partition.queue.erase(it);
                                retiredInThisIteration++;
                                localRetired++;
                                globalRetiredCount.fetch_add(1);
                            } else {
                                ++it;
                            }
                        }

                        if (retiredInThisIteration > 0 && localRetired % 1000 == 0) {
                            SPDLOG_INFO("Cleanup thread {} retired {} instructions, queue size: {}->{}", p,
                                        localRetired, queueSizeBefore, partition.queue.size());
                        }

                        // 没有退休指令时，强制推进周期
                        if (retiredInThisIteration == 0) {
                            localCycle += 5;
                            if (iterations % 50 == 0) {
                                SPDLOG_INFO("Cleanup thread {} forced cycle advancement to {}, queue size: {}", p,
                                            localCycle, partition.queue.size());
                            }
                        } else {
                            localCycle++;
                        }
                    }
                } // 锁在这里自动释放

                if (allEmpty) {
                    SPDLOG_INFO("Cleanup thread {} completed, all instructions retired ({} total)", p, localRetired);
                    break;
                }

                // 定期报告进度
                if (iterations % 100 == 99) {
                    std::lock_guard<std::mutex> lock(partition.mutex);
                    SPDLOG_INFO("Cleanup progress: thread {}, iterations {}, remaining instructions: {}", p, iterations,
                                partition.queue.size());

                    // 更新全局周期
                    // 使用正确类型的变量
                    typename decltype(this->currentCycle_)::value_type current = this->currentCycle_.load();
                    if (localCycle > current) {
                        this->currentCycle_.compare_exchange_strong(current, localCycle);
                        SPDLOG_INFO("Updated global cycle to {}", localCycle);
                    }
                }

                iterations++;
            }

            // 达到最大迭代次数时强制清空
            if (iterations >= MAX_ITERATIONS) {
                std::lock_guard<std::mutex> lock(partition.mutex);
                size_t remainingCount = partition.queue.size();
                if (remainingCount > 0) {
                    SPDLOG_WARN("Forced queue cleanup after max iterations, discarding {} instructions",
                                remainingCount);
                    partition.queue.clear();
                }
            }

            // 最终更新全局周期
            // 使用正确类型的变量
            typename decltype(this->currentCycle_)::value_type current = this->currentCycle_.load();
            if (localCycle > current) {
                this->currentCycle_.compare_exchange_strong(current, localCycle);
                SPDLOG_INFO("Final global cycle update to {}", localCycle);
            }
        });
    }

    // 等待所有清理线程完成
    for (auto &thread : threads) {
        thread.join();
    }

    cleanupComplete = true;
    timeoutThread.join(); // 等待超时监控线程结束

    SPDLOG_INFO("Instruction processing complete. Final stats:");
    SPDLOG_INFO("  - Total processed: {}", globalProcessedCount.load());
    SPDLOG_INFO("  - Total retired: {}", globalRetiredCount.load());
    SPDLOG_INFO("  - Final cycle: {}", currentCycle_.load());
    SPDLOG_INFO("  - Stall count: {}", stallCount_.load());
}
// 处理指令退休
bool ParallelRob::processRetirement(InstructionGroup &ins, RobPartition &partition) {
    if (ins.address == 0) {
        return true; // 非内存指令可以直接提交
    }

    // 检查内存访问是否完成
    if (partition.cur_latency == 0) {
        auto allAccess = controller_->get_access(ins.retireTimestamp);
        partition.cur_latency = controller_->calculate_latency(allAccess, 80.);

        // Directly update the retire timestamp to include the calculated latency
        ins.retireTimestamp += static_cast<long long>(partition.cur_latency);
    }

    if (currentCycle_.load() - ins.cycleCount >= partition.cur_latency) {
        // 计算实际延迟
        robCount_++;
        auto allAccess = controller_->get_access(currentCycle_.load());
        uint64_t latency = controller_->calculate_latency(allAccess, 80.);
        totalLatency_ += latency;

        partition.cur_latency = 0;
        return true;
    }

    stallCount_++; // 无法提交，增加停顿
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