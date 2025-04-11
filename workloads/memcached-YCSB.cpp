#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <regex>
#include <string>
#include <thread>

// 用于管理系统进程的类
class ProcessManager {
private:
    int memcached_pid = -1;
    int ycsb_pid = -1;
    std::string latency_file = "latency_output.txt";

public:
    // 启动memcached进程 (使用system而不是fork)
    bool start_memcached() {
        std::cout << "启动memcached服务..." << std::endl;

        // 将命令放入后台运行并保存PID
        std::string cmd = "memcached -p 11211 -u try > /dev/null 2>&1 & echo $!";
        FILE *pipe = popen(cmd.c_str(), "r");
        if (!pipe) {
            std::cerr << "无法执行memcached命令" << std::endl;
            return false;
        }

        char buffer[128];
        std::string result = "";
        while (!feof(pipe)) {
            if (fgets(buffer, 128, pipe) != NULL)
                result += buffer;
        }
        pclose(pipe);

        memcached_pid = std::stoi(result);
        std::cout << "memcached进程已启动，PID: " << memcached_pid << std::endl;

        // 等待服务启动
        std::this_thread::sleep_for(std::chrono::seconds(2));
        return true;
    }

    // 运行YCSB客户端
    bool run_ycsb() {
        std::cout << "启动YCSB客户端..." << std::endl;

        // 切换到YCSB目录
        chdir("../workloads/YCSB");

        // 运行YCSB并重定向输出到文件
        std::string cmd = "./bin/ycsb run memcached -s -P ./workloads/workloadb -p "
                          "memcached.hosts=127.0.0.1 > " +
                          latency_file + " 2>&1 & echo $!";
        FILE *pipe = popen(cmd.c_str(), "r");
        if (!pipe) {
            std::cerr << "无法执行YCSB命令" << std::endl;
            return false;
        }

        char buffer[128];
        std::string result = "";
        while (!feof(pipe)) {
            if (fgets(buffer, 128, pipe) != NULL)
                result += buffer;
        }
        pclose(pipe);

        ycsb_pid = std::stoi(result);
        std::cout << "YCSB进程已启动，PID: " << ycsb_pid << std::endl;
        return true;
    }

    // 等待YCSB完成
    bool wait_for_ycsb() {
        if (ycsb_pid <= 0)
            return false;

        std::cout << "等待YCSB客户端完成..." << std::endl;

        std::string cmd = "ps -p " + std::to_string(ycsb_pid) + " > /dev/null 2>&1";
        while (system(cmd.c_str()) == 0) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        std::cout << "YCSB客户端已完成" << std::endl;
        return true;
    }

    // 解析延迟结果
    void parse_latency_results() {
        std::cout << "\n--- 延迟结果 ---\n";

        std::ifstream file(latency_file);
        if (!file.is_open()) {
            std::cerr << "无法打开文件: " << latency_file << std::endl;
            return;
        }

        std::string line;
        std::regex latency_pattern("(AverageLatency|PercentileLatency)");

        while (std::getline(file, line)) {
            if (std::regex_search(line, latency_pattern)) {
                std::cout << line << std::endl;
            }
        }

        file.close();
    }

    // 停止memcached进程
    void stop_memcached() {
        if (memcached_pid <= 0)
            return;

        std::cout << "停止memcached服务 (PID: " << memcached_pid << ")..." << std::endl;
        std::string cmd = "kill " + std::to_string(memcached_pid);
        system(cmd.c_str());

        // 确认进程已终止
        cmd = "ps -p " + std::to_string(memcached_pid) + " > /dev/null 2>&1";
        while (system(cmd.c_str()) == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        std::cout << "memcached服务已停止" << std::endl;
    }

    // 清理资源
    ~ProcessManager() { stop_memcached(); }
};

int main(int argc, char *argv[]) {
    ProcessManager pm;

    // 1. 启动memcached
    if (!pm.start_memcached()) {
        std::cerr << "启动memcached失败，退出程序" << std::endl;
        return EXIT_FAILURE;
    }

    // 2. 运行YCSB客户端
    if (!pm.run_ycsb()) {
        std::cerr << "启动YCSB客户端失败，退出程序" << std::endl;
        return EXIT_FAILURE;
    }

    // 3. 等待YCSB客户端完成
    pm.wait_for_ycsb();

    // 4. 解析延迟结果
    pm.parse_latency_results();

    // 5. ProcessManager的析构函数会自动停止memcached

    return EXIT_SUCCESS;
}