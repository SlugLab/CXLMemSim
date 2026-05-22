/*
 * CXLMemSim monitor
 *
 *  By: Andrew Quinn
 *      Yiwei Yang
 *      Brian Zhao
 *  SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
 *  Copyright 2025 Regents of the University of California
 *  UC Santa Cruz Sluglab.
 */

#include "monitor.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string_view>
#if CXLMEMSIM_HAS_LINUX_PERF
#include <sys/syscall.h>
#endif
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>
timespec Monitor::last_delay = {0, 0};

#if CXLMEMSIM_HAS_LINUX_PERF
std::vector<pid_t> get_thread_ids(pid_t pid) {
    std::vector<pid_t> thread_ids;

    // task
    std::string task_dir = "/proc/" + std::to_string(pid) + "/task";

    DIR *dir = opendir(task_dir.c_str());
    if (dir == nullptr) {
        std::cerr << "Could not open the folder: " << task_dir << " - " << strerror(errno) << std::endl;
        return thread_ids;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        //  .  ..
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        // ID
        pid_t tid = std::stoi(entry->d_name);
        thread_ids.push_back(tid);
    }

    closedir(dir);
    return thread_ids;
}

// CPU
bool set_thread_affinity(pid_t tid, int cpu_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_id, &cpuset);

    int result = sched_setaffinity(tid, sizeof(cpu_set_t), &cpuset);

    if (result != 0) {
        std::cerr << " " << tid << " CPU: " << strerror(errno) << std::endl;
        return false;
    }

    return true;
}
#endif

#if !CXLMEMSIM_HAS_LINUX_PERF
namespace {

#if defined(__APPLE__)
struct XctraceSummary {
    uint64_t sample_count{};
    uint64_t miss_sample_count{};
    uint64_t backtrace_sample_count{};
};

std::filesystem::path make_xctrace_output_path(pid_t pid) {
    const char *dir_env = std::getenv("CXLMEMSIM_XCTRACE_DIR");
    std::filesystem::path output_dir = dir_env && dir_env[0] != '\0' ? dir_env : "xctrace";
    std::filesystem::create_directories(output_dir);

    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto stamp = std::chrono::duration_cast<std::chrono::seconds>(now).count();
    return output_dir / std::format("cxlmemsim_legacy_{}_{}.trace", pid, stamp);
}

std::string xctrace_template_name() {
    const char *template_env = std::getenv("CXLMEMSIM_XCTRACE_TEMPLATE");
    return template_env && template_env[0] != '\0' ? template_env : "CPU Counters";
}

std::chrono::milliseconds xctrace_startup_delay() {
    const char *startup_env = std::getenv("CXLMEMSIM_XCTRACE_STARTUP_MS");
    if (startup_env && startup_env[0] != '\0') {
        return std::chrono::milliseconds(std::max(0, std::atoi(startup_env)));
    }
    return std::chrono::milliseconds(1500);
}

std::chrono::milliseconds xctrace_stop_grace() {
    const char *stop_env = std::getenv("CXLMEMSIM_XCTRACE_STOP_MS");
    if (stop_env && stop_env[0] != '\0') {
        return std::chrono::milliseconds(std::max(0, std::atoi(stop_env)));
    }
    return std::chrono::milliseconds(5000);
}

bool xctrace_debug_enabled() {
    const char *debug_env = std::getenv("CXLMEMSIM_XCTRACE_DEBUG");
    return debug_env && std::strcmp(debug_env, "1") == 0;
}

std::optional<std::filesystem::path> find_xctrace() {
    const char *xctrace_env = std::getenv("CXLMEMSIM_XCTRACE");
    if (xctrace_env && xctrace_env[0] != '\0' && access(xctrace_env, X_OK) == 0) {
        return std::filesystem::path{xctrace_env};
    }

    for (const auto &candidate : {"/Applications/Xcode.app/Contents/Developer/usr/bin/xctrace", "/usr/bin/xctrace"}) {
        if (access(candidate, X_OK) == 0) {
            return std::filesystem::path{candidate};
        }
    }

    return std::nullopt;
}

pid_t start_xctrace_recording(pid_t target_pid, std::string *output_path) {
    const char *disable_env = std::getenv("CXLMEMSIM_XCTRACE_DISABLE");
    if (disable_env && std::strcmp(disable_env, "1") == 0) {
        return -1;
    }

    auto xctrace_path = find_xctrace();
    if (!xctrace_path) {
        std::cerr << "xctrace is unavailable; install Xcode or set CXLMEMSIM_XCTRACE to an xctrace path.\n";
        return -1;
    }

    const auto output = make_xctrace_output_path(target_pid);
    const auto output_string = output.string();
    const auto target_pid_string = std::to_string(target_pid);
    const auto template_name = xctrace_template_name();

    pid_t recorder_pid = fork();
    if (recorder_pid < 0) {
        std::cerr << "Failed to fork xctrace recorder: " << std::strerror(errno) << "\n";
        return -1;
    }

    if (recorder_pid == 0) {
        execl(xctrace_path->c_str(), "xctrace", "record", "--quiet", "--template", template_name.c_str(), "--attach",
              target_pid_string.c_str(), "--output", output_string.c_str(), "--no-prompt", nullptr);
        std::_Exit(127);
    }

    if (output_path) {
        *output_path = output_string;
    }
    std::cout << "xctrace recording: " << output_string << " (template: " << template_name << ")\n";
    return recorder_pid;
}

bool reap_child_if_done(pid_t child_pid) {
    int status = 0;
    const pid_t result = waitpid(child_pid, &status, WNOHANG);
    return result == child_pid || (result < 0 && errno == ECHILD);
}

void wait_for_child_exit(pid_t child_pid) {
    int status = 0;
    while (waitpid(child_pid, &status, 0) < 0) {
        if (errno == EINTR) {
            continue;
        }
        return;
    }
}

void stop_xctrace_recording(pid_t recorder_pid) {
    if (recorder_pid <= 0) {
        return;
    }
    if (reap_child_if_done(recorder_pid)) {
        return;
    }

    const auto deadline = std::chrono::steady_clock::now() + xctrace_stop_grace();
    while (std::chrono::steady_clock::now() < deadline) {
        if (reap_child_if_done(recorder_pid)) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    kill(recorder_pid, SIGINT);
    wait_for_child_exit(recorder_pid);
}

std::filesystem::path make_xctrace_export_path(const std::filesystem::path &trace_path, std::string_view schema) {
    auto export_path = trace_path;
    export_path += ".";
    export_path += schema;
    export_path += ".xml";
    return export_path;
}

bool wait_for_child(pid_t child_pid) {
    int status = 0;
    while (waitpid(child_pid, &status, 0) < 0) {
        if (errno == EINTR) {
            continue;
        }
        return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool export_xctrace_table(const std::string &trace_path, std::string_view schema, std::filesystem::path *output_path) {
    auto xctrace_path = find_xctrace();
    if (!xctrace_path || trace_path.empty()) {
        return false;
    }

    const auto output = make_xctrace_export_path(trace_path, schema);
    const std::string xpath = std::format("/trace-toc/run[@number=\"1\"]/data/table[@schema=\"{}\"]", schema);
    const auto output_string = output.string();

    pid_t export_pid = fork();
    if (export_pid < 0) {
        return false;
    }

    if (export_pid == 0) {
        auto *dev_null = std::freopen("/dev/null", "w", stdout);
        (void)dev_null;
        dev_null = std::freopen("/dev/null", "w", stderr);
        (void)dev_null;
        execl(xctrace_path->c_str(), "xctrace", "export", "--input", trace_path.c_str(), "--xpath", xpath.c_str(),
              "--output", output_string.c_str(), nullptr);
        std::_Exit(127);
    }

    const bool child_status_ok = wait_for_child(export_pid);
    const bool output_ok = std::filesystem::exists(output) && std::filesystem::file_size(output) > 0;
    if (!child_status_ok && xctrace_debug_enabled()) {
        SPDLOG_INFO("xctrace export for schema {} returned a non-zero child status", schema);
    }
    if (!output_ok) {
        return false;
    }

    if (output_path) {
        *output_path = output;
    }
    return true;
}

std::string read_text_file(const std::filesystem::path &path) {
    std::ifstream input(path);
    if (!input) {
        return {};
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

uint64_t count_occurrences(std::string_view text, std::string_view needle) {
    if (needle.empty()) {
        return 0;
    }

    uint64_t count = 0;
    size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string_view::npos) {
        count++;
        pos += needle.size();
    }
    return count;
}

std::optional<std::string> find_metric_string_id(std::string_view xml, std::string_view metric_name) {
    const std::string needle = std::format("fmt=\"{}\"", metric_name);
    const size_t fmt_pos = xml.find(needle);
    if (fmt_pos == std::string_view::npos) {
        return std::nullopt;
    }

    const size_t tag_start = xml.rfind("<string ", fmt_pos);
    const size_t tag_end = xml.find('>', fmt_pos);
    if (tag_start == std::string_view::npos || tag_end == std::string_view::npos || tag_start > tag_end) {
        return std::nullopt;
    }

    const std::string_view tag = xml.substr(tag_start, tag_end - tag_start);
    constexpr std::string_view id_marker = "id=\"";
    const size_t id_pos = tag.find(id_marker);
    if (id_pos == std::string_view::npos) {
        return std::nullopt;
    }

    const size_t id_start = id_pos + id_marker.size();
    const size_t id_end = tag.find('"', id_start);
    if (id_end == std::string_view::npos || id_end == id_start) {
        return std::nullopt;
    }

    return std::string(tag.substr(id_start, id_end - id_start));
}

XctraceSummary parse_counting_mode_samples(const std::string &xml) {
    XctraceSummary summary{};
    size_t pos = 0;
    while ((pos = xml.find("<row", pos)) != std::string::npos) {
        const size_t row_end = xml.find("</row>", pos);
        if (row_end == std::string::npos) {
            break;
        }

        const std::string row = xml.substr(pos, row_end - pos);
        const std::string lower_row = lower_copy(row);
        summary.sample_count++;
        if (lower_row.find("<tagged-backtrace") != std::string::npos) {
            summary.backtrace_sample_count++;
        }
        if (lower_row.find("miss") != std::string::npos || lower_row.find("cache") != std::string::npos ||
            lower_row.find("l1d") != std::string::npos) {
            summary.miss_sample_count++;
        }

        pos = row_end + std::string_view("</row>").size();
    }
    return summary;
}

uint64_t parse_metric_table_cycle_rows(const std::string &xml) {
    const std::string lower_xml = lower_copy(xml);
    const std::optional<std::string> cycle_metric_id = find_metric_string_id(lower_xml, "cycle");
    const std::string cycle_ref =
        cycle_metric_id ? std::format("<string ref=\"{}\"/>", *cycle_metric_id) : std::string{};

    uint64_t cycle_rows = 0;
    size_t pos = 0;
    while ((pos = lower_xml.find("<row", pos)) != std::string::npos) {
        const size_t row_end = lower_xml.find("</row>", pos);
        if (row_end == std::string::npos) {
            break;
        }

        const std::string_view row(lower_xml.data() + pos, row_end - pos);
        const bool is_cycle_metric = row.find("fmt=\"cycle\"") != std::string_view::npos ||
                                     row.find(">cycle</string>") != std::string_view::npos ||
                                     (!cycle_ref.empty() && row.find(cycle_ref) != std::string_view::npos);
        if (is_cycle_metric && row.find("<sentinel") != std::string_view::npos) {
            cycle_rows++;
        }
        pos = row_end + std::string_view("</row>").size();
    }

    if (cycle_rows == 0) {
        cycle_rows = count_occurrences(lower_xml, "<sentinel");
    }
    return cycle_rows;
}

XctraceSummary summarize_xctrace(const std::string &trace_path) {
    XctraceSummary summary{};

    std::filesystem::path samples_xml_path;
    if (export_xctrace_table(trace_path, "CountingModeSamples", &samples_xml_path)) {
        const std::string samples_xml = read_text_file(samples_xml_path);
        summary = parse_counting_mode_samples(samples_xml);
        if (xctrace_debug_enabled()) {
            SPDLOG_INFO("xctrace CountingModeSamples export {} bytes -> samples={}, misses={}, backtraces={}",
                        samples_xml.size(), summary.sample_count, summary.miss_sample_count,
                        summary.backtrace_sample_count);
        }
        if (!std::getenv("CXLMEMSIM_XCTRACE_KEEP_XML")) {
            std::filesystem::remove(samples_xml_path);
        }
    }

    if (summary.sample_count == 0) {
        std::filesystem::path metrics_xml_path;
        if (export_xctrace_table(trace_path, "MetricTable", &metrics_xml_path)) {
            const std::string metrics_xml = read_text_file(metrics_xml_path);
            summary.sample_count = parse_metric_table_cycle_rows(metrics_xml);
            summary.backtrace_sample_count = summary.sample_count;
            if (xctrace_debug_enabled()) {
                SPDLOG_INFO("xctrace MetricTable export {} bytes -> cycle rows={}", metrics_xml.size(),
                            summary.sample_count);
            }
            if (!std::getenv("CXLMEMSIM_XCTRACE_KEEP_XML")) {
                std::filesystem::remove(metrics_xml_path);
            }
        }
    }

    return summary;
}

void apply_xctrace_summary(Monitor &mon) {
    if (mon.xctrace_output_path.empty()) {
        return;
    }

    const XctraceSummary summary = summarize_xctrace(mon.xctrace_output_path);
    if (summary.sample_count == 0 && summary.backtrace_sample_count == 0 && summary.miss_sample_count == 0) {
        SPDLOG_WARN("xctrace export did not produce macOS counter samples for {}", mon.xctrace_output_path);
        return;
    }

    mon.before->pebs.total = summary.sample_count;
    mon.after->pebs.total = summary.sample_count;
    mon.before->pebs.llcmiss = summary.miss_sample_count;
    mon.after->pebs.llcmiss = summary.miss_sample_count;
    mon.before->lbr.total = summary.backtrace_sample_count;
    mon.after->lbr.total = summary.backtrace_sample_count;
}
#endif

bool process_is_running(pid_t pid) {
    if (pid <= 0) {
        return false;
    }
    if (kill(pid, 0) == 0) {
        return true;
    }
    return errno != ESRCH;
}

} // namespace
#endif

Monitors::Monitors(int cpu_count, cpu_set_t *use_cpuset) : print_flag(true) {
    mon = std::vector<Monitor>(cpu_count);
    /** Init mon */
    for (int i = 0; i < cpu_count; i++) {
        disable(i);

        // iCPU
        int available_cpu = -1;
        int count = 0;

        for (int cpuid = 0; cpuid < helper.num_of_cpu(); cpuid++) {
            if (!CPU_ISSET(cpuid, use_cpuset)) {
                if (count == i) {
                    available_cpu = cpuid;
                    break;
                }
                count++;
            }
        }

        if (available_cpu != -1) {
            mon[i].cpu_core = available_cpu;
        } else {
            std::cout << "No available CPU" << std::endl;
        }
    }
}
void Monitors::stop_all(const int processes) {
    for (auto i = 0; i < processes; ++i) {
        if (mon[i].status == MONITOR_ON) {
            mon[i].stop();
        }
    }
}
void Monitors::run_all(const int processes) {
    for (auto i = 0; i < processes; ++i) {
        if (mon[i].status == MONITOR_OFF) {
            mon[i].run();
        }
    }
}
Monitor *Monitors::get_mon(const int tgid, const int tid) {
    for (auto &i : mon) {
        if (i.tgid == tgid && i.tid == tid) {
            return &i;
        }
    }
    return new Monitor();
}
int Monitors::enable(uint32_t tgid, uint32_t tid, bool is_process, uint64_t pebs_sample_period, int32_t tnum) {
    int target = -1;

    for (int i = 0; i < tnum; i++) {
        if (mon[i].tgid == tgid && mon[i].tid == tid) {
            SPDLOG_DEBUG("already exists");
            return -1;
        }
    }
    for (int i = 0; i < tnum; i++) {
        if (mon[i].status != MONITOR_DISABLE) {
            continue;
        }
        target = i;
        break;
    }
    if (target == -1) {
        SPDLOG_DEBUG("All cores are used");
        return -1;
    }

    /* set CPU affinity to not used core. */
#if CXLMEMSIM_HAS_LINUX_PERF
    int s;
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(mon[target].cpu_core, &cpuset);
    s = sched_setaffinity(tid, sizeof(cpu_set_t), &cpuset);
    if (!is_process)
        if (s != 0) {
            if (errno == ESRCH) {
                if (tid != tgid) {
                    static auto thread_ids = get_thread_ids(tgid);
                    tid = thread_ids.back();
                    if (tid) {
                        thread_ids.pop_back();
                        std::cout << "set affinity for thread " << tid << std::endl;
                        s = sched_setaffinity(tid, sizeof(cpu_set_t), &cpuset);
                        if (s != 0) {
                            std::cout << "Failed to setaffinity for thread " << tid << std::endl;
                            return -2;
                        }
                    }
                } else {
                    return -2;
                }
            } else {
                std::cout << "Failed to setaffinity" << std::endl;
            }
        }
#else
    (void)pebs_sample_period;
#endif

    /* init */
    disable(target);
    mon[target].status = MONITOR_ON;
    mon[target].tgid = tgid;
    mon[target].tid = tid; // We can setup the process here
    mon[target].is_process = is_process;

    if (pebs_sample_period) {
#if CXLMEMSIM_HAS_LINUX_PERF
        /* pebs start */
        std::unique_ptr<PEBS> pebs_ctx;
        std::unique_ptr<LBR> lbr_ctx;
        try {
            pebs_ctx = std::make_unique<PEBS>(tgid, pebs_sample_period);
        } catch (const std::exception &e) {
            SPDLOG_ERROR("Failed to initialize PEBS for tgid={}, tid={}: {}", tgid, tid, e.what());
            disable(target);
            return -3;
        }
        SPDLOG_DEBUG("{}Process [tgid={}, tid={}]: enable to pebs.", target, mon[target].tgid, mon[target].tid);

        try {
            lbr_ctx = std::make_unique<LBR>(tgid, 1000);
        } catch (const std::exception &e) {
            SPDLOG_WARN("LBR is unavailable for tgid={}, tid={}: {}; continuing with PEBS samples only.", tgid, tid,
                        e.what());
        }

        mon[target].pebs_ctx = pebs_ctx.release();
        mon[target].lbr_ctx = lbr_ctx.release();
        new std::jthread(mon[target].wait, &mon, target);
#else
#if defined(__APPLE__)
        mon[target].stop();
        mon[target].xctrace_pid = start_xctrace_recording(tgid, &mon[target].xctrace_output_path);
        if (mon[target].xctrace_pid > 0) {
            std::this_thread::sleep_for(xctrace_startup_delay());
        }
#else
        SPDLOG_WARN("PEBS/LBR sampling is unavailable on this platform; legacy will run with zero PMU samples.");
#endif
#endif
    }
    SPDLOG_INFO("pid {}[tgid={}, tid={}] monitoring start", target, mon[target].tgid, mon[target].tid);

    return target;
}
void Monitors::disable(const uint32_t target) {
    mon[target].is_process = false; // Here to add the multi process.
    mon[target].status = MONITOR_DISABLE;
    mon[target].tgid = 0;
    mon[target].tid = 0;
    mon[target].before = &mon[target].elem[0];
    mon[target].after = &mon[target].elem[1];
    mon[target].total_delay = 0;
    mon[target].injected_delay.tv_sec = 0;
    mon[target].injected_delay.tv_nsec = 0;
    mon[target].end_exec_ts.tv_sec = 0;
    mon[target].end_exec_ts.tv_nsec = 0;
    if (mon[target].pebs_ctx != nullptr) {
#if CXLMEMSIM_HAS_LINUX_PERF
        delete mon[target].pebs_ctx;
#endif
        mon[target].pebs_ctx = nullptr;
    }
    if (mon[target].lbr_ctx != nullptr) {
#if CXLMEMSIM_HAS_LINUX_PERF
        delete mon[target].lbr_ctx;
#endif
        mon[target].lbr_ctx = nullptr;
    }
#if defined(__APPLE__) && !CXLMEMSIM_HAS_LINUX_PERF
    stop_xctrace_recording(mon[target].xctrace_pid);
    mon[target].xctrace_pid = -1;
    mon[target].xctrace_output_path.clear();
#endif
    for (auto &j : mon[target].elem) {
        j.pebs.total = 0;
        j.pebs.llcmiss = 0;
        j.lbr.total = 0;
        j.lbr.tid = 0;
        j.lbr.time = 0;
    }
}
bool Monitors::check_all_terminated(const uint32_t processes) {
    bool allTerminated = true;

    for (uint32_t i = 0; i < processes; ++i) {
        // Atomic load
        auto st = mon[i].status.load();

        if (st == MONITOR_ON || st == MONITOR_OFF) {
#if !CXLMEMSIM_HAS_LINUX_PERF
            if (!process_is_running(mon[i].tid)) {
                mon[i].status = MONITOR_TERMINATED;
                if (this->terminate(mon[i].tgid, mon[i].tid, processes) < 0) {
                    SPDLOG_ERROR("Failed to terminate monitor");
                    exit(1);
                }
            } else
#endif
            {
                // We still have an active or paused monitor => not all terminated
                allTerminated = false;
            }
        } else if (st != MONITOR_DISABLE) {
            // Possibly MONITOR_TERMINATED or other final states
            // Attempt to finalize if needed
            if (this->terminate(mon[i].tgid, mon[i].tid, processes) < 0) {
                SPDLOG_ERROR("Failed to terminate monitor");
                exit(1);
            }
        }
    }

    return allTerminated;
}
int Monitors::terminate(const uint32_t tgid, const uint32_t tid, const int32_t tnum) {
    int target = -1;

    for (int i = 0; i < tnum; i++) {
        if (mon[i].status == MONITOR_DISABLE) {
            continue;
        }
        if (mon[i].tgid != tgid || mon[i].tid != tid) {
            continue;
        }
        target = i;
        /* Save end time before trace serialization work. */
        if (mon[target].end_exec_ts.tv_sec == 0 && mon[target].end_exec_ts.tv_nsec == 0) {
            clock_gettime(CLOCK_MONOTONIC, &mon[i].end_exec_ts);
        }

        /* pebs stop */
#if CXLMEMSIM_HAS_LINUX_PERF
        delete mon[target].pebs_ctx;
        delete mon[target].lbr_ctx;
#endif
        mon[target].pebs_ctx = nullptr;
        mon[target].lbr_ctx = nullptr;
#if defined(__APPLE__) && !CXLMEMSIM_HAS_LINUX_PERF
        stop_xctrace_recording(mon[target].xctrace_pid);
        mon[target].xctrace_pid = -1;
        apply_xctrace_summary(mon[target]);
#endif

        /* display results */
        std::cout << std::format("========== Process {}[tgid={}, tid={}] statistics summary ==========\n", target,
                                 mon[target].tgid, mon[target].tid);
        double emulated_time =
            (double)(mon[target].end_exec_ts.tv_sec - mon[target].start_exec_ts.tv_sec) +
            (double)(mon[target].end_exec_ts.tv_nsec - mon[target].start_exec_ts.tv_nsec) / 1000000000;
        std::cout << std::format("emulated time ={}", emulated_time) << std::endl;
        std::cout << std::format("total delay   ={}", mon[target].total_delay) << std::endl;
        std::cout << std::format("PEBS sample total {} {}", mon[target].before->pebs.total,
                                 mon[target].after->pebs.llcmiss)
                  << std::endl;
        std::cout << std::format("LBR sample total {}", mon[target].before->lbr.total) << std::endl;
#if defined(__APPLE__) && !CXLMEMSIM_HAS_LINUX_PERF
        if (!mon[target].xctrace_output_path.empty()) {
            std::cout << std::format("xctrace output {}", mon[target].xctrace_output_path) << std::endl;
        }
#endif
        std::cout << std::format("{}", *controller) << std::endl;
        break;
    }

    return target;
}

void Monitor::stop() { // thread create and proecess create get the pmu
    int ret;

    if (this->is_process) {
        // In case of process, use SIGSTOP.
        SPDLOG_DEBUG("Send SIGSTOP to pid={}", this->tid);
        ret = kill(this->tid, SIGSTOP);
    } else {
#if CXLMEMSIM_HAS_LINUX_PERF
        // Use SIGUSR1 instead of SIGSTOP.
        // When the target thread receives SIGUSR1, it must stop until it receives SIGCONT.
        SPDLOG_DEBUG("Send SIGUSR1 to tid={}(tgid={})", this->tid, this->tgid);
        ret = syscall(SYS_tgkill, this->tgid, this->tid, SIGUSR1);
#else
        SPDLOG_DEBUG("Send SIGSTOP to pid={}", this->tid);
        ret = kill(this->tid, SIGSTOP);
#endif
    }

    if (ret == -1) {
        if (errno == ESRCH) {
            // in this case process or process group does not exist.
            // It might be a zombie or has terminated execution.
            this->status = MONITOR_TERMINATED;
        } else if (errno == EPERM) {
            this->status = MONITOR_NOPERMISSION;
            SPDLOG_ERROR("Failed to signal to any of the target processes. Due to does not have permission.  It "
                         "might have wrong result.");
        }
    } else {
        this->status = MONITOR_OFF;
        SPDLOG_DEBUG("Process [{}:{}] is stopped.", this->tgid, this->tid);
    }
}

void Monitor::run() {
    // SPDLOG_INFO("Send SIGCONT to tid={}(tgid={})", this->tid, this->tgid);
    // usleep(10);

#if CXLMEMSIM_HAS_LINUX_PERF
    const int ret = syscall(SYS_tgkill, this->tgid, this->tid, SIGCONT);
#else
    const int ret = kill(this->tid, SIGCONT);
#endif
    if (ret == -1) {
        if (errno == ESRCH) {
            // in this case process or process group does not exist.
            // It might be a zombie or has terminated execution.
            this->status = MONITOR_TERMINATED;
        } else if (errno == EPERM) {
            this->status = MONITOR_NOPERMISSION;
            SPDLOG_ERROR("Failed to signal to any of the target processes. Due to does not have permission.  It "
                         "might have wrong result.");
        } else {
            this->status = MONITOR_UNKNOWN;
            perror("Failed to signal to any of the target processes");
            SPDLOG_ERROR("I'm dying {} {}", this->tgid, this->tid);
        }
    } else {
        this->status = MONITOR_ON;
    }
}

void Monitor::clear_time(timespec *time) {
    time->tv_sec = 0;
    time->tv_nsec = 0;
}

Monitor::Monitor() // which one to hook
    : tgid(0), tid(0), cpu_core(0), status(0), injected_delay({0}), wasted_delay({0}), before(nullptr), after(nullptr),
      total_delay(0), start_exec_ts({0}), end_exec_ts({0}), is_process(false) {
#if defined(__APPLE__) && !CXLMEMSIM_HAS_LINUX_PERF
    xctrace_pid = -1;
#endif

    for (auto &j : this->elem) {
        j.cpus = std::vector<CPUElem>(helper.used_cpu.size());
        j.chas = std::vector<CHAElem>(helper.used_cha.size());
    }
}

[[maybe_unused]] static bool check_continue(const timespec wasted_delay, const timespec injected_delay) {
    // This equation for original one. but it causes like 45ms-> 60ms
    // calculated delay : 45ms
    // actual elapsed time : 60ms (default epoch: 20ms)
    if (wasted_delay.tv_sec > injected_delay.tv_sec ||
        (wasted_delay.tv_sec >= injected_delay.tv_sec && wasted_delay.tv_nsec >= injected_delay.tv_nsec)) {
        return true;
    }
    return false;
}

uint64_t operator-(const timespec &lhs, const timespec &rhs) {
    return (lhs.tv_sec - rhs.tv_sec) * 1000000000 + (lhs.tv_nsec - rhs.tv_nsec);
}

timespec operator+(const timespec &lhs, const timespec &rhs) {
    timespec result{};

    if (lhs.tv_nsec + rhs.tv_nsec >= 1000000000L) {
        result.tv_sec = lhs.tv_sec + rhs.tv_sec + 1;
        result.tv_nsec = lhs.tv_nsec - 1000000000L + rhs.tv_nsec;
    } else {
        result.tv_sec = lhs.tv_sec + rhs.tv_sec;
        result.tv_nsec = lhs.tv_nsec + rhs.tv_nsec;
    }

    return result;
}
timespec operator*(const timespec &lhs, const timespec &rhs) {
    timespec result{};

    if (lhs.tv_nsec < rhs.tv_nsec) {
        result.tv_sec = lhs.tv_sec - rhs.tv_sec - 1;
        result.tv_nsec = lhs.tv_nsec + 1000000000L - rhs.tv_nsec;
    } else {
        result.tv_sec = lhs.tv_sec - rhs.tv_sec;
        result.tv_nsec = lhs.tv_nsec - rhs.tv_nsec;
    }

    return result;
}
void Monitor::wait(std::vector<Monitor> *mons, int target) {
#if !CXLMEMSIM_HAS_LINUX_PERF
    (void)mons;
    (void)target;
    return;
#else
    auto &mon = (*mons)[target];
    uint64_t diff_nsec, target_nsec;
    timespec start_ts{}, end_ts{};
    timespec sleep_target{}, wanted_delay{}, interval_target{};
    timespec prev_wanted_delay = mon.wanted_delay;
    // while we're alive
    while ((mon.status == MONITOR_ON || mon.status == MONITOR_OFF)) {
        // figure out our delay
        wanted_delay = mon.wanted_delay;
        sleep_target = start_ts + wanted_delay * prev_wanted_delay;
        target_nsec = wanted_delay - prev_wanted_delay;
        interval_target = end_ts + interval_delay;
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &interval_target, nullptr);
        // start time before we ask them to sleep
        clock_gettime(CLOCK_MONOTONIC, &start_ts);
        mon.stop();
        diff_nsec = 0;

        // until we've waited enough time...
        while (diff_nsec < target_nsec) {
            SPDLOG_DEBUG("[{}:{}][OFF] total: {}", mon.tgid, mon.tid, wanted_delay.tv_nsec);
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &sleep_target, nullptr);
            clock_gettime(CLOCK_MONOTONIC, &end_ts);
            diff_nsec = end_ts - start_ts;
        }
        mon.run();
        prev_wanted_delay = wanted_delay;
        clock_gettime(CLOCK_MONOTONIC, &end_ts);
    }
    // SPDLOG_INFO("{}:{}", prev_wanted_delay.tv_sec, prev_wanted_delay.tv_nsec);
#endif
}
