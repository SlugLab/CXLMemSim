//
// Created by root on 2/19/25.
//

#include "rob.h"
// 发射指令到ROB
bool Rob::issue(const InstructionGroup &ins) {
    if (queue_.size() >= maxSize_) {
        stallCount_++;
        return false; // ROB已满,停顿
    }

    // 将指令加入ROB尾部
    queue_.push_back(ins);

    // 对于内存访问指令,通知控制器
    if (ins.address != 0) {
        counter ++;
        auto lbrs = std::vector<lbr>();
        lbrs.reserve(32);
        controller_->insert(ins.retireTimestamp, 0, ins.address, 0, counter);
        controller_->insert(ins.retireTimestamp, 0, lbrs.data(), {});
    }

    return true;
}

// 检查指令是否可以提交
bool Rob::canRetire(const InstructionGroup &ins) {
    if (ins.address == 0) {
        return true; // 非内存指令可以直接提交
    }

    // 检查内存访问是否完成
    if (cur_latency == 0) {
        auto allAccess = controller_->get_access(ins.retireTimestamp);
        cur_latency = controller_->calculate_latency(allAccess, 80.);
    }
    // SPDLOG_INFO("{}",cur_latency);
    if (currentCycle_ - ins.cycleCount >= cur_latency) {
        cur_latency = 0;
        return true;
    }
    return false;
}

// 提交最老的指令
void Rob::retire() {
    if (queue_.empty()) {
        return;
    }

    auto &oldestIns = queue_.front();
    if (!canRetire(oldestIns)) {
        stallCount_++; // 无法提交,增加停顿
        return;
    }

    // 计算这条指令的实际延迟
    if (oldestIns.address != 0) {
        auto allAccess = controller_->get_access(currentCycle_);
        uint64_t latency = controller_->calculate_latency(allAccess, 80.); // also delete the latency
        totalLatency_ += latency;
    }

    // 提交指令
    queue_.pop_front();
}

// 时钟周期推进
void Rob::tick() {
    currentCycle_++;
    // 尝试提交指令
    retire();
}

// 并行处理所有指令
void ParallelRob::processInstructions(const std::vector<InstructionGroup>& instructions) {
    // 第一步：预处理 - 将指令分配到不同分区
    std::vector<std::vector<InstructionGroup>> partitionedInstructions(NUM_PARTITIONS);
    for (const auto& ins : instructions) {
        size_t partIndex = getPartitionIndex(ins);
        partitionedInstructions[partIndex].push_back(ins);
    }

    // 第二步：并行处理每个分区的指令
    std::vector<std::thread> threads;
    std::atomic<uint64_t> globalMaxCycle{currentCycle_.load()};

    for (int p = 0; p < NUM_PARTITIONS; p++) {
        threads.emplace_back([this, p, &partitionedInstructions, &globalMaxCycle]() {
            auto& partition = this->partitions_[p];
            auto& insForPartition = partitionedInstructions[p];

            // 按照循环计数排序，确保正确的处理顺序
            std::sort(insForPartition.begin(), insForPartition.end(),
                     [](const InstructionGroup& a, const InstructionGroup& b) {
                         return a.cycleCount < b.cycleCount;
                     });

            uint64_t localCycle = this->currentCycle_.load();

            for (const auto& ins : insForPartition) {
                // 处理这条指令
                {
                    std::lock_guard<std::mutex> lock(partition.mutex);  // 修正：使用.而不是->

                    // 等待直到有空间
                    while (partition.queue.size() >= this->maxSize_ / NUM_PARTITIONS) {  // 修正：使用.
                        this->stallCount_++;

                        // 尝试退休指令以释放空间
                        auto it = partition.queue.begin();  // 修正：使用.
                        while (it != partition.queue.end()) {  // 修正：使用.
                            if (this->processRetirement(*it, partition)) {  // 修正：直接传递partition
                                it = partition.queue.erase(it);  // 修正：使用.
                            } else {
                                ++it;
                            }
                        }

                        // 推进本地周期
                        localCycle++;
                    }

                    // 发射指令
                    partition.queue.push_back(ins);  // 修正：使用.

                    // 对于内存访问指令，通知控制器
                    if (ins.address != 0) {
                        int currentCounter = this->counter.fetch_add(1) + 1;
                        auto lbrs = std::vector<lbr>();
                        lbrs.resize(32);
                        this->controller_->insert(ins.retireTimestamp, 0, ins.address, 0, currentCounter*10);
                        this->controller_->insert(ins.retireTimestamp, 0, lbrs.data(), {});
                    }

                    // 尝试退休已完成的指令
                    auto it = partition.queue.begin();  // 修正：使用.
                    while (it != partition.queue.end()) {  // 修正：使用.
                        if (this->processRetirement(*it, partition)) {  // 修正：直接传递partition
                            it = partition.queue.erase(it);  // 修正：使用.
                        } else {
                            ++it;
                        }
                    }

                    // 推进本地周期
                    localCycle++;
                }
            }

            // 更新全局最大周期
            uint64_t current;
            do {
                current = globalMaxCycle.load();
            } while (localCycle > current &&
                    !globalMaxCycle.compare_exchange_weak(current, localCycle));
        });
    }

    // 等待所有线程完成
    for (auto& thread : threads) {
        thread.join();
    }

    // 更新全局周期
    currentCycle_.store(globalMaxCycle.load());

    // 最后阶段：并行清空所有分区队列中的剩余指令
    threads.clear();
    for (int p = 0; p < NUM_PARTITIONS; p++) {
        threads.emplace_back([this, p]() {
            auto& partition = this->partitions_[p];
            uint64_t localCycle = this->currentCycle_.load();

            while (true) {
                bool allEmpty = true;
                {
                    std::lock_guard<std::mutex> lock(partition.mutex);  // 修正：使用.
                    if (!partition.queue.empty()) {  // 修正：使用.
                        allEmpty = false;

                        // 尝试退休指令
                        auto it = partition.queue.begin();  // 修正：使用.
                        while (it != partition.queue.end()) {  // 修正：使用.
                            if (this->processRetirement(*it, partition)) {  // 修正：直接传递partition
                                it = partition.queue.erase(it);  // 修正：使用.
                            } else {
                                ++it;
                            }
                        }

                        // 推进本地周期
                        localCycle++;
                    }
                }

                if (allEmpty) break;
            }

            // 更新全局周期
            int64_t current; // 从uint64_t改为int64_t
            do {
                current = this->currentCycle_.load();
            } while (localCycle > current &&
                    !this->currentCycle_.compare_exchange_weak(current, localCycle));
        });
    }

    // 等待所有线程完成
    for (auto& thread : threads) {
        thread.join();
    }
}

// 处理指令退休
bool ParallelRob::processRetirement(InstructionGroup& ins, RobPartition& partition) {
    if (ins.address == 0) {
        return true; // 非内存指令可以直接提交
    }

    // 检查内存访问是否完成
    if (partition.cur_latency == 0) {
        auto allAccess = controller_->get_access(ins.retireTimestamp);
        partition.cur_latency = controller_->calculate_latency(allAccess, 80.);
    }

    if (currentCycle_.load() - ins.cycleCount >= partition.cur_latency) {
        // 计算实际延迟
        auto allAccess = controller_->get_access(currentCycle_.load());
        uint64_t latency = controller_->calculate_latency(allAccess, 80.);
        totalLatency_ += latency;

        partition.cur_latency = 0;
        return true;
    }

    stallCount_++; // 无法提交，增加停顿
    return false;
}

double ParallelRob::getAverageLatency()  {
    size_t totalInstructions = 0;
    for (auto& part : partitions_) {
        std::lock_guard<std::mutex> lock(part.mutex);  // 修正：使用.而不是->
        totalInstructions += part.queue.size();  // 修正：使用.
    }

    return totalInstructions == 0 ? 0 :
        static_cast<double>(totalLatency_.load()) / totalInstructions;
}