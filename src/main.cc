/*
 * CXLMemSim main
 *
 *  By: Andrew Quinn
 *      Yiwei Yang
 *      Brian Zhao
 *  SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
 *  Copyright 2025 Regents of the University of California
 *  UC Santa Cruz Sluglab.
 */
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_OFF
#include "cxlendpoint.h"
#include "helper.h"
#include "monitor.h"
#include "policy.h"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <numeric>
#include <spdlog/cfg/env.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>
Helper helper{};
CXLController *controller;
Monitors *monitors;
auto cha_mapping = std::vector{0, 1, 2, 3, 4, 5, 6, 7, 8};

namespace {

struct CXLMemSimOptions {
    bool help = false;
    std::string target = "./microbench/malloc";
    std::vector<int> cpuset{0, 1, 2, 3};
    double dramlatency = 110.0;
    int pebsperiod = 10;
    std::string mode = "p";
    std::string topology = "(1,(2,3))";
    std::vector<int> capacity{0, 20, 20, 20};
    double frequency = 4000.0;
    std::vector<int> latency{200, 250, 200, 250, 200, 250};
    std::vector<int> bandwidth{50, 50, 50, 50, 50, 50};
    std::vector<double> mlc_bandwidth{};
    double bandwidth_knee = 0.80;
    uint64_t bandwidth_window_ns = 100000;
    std::vector<std::string> pmu_name{"prefetch",  "l2hit",        "l2miss",      "cpus_dram_rds",
                                      "llcl_hits", "snoop_fwd_wb", "total_stall", "l2stall"};
    std::vector<uint64_t> pmu_config1{0x02c0, 0x0134, 0x7e35, 0x01d3, 0x04d1, 0x01b7, 0x04004a3, 0x0449};
    std::vector<uint64_t> pmu_config2{0, 0, 0, 0, 0, 0x1a610008, 0, 0};
    std::vector<double> weight{88, 88, 88, 88, 88, 88, 88};
    std::vector<double> weight_vec{400, 800, 1200, 1600, 2000, 2400, 3000};
    std::vector<std::string> policy{"none", "none", "none", "none"};
    std::vector<std::string> env{"OMP_NUM_THREADS=24"};
};

std::string trim_copy(const std::string &value) {
    const auto begin = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) { return std::isspace(ch); });
    const auto end =
        std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) { return std::isspace(ch); }).base();
    return begin < end ? std::string(begin, end) : std::string();
}

std::vector<std::string> split_csv(const std::string &value) {
    std::vector<std::string> fields;
    std::istringstream stream(value);
    std::string field;
    while (std::getline(stream, field, ',')) {
        fields.push_back(trim_copy(field));
    }
    if (fields.empty() && !value.empty()) {
        fields.push_back(trim_copy(value));
    }
    return fields;
}

template <typename T> T parse_value(const std::string &value);

template <> int parse_value<int>(const std::string &value) { return std::stoi(value, nullptr, 0); }

template <> uint64_t parse_value<uint64_t>(const std::string &value) { return std::stoull(value, nullptr, 0); }

template <> double parse_value<double>(const std::string &value) { return std::stod(value); }

template <> std::string parse_value<std::string>(const std::string &value) { return value; }

template <typename T> std::vector<T> parse_csv_vector(const std::string &value) {
    std::vector<T> parsed;
    for (const auto &field : split_csv(value)) {
        if (!field.empty()) {
            parsed.push_back(parse_value<T>(field));
        }
    }
    return parsed;
}

bool option_value_follows(int argc, char *argv[], int index) { return index + 1 < argc && argv[index + 1][0] != '-'; }

std::string require_cli_value(int argc, char *argv[], int &index, const std::string &key,
                              const std::string &inline_value, bool has_inline_value) {
    if (has_inline_value) {
        return inline_value;
    }
    if (!option_value_follows(argc, argv, index)) {
        throw std::invalid_argument("Missing value for option " + key);
    }
    index++;
    return argv[index];
}

void print_main_help(const char *program) {
    std::cout << "Usage: " << program << " [options]\n\n"
              << "Options:\n"
              << "  -h, --help                     Show this help text\n"
              << "  -t, --target <command>         Target executable and arguments\n"
              << "  -c, --cpuset <list>            CPU list, for example 0,1,2,3\n"
              << "  -d, --dramlatency <ns>         Platform DRAM latency\n"
              << "  -p, -i, --pebsperiod <count>   PEBS sample period\n"
              << "  -m, --mode <mode>              page, cacheline, hugepage_2M, or hugepage_1G\n"
              << "  -o, --topology <tree>          Newick-style CXL topology\n"
              << "  -q, --capacity <list>          Local and expander capacity vector\n"
              << "  -f, --frequency <MHz>          CPU frequency used by the model\n"
              << "  -l, --latency <list>           Read/write latency vector\n"
              << "  -b, --bandwidth <list>         Read/write bandwidth vector\n"
              << "      --mlc-bandwidth <list>     MLC read,write,mixed GB/s and optional knee ratio\n"
              << "      --bandwidth-knee <ratio>   Utilization where bandwidth latency starts rising\n"
              << "      --bandwidth-window-ns <ns> Minimum bandwidth accounting window\n"
              << "  -x, --pmu_name <list>          PMU event names\n"
              << "  -y, --pmu_config1 <list>       PMU config values\n"
              << "  -z, --pmu_config2 <list>       PMU config1 values\n"
              << "  -w, --weight <list>            Linear-regression weights\n"
              << "  -v, --weight_vec <list>        Linear-regression weight vector\n"
              << "  -k, --policy <list>            allocation,migration,paging,caching policies\n"
              << "  -e, --env <list>               Environment entries for the target\n";
}

bool parse_main_options(int argc, char *argv[], CXLMemSimOptions &opts, std::string &error) {
    try {
        for (int i = 1; i < argc; i++) {
            std::string arg = argv[i];
            std::string key;
            std::string value;
            bool has_inline_value = false;

            if (arg.rfind("--", 0) == 0) {
                auto eq_pos = arg.find('=');
                key = arg.substr(2, eq_pos == std::string::npos ? std::string::npos : eq_pos - 2);
                if (eq_pos != std::string::npos) {
                    value = arg.substr(eq_pos + 1);
                    has_inline_value = true;
                }
            } else if (arg == "-h") {
                key = "help";
            } else if (arg == "-t") {
                key = "target";
            } else if (arg == "-c") {
                key = "cpuset";
            } else if (arg == "-d") {
                key = "dramlatency";
            } else if (arg == "-p" || arg == "-i") {
                key = "pebsperiod";
            } else if (arg == "-m") {
                key = "mode";
            } else if (arg == "-o") {
                key = "topology";
            } else if (arg == "-q") {
                key = "capacity";
            } else if (arg == "-f") {
                key = "frequency";
            } else if (arg == "-l") {
                key = "latency";
            } else if (arg == "-b") {
                key = "bandwidth";
            } else if (arg == "-x") {
                key = "pmu_name";
            } else if (arg == "-y") {
                key = "pmu_config1";
            } else if (arg == "-z") {
                key = "pmu_config2";
            } else if (arg == "-w") {
                key = "weight";
            } else if (arg == "-v") {
                key = "weight_vec";
            } else if (arg == "-k") {
                key = "policy";
            } else if (arg == "-e") {
                key = "env";
            } else {
                throw std::invalid_argument("Unknown option: " + arg);
            }

            auto get_value = [&](const std::string &option_name) {
                return require_cli_value(argc, argv, i, option_name, value, has_inline_value);
            };

            if (key == "help") {
                opts.help = true;
            } else if (key == "target") {
                opts.target = get_value(key);
            } else if (key == "cpuset") {
                opts.cpuset = parse_csv_vector<int>(get_value(key));
            } else if (key == "dramlatency") {
                opts.dramlatency = parse_value<double>(get_value(key));
            } else if (key == "pebsperiod") {
                opts.pebsperiod = parse_value<int>(get_value(key));
            } else if (key == "mode") {
                opts.mode = get_value(key);
            } else if (key == "topology") {
                opts.topology = get_value(key);
            } else if (key == "capacity") {
                opts.capacity = parse_csv_vector<int>(get_value(key));
            } else if (key == "frequency") {
                opts.frequency = parse_value<double>(get_value(key));
            } else if (key == "latency") {
                opts.latency = parse_csv_vector<int>(get_value(key));
            } else if (key == "bandwidth") {
                opts.bandwidth = parse_csv_vector<int>(get_value(key));
            } else if (key == "mlc-bandwidth") {
                opts.mlc_bandwidth = parse_csv_vector<double>(get_value(key));
                if (opts.mlc_bandwidth.size() >= 4) {
                    opts.bandwidth_knee = opts.mlc_bandwidth[3];
                }
            } else if (key == "bandwidth-knee") {
                opts.bandwidth_knee = parse_value<double>(get_value(key));
            } else if (key == "bandwidth-window-ns") {
                opts.bandwidth_window_ns = parse_value<uint64_t>(get_value(key));
            } else if (key == "pmu_name") {
                opts.pmu_name = parse_csv_vector<std::string>(get_value(key));
            } else if (key == "pmu_config1") {
                opts.pmu_config1 = parse_csv_vector<uint64_t>(get_value(key));
            } else if (key == "pmu_config2") {
                opts.pmu_config2 = parse_csv_vector<uint64_t>(get_value(key));
            } else if (key == "weight") {
                opts.weight = parse_csv_vector<double>(get_value(key));
            } else if (key == "weight_vec") {
                opts.weight_vec = parse_csv_vector<double>(get_value(key));
            } else if (key == "policy") {
                opts.policy = parse_csv_vector<std::string>(get_value(key));
            } else if (key == "env") {
                opts.env = parse_csv_vector<std::string>(get_value(key));
            } else {
                throw std::invalid_argument("Unknown option: --" + key);
            }
        }

        while (opts.policy.size() < 4) {
            opts.policy.emplace_back("none");
        }
        return true;
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
}

} // namespace

int main(int argc, char *argv[]) {
    spdlog::cfg::load_env_levels();

    CXLMemSimOptions opts;
    std::string parse_error;
    if (!parse_main_options(argc, argv, opts, parse_error)) {
        std::cerr << "Failed to parse options: " << parse_error << "\n\n";
        print_main_help(argv[0]);
        return 1;
    }
    if (opts.help) {
        print_main_help(argv[0]);
        return 0;
    }

    auto target = opts.target;
    auto cpuset = opts.cpuset;
    auto pebsperiod = opts.pebsperiod;
    auto latency = opts.latency;
    auto bandwidth = opts.bandwidth;
    auto mlc_bandwidth = opts.mlc_bandwidth;
    auto bandwidth_knee = opts.bandwidth_knee;
    auto bandwidth_window_ns = opts.bandwidth_window_ns;
    [[maybe_unused]] auto frequency = opts.frequency;
    auto topology = opts.topology;
    auto capacity = opts.capacity;
    auto dramlatency = opts.dramlatency;
    auto pmu_name = opts.pmu_name;
    auto pmu_config1 = opts.pmu_config1;
    auto pmu_config2 = opts.pmu_config2;
    auto weight = opts.weight;
    auto weight_vec = opts.weight_vec;
    auto page_ = opts.mode;
    auto policy = opts.policy;
    auto env = opts.env;

    page_type mode;
    if (page_ == "hugepage_2M") {
        mode = HUGEPAGE_2M;
    } else if (page_ == "hugepage_1G") {
        mode = HUGEPAGE_1G;
    } else if (page_ == "cacheline") {
        mode = CACHELINE;
    } else {
        mode = PAGE;
    }
    AllocationPolicy *policy1;
    MigrationPolicy *policy2;
    PagingPolicy *policy3;
    CachingPolicy *policy4;

    // Initialize allocation policy
    if (policy[0] == "interleave") {
        policy1 = new InterleavePolicy();
    } else if (policy[0] == "numa") {
        policy1 = new NUMAPolicy();
    } else {
        policy1 = new AllocationPolicy();
    }

    // Initialize migration policy
    if (policy[1] == "heataware") {
        policy2 = new HeatAwareMigrationPolicy();
    } else if (policy[1] == "frequency") {
        policy2 = new FrequencyBasedMigrationPolicy();
    } else if (policy[1] == "loadbalance") {
        policy2 = new LoadBalancingMigrationPolicy();
    } else if (policy[1] == "locality") {
        policy2 = new LocalityBasedMigrationPolicy();
    } else if (policy[1] == "lifetime") {
        policy2 = new LifetimeBasedMigrationPolicy();
    } else if (policy[1] == "hybrid") {
        auto *hybridPolicy = new HybridMigrationPolicy();
        // Can add multiple policies to the hybrid policy
        hybridPolicy->add_policy(new HeatAwareMigrationPolicy());
        hybridPolicy->add_policy(new FrequencyBasedMigrationPolicy());
        policy2 = hybridPolicy;
    } else {
        SPDLOG_ERROR("Unknown migration policy: {}", policy[1]);
        policy2 = new MigrationPolicy();
    }

    // Initialize paging policy
    if (policy[2] == "hugepage") {
        policy3 = new HugePagePolicy();
    } else if (policy[2] == "pagetableaware") {
        policy3 = new PageTableAwarePolicy();
    } else {
        SPDLOG_ERROR("Unknown paging policy: {}", policy[2]);
        policy3 = new PagingPolicy();
    }

    // Initialize caching policy
    if (policy[3] == "fifo") {
        policy4 = new FIFOPolicy();
    } else if (policy[3] == "frequency") {
        policy4 = new FrequencyBasedInvalidationPolicy();
    } else {
        SPDLOG_ERROR("Unknown caching policy: {}", policy[3]);
        policy4 = new CachingPolicy();
    }

    uint64_t use_cpus = 0;
    cpu_set_t use_cpuset;
    CPU_ZERO(&use_cpuset);
    for (auto i : cpuset) {
        if (!use_cpus || use_cpus & 1UL << i) {
            CPU_SET(i, &use_cpuset);
            std::cout << "use cpuid: " << i << " " << use_cpus << std::endl;
        }
    }

    auto tnum = CPU_COUNT(&use_cpuset);
    auto cur_processes = 0;
    auto ncpu = helper.num_of_cpu();
    [[maybe_unused]] auto ncha = helper.num_of_cha();
    SPDLOG_DEBUG("tnum:{}", tnum);
    for (size_t idx = 0; idx < weight.size(); idx++) {
        SPDLOG_DEBUG("weight[{}]:{}", weight_vec[idx], weight[idx]);
    }

    for (size_t idx = 0; idx < capacity.size(); idx++) {
        if (idx == 0) {
            SPDLOG_DEBUG("local_memory_region capacity:{}", capacity[idx]);
            controller = new CXLController({policy1, policy2, policy3, policy4}, capacity[0], mode, 100, dramlatency);
        } else {
            SPDLOG_DEBUG("memory_region:{}", (idx - 1) + 1);
            SPDLOG_DEBUG(" capacity:{}", capacity[(idx - 1) + 1]);
            SPDLOG_DEBUG(" read_latency:{}", latency[(idx - 1) * 2]);
            SPDLOG_DEBUG(" write_latency:{}", latency[(idx - 1) * 2 + 1]);
            SPDLOG_DEBUG(" read_bandwidth:{}", bandwidth[(idx - 1) * 2]);
            SPDLOG_DEBUG(" write_bandwidth:{}", bandwidth[(idx - 1) * 2 + 1]);
            auto *ep = new CXLMemExpander(bandwidth[(idx - 1) * 2], bandwidth[(idx - 1) * 2 + 1],
                                          latency[(idx - 1) * 2], latency[(idx - 1) * 2 + 1], idx - 1, capacity[idx]);
            BandwidthModelConfig bw_model;
            bw_model.read_peak_gbps =
                mlc_bandwidth.size() >= 1 ? mlc_bandwidth[0] : static_cast<double>(bandwidth[(idx - 1) * 2]);
            bw_model.write_peak_gbps =
                mlc_bandwidth.size() >= 2 ? mlc_bandwidth[1] : static_cast<double>(bandwidth[(idx - 1) * 2 + 1]);
            bw_model.mixed_peak_gbps = mlc_bandwidth.size() >= 3 ? mlc_bandwidth[2] : 0.0;
            bw_model.knee_utilization = bandwidth_knee;
            bw_model.min_window_ns = bandwidth_window_ns;
            bw_model.calibrated_from_mlc = !mlc_bandwidth.empty();
            ep->configure_bandwidth_model(bw_model);
            controller->insert_end_point(ep);
        }
    }
    controller->construct_topo(topology);

    BandwidthModelConfig fabric_bw_model;
    fabric_bw_model.read_peak_gbps =
        mlc_bandwidth.size() >= 1 ? mlc_bandwidth[0] : static_cast<double>(bandwidth.empty() ? 50 : bandwidth[0]);
    fabric_bw_model.write_peak_gbps =
        mlc_bandwidth.size() >= 2 ? mlc_bandwidth[1] : static_cast<double>(bandwidth.size() >= 2 ? bandwidth[1] : 50);
    fabric_bw_model.mixed_peak_gbps = mlc_bandwidth.size() >= 3 ? mlc_bandwidth[2] : 0.0;
    fabric_bw_model.knee_utilization = bandwidth_knee;
    fabric_bw_model.min_window_ns = bandwidth_window_ns;
    fabric_bw_model.calibrated_from_mlc = !mlc_bandwidth.empty();
    controller->configure_bandwidth_model(fabric_bw_model);

    /** Hove been got by socket if it's not main thread and synchro */
    SPDLOG_DEBUG("cpu_freq:{}", frequency);
    SPDLOG_DEBUG("num_of_cha:{}", ncha);
    SPDLOG_DEBUG("num_of_cpu:{}", ncpu);
    for (auto cpu_id : cpuset) {
        helper.used_cpu.push_back(cpu_id);
        helper.used_cha.push_back(cpu_id);
    }
    monitors = new Monitors{tnum, &use_cpuset};

    /** Reinterpret the input for the argv argc */
    char cmd_buf[1024] = {0};
    strncpy(cmd_buf, target.c_str(), sizeof(cmd_buf));
    char *strtok_state = nullptr;
    char *filename = strtok_r(cmd_buf, " ", &strtok_state);
    char *args[32] = {nullptr};
    args[0] = filename;
    size_t current_arg_idx;
    for (current_arg_idx = 1; current_arg_idx < 32; ++current_arg_idx) {
        char *current_arg = strtok_r(nullptr, " ", &strtok_state);
        if (current_arg == nullptr) {
            break;
        }
        args[current_arg_idx] = current_arg;
        SPDLOG_INFO("args[{}] = {}", current_arg_idx, args[current_arg_idx]);
    }

    /** Create target process */
    Helper::detach_children();
    auto t_process = fork();
    if (t_process < 0) {
        SPDLOG_ERROR("Fork: failed to create target process");
        exit(1);
    }
    if (t_process == 0) {
        sleep(1);
        std::vector<const char *> envp;
        envp.emplace_back("OMP_NUM_THREADS=4");
        while (!env.empty()) {
            envp.emplace_back(env.back().c_str());
            env.pop_back();
        }
        envp.emplace_back(nullptr);
        execve(filename, args, const_cast<char *const *>(envp.data()));
        SPDLOG_ERROR("Exec: failed to create target process");
        exit(1);
    }
    /** In case of process, use SIGSTOP. */
    if (auto res = monitors->enable(t_process, t_process, true, pebsperiod, tnum); res < 0) {
        SPDLOG_ERROR("Failed to enable monitor for pid {} (code {})", t_process, res);
        kill(t_process, SIGTERM);
        return 1;
    }
    cur_processes++;
    SPDLOG_DEBUG("pid of CXLMemSim = {}, cur process={}", t_process, cur_processes);

    if (cur_processes >= ncpu) {
        SPDLOG_ERROR("Failed to execute. The number of processes/threads of the target application is more than "
                     "physical CPU cores.");
        exit(0);
    }

    /** Wait all the target processes until emulation process initialized. */
    monitors->stop_all(cur_processes);

    /** Get CPU information */
    if (!get_cpu_info(&monitors->mon[0].before->cpuinfo)) {
        SPDLOG_WARN("Failed to obtain CPU information. PMU model detection will use the generic fallback.");
    }
    auto perf_config =
        helper.detect_model(monitors->mon[0].before->cpuinfo.cpu_model, pmu_name, pmu_config1, pmu_config2);
    PMUInfo pmu{t_process, &helper, &perf_config};

    SPDLOG_DEBUG("The target process starts running.");
    SPDLOG_TRACE("{}", *monitors);
    monitors->print_flag = false;

    /* read CHA params */
    for (auto &mon : monitors->mon) {
        for (size_t idx = 0; idx < pmu.chas.size(); idx++) {
            pmu.chas[idx].read_cha_elems(&mon.before->chas[idx]);
        }
        for (size_t idx = 0; idx < pmu.cpus.size(); idx++) {
            pmu.cpus[idx].read_cpu_elems(&mon.before->cpus[idx]);
        }
    }

    uint32_t diff_nsec = 0;
    timespec start_ts{}, end_ts{};

    /** Wait all the target processes until emulation process initialized. */
    monitors->run_all(cur_processes);
    for (int i = 0; i < cur_processes; i++) {
        clock_gettime(CLOCK_MONOTONIC, &monitors->mon[i].start_exec_ts);
    }

    while (true) {
        uint64_t calibrated_delay;
        for (size_t i = 0; i < monitors->mon.size(); i++) {
            auto &mon = monitors->mon[i];
            // check other process
            auto m_status = mon.status.load();
            if (m_status == MONITOR_DISABLE) {
                continue;
            }
            if (m_status == MONITOR_ON || m_status == MONITOR_SUSPEND) {
                clock_gettime(CLOCK_MONOTONIC, &start_ts);
                SPDLOG_DEBUG("[{}:{}:{}] start_ts: {}.{}", i, mon.tgid, mon.tid, start_ts.tv_sec, start_ts.tv_nsec);
                /** Read CHA values */
                std::vector<uint64_t> cha_vec{0, 0, 0, 0}, cpu_vec{0, 0, 0, 0};

                /*** read CPU params */
                uint64_t wb_cnt = 0, target_l2stall = 0, target_llcmiss = 0, target_llchits = 0, all_llcmiss = 0,
                         all_prefetch = 0;
                [[maybe_unused]] uint64_t target_l2miss = 0;
                double writeback_latency;
                /* read PEBS and LBR samples */
#if CXLMEMSIM_HAS_LINUX_PERF
                if (mon.is_process) {
                    /* read PEBS sample */
                    if (mon.pebs_ctx && mon.pebs_ctx->read(controller, &mon.after->pebs) < 0) {
                        SPDLOG_ERROR("[{}:{}:{}] Warning: Failed PEBS read", i, mon.tgid, mon.tid);
                    }
                    /* read LBR sample */
                    if (mon.lbr_ctx && mon.lbr_ctx->read(controller, &mon.after->lbr) < 0) {
                        SPDLOG_ERROR("[{}:{}:{}] Warning: Failed LBR read", i, mon.tgid, mon.tid);
                    }
                }
#endif
                target_llcmiss = mon.after->pebs.total - mon.before->pebs.total;

                for (size_t idx = 0; idx < pmu.cpus.size(); idx++) {
                    pmu.cpus[idx].read_cpu_elems(&mon.after->cpus[i]);
                    cpu_vec[idx] = mon.after->cpus[i].cpu[idx] - mon.before->cpus[i].cpu[idx];
                }

                for (size_t idx = 0; idx < pmu.chas.size(); idx++) {
                    pmu.chas[idx].read_cha_elems(&mon.after->chas[cha_mapping[i]]);
                    cha_vec[idx] = mon.after->chas[cha_mapping[i]].cha[idx] - mon.before->chas[cha_mapping[i]].cha[idx];
                }
                target_llchits = cpu_vec[0];
                wb_cnt = cpu_vec[1];
                target_l2stall = cpu_vec[3];
                target_l2miss = cha_vec[2];
                for (const auto &mon : monitors->mon) {
                    all_llcmiss += mon.after->pebs.total - mon.before->pebs.total;
                    all_prefetch += mon.after->chas[cha_mapping[i]].cha[0] - mon.before->chas[cha_mapping[i]].cha[0];
                }
                auto avg_weight = std::accumulate(weight.begin(), weight.end(), 0.0) / weight.size();
                // LSU
                writeback_latency = (double)target_l2stall * avg_weight *
                                    (wb_cnt * target_llcmiss / (all_llcmiss + all_prefetch + 1) /
                                     (target_llchits + avg_weight * target_llcmiss + 1));
                uint64_t emul_delay =
                    std::min(controller->latency_lat + controller->bandwidth_lat + writeback_latency, 1.) * 1000000;

                SPDLOG_DEBUG("[{}:{}:{}] pebs: total={}, ", i, mon.tgid, mon.tid, mon.after->pebs.total);

                mon.before->pebs.total = mon.after->pebs.total;
                mon.before->lbr.total = mon.after->lbr.total;

                SPDLOG_DEBUG("delay={}", emul_delay);

                /* compensation of delay END(1) */
                clock_gettime(CLOCK_MONOTONIC, &end_ts);
                diff_nsec += (end_ts.tv_sec - start_ts.tv_sec) * 1000000000 + (end_ts.tv_nsec - start_ts.tv_nsec);
                SPDLOG_DEBUG("diff_nsec:{}", diff_nsec);

                calibrated_delay = diff_nsec > emul_delay ? 0 : emul_delay - diff_nsec;
                mon.total_delay += (double)calibrated_delay / 1000000000;
                diff_nsec = 0;

                /* insert emulated NVM latency */
                mon.injected_delay.tv_sec += std::lround(calibrated_delay / 1000000000);
                mon.injected_delay.tv_nsec += std::lround(calibrated_delay % 1000000000);
                SPDLOG_DEBUG("[{}:{}:{}]delay:{} , total delay:{}", i, mon.tgid, mon.tid, calibrated_delay,
                             mon.total_delay);

                {
                    std::lock_guard lock(mon.wanted_delay_mutex);
                    timespec new_wanted = mon.wanted_delay;
                    new_wanted.tv_nsec += emul_delay;
                    new_wanted.tv_sec += new_wanted.tv_nsec / 1000000000;
                    new_wanted.tv_nsec = new_wanted.tv_nsec % 1000000000;
                    mon.wanted_delay = new_wanted;
                    SPDLOG_DEBUG("{}:{}", new_wanted.tv_sec, new_wanted.tv_nsec);
                    SPDLOG_DEBUG("{}", *monitors);
                }
                controller->latency_lat = 0;
                controller->bandwidth_lat = 0;
            }

        } // End for-loop for all target processes

        if (monitors->check_all_terminated(tnum)) {
            break;
        }
    } // End while-loop for emulation

    return 0;
}
