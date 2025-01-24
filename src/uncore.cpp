/*
 * CXLMemSim uncore
 *
 *  By: Andrew Quinn
 *      Yiwei Yang
 *
 *  Copyright 2025 Regents of the University of California
 *  UC Santa Cruz Sluglab.
 */

#include "uncore.h"
#include <climits>
#include <unistd.h>
#include <fcntl.h>
extern Helper helper;
Uncore::Uncore(const uint32_t unc_idx, PerfConfig *perf_config) {
    unsigned long value;
    int r;
    char path[64], buf[32];
    memset(path, 0, sizeof(path));
    snprintf(path, sizeof(path) - 1, perf_config->path_format_cha_type.c_str(), unc_idx);

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        SPDLOG_ERROR("open {} failed", path);
        throw std::runtime_error("open");
    }

    memset(buf, 0, sizeof(buf));
    r = read(fd, buf, sizeof(buf) - 1);
    if (r < 0) {
        SPDLOG_ERROR("read {} failed", fd);
        close(fd);
        throw std::runtime_error("read");
    }
    close(fd);

    value = strtoul(buf, nullptr, 10);
    if (value == ULONG_MAX) {
        SPDLOG_ERROR("strtoul {} failed", fd);
        throw std::runtime_error("strtoul");
    }

    for (auto const &[k, v] : this->perf | std::views::enumerate) {
        v = init_uncore_perf(-1, (int)unc_idx, std::get<1>(perf_config->cha[k]), std::get<2>(perf_config->cha[k]),
                             value);
    }
}

int Uncore::read_cha_elems(struct CHAElem *elem) {
    ssize_t r;
    for (auto const &[idx, value] : this->perf | std::views::enumerate) {
        r = value->read_pmu(&elem->cha[idx]);
        if (r < 0) {
            SPDLOG_ERROR("read cha_elems[{}] failed.\n", std::get<0>(helper.perf_conf.cha[idx]));
            return r;
        }
        SPDLOG_DEBUG("read cha_elems[{}]:{}\n", std::get<0>(helper.perf_conf.cha[idx]), elem->cha[idx]);
    }

    return 0;
}
