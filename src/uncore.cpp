/*
 * CXLMemSim uncore
 *
 *  By: Andrew Quinn
 *      Yiwei Yang
 *      Brian Zhao
 *  SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
 *  Copyright 2025 Regents of the University of California
 *  UC Santa Cruz Sluglab.
 */

#include "uncore.h"
#include <climits>
#include <fcntl.h>
#include <unistd.h>
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

    for (size_t k = 0; k < this->perf.size(); k++) {
        this->perf[k] = init_uncore_perf(-1, (int)unc_idx, std::get<1>(perf_config->cha[k]), std::get<2>(perf_config->cha[k]),
                             value);
    }
}

int Uncore::read_cha_elems(struct CHAElem *elem) {
    ssize_t r;
    for (size_t idx = 0; idx < this->perf.size(); idx++) {
        r = this->perf[idx]->read_pmu(&elem->cha[idx]);
        if (r < 0) {
            SPDLOG_ERROR("read cha_elems[{}] failed.\n", std::get<0>(helper.perf_conf.cha[idx]));
            return r;
        }
        SPDLOG_DEBUG("read cha_elems[{}]:{}\n", std::get<0>(helper.perf_conf.cha[idx]), elem->cha[idx]);
    }

    return 0;
}
