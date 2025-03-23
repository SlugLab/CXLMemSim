#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <syscall.h>
#include <unistd.h>
#include <signal.h>

// 显示线程ID的辅助函数
void print_thread_id(const std::string& prefix) {
    pid_t tid = syscall(SYS_gettid);
    std::cout << prefix << "线程ID: " << tid << std::endl;
}

// 线程工作函数
void thread_function(int id) {
    print_thread_id("子线程 #" + std::to_string(id) + " ");

    // 模拟一些工作
    std::cout << "线程 #" << id << " 开始工作..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(2));
    std::cout << "线程 #" << id << " 完成工作!" << std::endl;
}

// 演示exit_group的线程函数
void exit_group_thread() {
    print_thread_id("即将退出的线程 ");
    std::cout << "此线程将调用exit_group()终止所有线程..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(3));

    // 使用syscall直接调用exit_group
    // exit_group会终止调用进程的所有线程
    std::cout << "调用exit_group(0)..." << std::endl;
    syscall(SYS_exit_group, 0);

    // 这行代码永远不会执行
    std::cout << "此消息不会显示" << std::endl;
}

int main() {
    
    print_thread_id("主线程 ");

    // 创建一些普通工作线程
    std::vector<std::shared_ptr<std::thread>> threads;
    for (int i = 0; i < 3; ++i) {
    std::cout << "english, finally\n";
	    std::shared_ptr<std::thread> thread = std::make_shared<std::thread>(thread_function, i);
        threads.push_back(thread);
    }

    // 创建将调用exit_group的线程
    std::thread exit_thread(exit_group_thread);

    // 等待所有工作线程完成
    for (auto& t : threads) {
        if (t->joinable()) {
            t->join();
        }
    }

    // 等待exit_group线程
    if (exit_thread.joinable()) {
        exit_thread.join();
    }

    // 由于exit_group被调用，这里的代码也不会执行
    std::cout << "主程序结束" << std::endl;

    return 0;
}
