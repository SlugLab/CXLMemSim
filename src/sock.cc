#include "sock.h"
#include "cxlendpoint.h"
#include "helper.h"
#include "monitor.h"
#include "policy.h"
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cxxopts.hpp>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

Helper helper{};
int main() {
    // auto tnum = 1;
    // auto pebsperiod = 1000000;
    // std::vector<int> cpuset = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    // std::vector<std::string> pmu_name = {"1","2","3","4","5","6","7","8"};
    // std::vector<uint64_t> pmu_config1 = {0, 1, 2, 3, 4, 5, 6, 7};
    // std::vector<uint64_t> pmu_config2 = {0, 1, 2, 3, 4, 5, 6, 7};
    // uint64_t use_cpus = 0;
    // cpu_set_t use_cpuset;
    // CPU_ZERO(&use_cpuset);
    // for (auto i : cpuset) {
    //     if (!use_cpus || use_cpus & 1UL << i) {
    //         CPU_SET(i, &use_cpuset);
    //         LOG(DEBUG) << fmt::format("use cpuid: {}{}\n", i, use_cpus);
    //     }
    // }
    auto sock = socket(AF_UNIX, SOCK_DGRAM, 0);
    struct sockaddr_un addr {};

    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, SOCKET_PATH);
    remove(addr.sun_path);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) == -1) { // can be blocked for multi thread
        LOG(ERROR) << "Failed to execute. Can't bind to a socket.\n";
        exit(1);
    }

    size_t sock_buf_size = sizeof(op_data) + 1;
    char *sock_buf = (char *)malloc(sock_buf_size);

    // Monitors monitors{tnum, &use_cpuset};
    // auto perf_config =
    //     helper.detect_model(monitors.mon[0].before->cpuinfo.cpu_model, pmu_name, pmu_config1, pmu_config2);
    // PMUInfo pmu{1234, &helper, &perf_config};

    while (true) {
        /** Get from the CXLMemSimHook */
        int n;
        do {
            memset(sock_buf, 0, sock_buf_size);
            // without blocking
            n = recv(sock, sock_buf, sock_buf_size, MSG_DONTWAIT);
            if (n < 1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // no data
                    break;
                } else {
                    LOG(ERROR) << "Failed to recv";
                    exit(-1);
                }
            } else if (n >= sizeof(struct op_data) && n <= sock_buf_size - 1) {
                auto *opd = (struct op_data *)sock_buf;
                LOG(ERROR) << fmt::format("received data: size={}, tgid={}, tid=[], opcode={}\n", n, opd->tgid,
                                          opd->tid, opd->opcode);

                if (opd->opcode == CXLMEMSIM_THREAD_CREATE || opd->opcode == CXLMEMSIM_PROCESS_CREATE) {
                    int t;
                    bool is_process = opd->opcode == CXLMEMSIM_PROCESS_CREATE;
                    // register to monitor
                    LOG(DEBUG) << fmt::format("enable monitor: tgid={}, tid={}, is_process={}\n", opd->tgid, opd->tid,
                                               is_process);

                    // t = monitors.enable(opd->tgid, opd->tid, is_process, pebsperiod, tnum);
                    if (t == -1) {
                        LOG(ERROR) << "Failed to enable monitor\n";
                    } else if (t < 0) {
                        // tid not found. might be already terminated.
                        continue;
                    }
                    // auto mon = monitors.mon[t];
                    // Wait the t processes until emulation process initialized.
                    // mon.stop();
                    /* read CHA params */
                    // for (auto const &[idx, value] : pmu.chas | enumerate) {
                    //     pmu.chas[idx].read_cha_elems(&mon.before->chas[idx]);
                    // }
                    // for (auto const &[idx, value] : pmu.chas | enumerate) {
                    //     pmu.chas[idx].read_cha_elems(&mon.before->chas[idx]);
                    // }
                    // // Run the t processes.
                    // mon.run();
                    // clock_gettime(CLOCK_MONOTONIC, &mon.start_exec_ts);
                } else if (opd->opcode == CXLMEMSIM_THREAD_EXIT) {
                    // unregister from monitor, and display results.
                    // get the tid from the tgid
                    LOG(ERROR)<< fmt::format("disable monitor: tgid={}, tid={}\n", opd->tgid, opd->tid);
                    // auto mon = monitors.get_mon(opd->tgid, opd->tid);
                    // mon.stop();
                } else if (opd->opcode == CXLMEMSIM_STABLE_SIGNAL) {
                    // for (auto const &[i, mon] : monitors.mon | enumerate) {
                    //     if (mon.status == MONITOR_ON) {
                    //         mon.stop();
                    //         mon.status = MONITOR_SUSPEND;
                    //     }
                    // }
                }

            } else {
                LOG(ERROR) << fmt::format("received data is invalid size: size={}", n);
            }
        } while (n > 0); // check the next message.
    }
}