//
// Created by victoryang00 on 1/12/23.
//

#include "uncore.h"
Uncore::Uncore(const uint32_t unc_idx, PerfConfig *perf_config) {
    int ret, fd;
    ssize_t r;
    unsigned long value;
    char path[64], buf[32];

    memset(path, 0, sizeof(path));
    snprintf(path, sizeof(path) - 1, perf_config->path_format_cbo_type, unc_idx);

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        LOG(ERROR) << fmt::format("open {} failed", path);
        throw std::runtime_error("open");
    }

    memset(buf, 0, sizeof(buf));
    r = read(fd, buf, sizeof(buf) - 1);
    if (r < 0) {
        LOG(ERROR) << fmt::format("read {} failed", path);
        close(fd);
        throw std::runtime_error("read");
    }
    close(fd);

    value = strtoul(buf, nullptr, 10);
    if (value == ULONG_MAX) {
        LOG(ERROR) << fmt::format("strtoul {} failed", path);
        throw std::runtime_error("strtoul");
    }

    int cpu = (int)unc_idx;
    pid_t pid = -1; /* when using uncore, pid must be -1. */
    int group_fd = -1;
    auto attr = perf_event_attr{
        .type = (uint32_t)value,
        .size = sizeof(struct perf_event_attr),
        .config = perf_config->cbo_config,
        .disabled = 1,
        .inherit = 1,
        .enable_on_exec = 1,
    };

    /* when using uncore, don't set exclude_xxx flags. */
    this->perf = new PerfInfo(group_fd, cpu, pid, 0, attr);
}

int Uncore::read_cbo_elems(struct CBOElem *elem) {
    int r = this->perf->read_pmu(&elem->llc_wb);
    if (r < 0) {
        LOG(ERROR) << fmt::format("perf_read_pmu failed.\n");
    }

    LOG(DEBUG) << fmt::format("llc_wb:{}\n", elem->llc_wb);
    return r;
}
