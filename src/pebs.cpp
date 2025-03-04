/*
 * CXLMemSim pebs
 *
 *  By: Andrew Quinn
 *      Yiwei Yang
 *      Brian Zhao
 *  SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
 *  Copyright 2025 Regents of the University of California
 *  UC Santa Cruz Sluglab.
 */

#include "pebs.h"

struct perf_sample {
    perf_event_header header;
    uint32_t pid;
    uint32_t tid;
    uint64_t timestamp;
    uint64_t addr;
    uint64_t value;
    uint64_t time_enabled;
    uint64_t phys_addr;
};

PEBS::PEBS(pid_t pid, uint64_t sample_period) : pid(pid), sample_period(sample_period) {
    // Configure perf_event_attr struct
    perf_event_attr pe = {
        .type = PERF_TYPE_RAW,
        .size = sizeof(struct perf_event_attr),
        .config = 0x20d1, // mem_load_retired.l3_miss
        .sample_period = sample_period,
        .sample_type = PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_ADDR | PERF_SAMPLE_READ | PERF_SAMPLE_PHYS_ADDR,
        .read_format = PERF_FORMAT_TOTAL_TIME_ENABLED,
        .disabled = 1, // Event is initially disabled
        .exclude_kernel = 1,
        .precise_ip = 1,
        .config1 = 3,
    }; // excluding events that happen in the kernel-space

    int cpu = -1; // measure on any cpu
    int group_fd = -1;
    unsigned long flags = 0;

    this->fd = perf_event_open(&pe, pid, cpu, group_fd, flags);
    if (this->fd == -1) {
        perror("perf_event_open");
        throw;
    }

    this->mplen = MMAP_SIZE;
    this->mp = (perf_event_mmap_page *)mmap(nullptr, MMAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, this->fd, 0);

    if (this->mp == MAP_FAILED) {
        perror("mmap");
        throw;
    }

    if (this->start() < 0) {
        perror("start");
        throw;
    }
}
int PEBS::read(CXLController *controller, PEBSElem *elem) {
    if (this->fd < 0) {
        return 0;
    }

    if (mp == MAP_FAILED)
        return -1;

    int r = 0;
    perf_event_header *header;
    perf_sample *data;
    uint64_t last_head;
    char *dp = (char *)mp + PAGE_SIZE;

    do {
        this->seq = mp->lock; // explicit copy
        barrier();
        last_head = mp->data_head;
        while (this->rdlen < last_head) {
            header = reinterpret_cast<perf_event_header *>(dp + this->rdlen % DATA_SIZE);

            switch (header->type) {
            case PERF_RECORD_LOST:
                SPDLOG_DEBUG("received PERF_RECORD_LOST\n");
                break;
            case PERF_RECORD_SAMPLE:
                data = (struct perf_sample *)(dp + this->rdlen % DATA_SIZE);

                if (header->size < sizeof(*data)) {
                    SPDLOG_DEBUG("size too small. size:{}\n", header->size);
                    r = -1;
                    continue;
                }
                if (this->pid == data->pid) {
                    SPDLOG_TRACE("pid:{} tid:{} time:{} addr:{} phys_addr:{} llc_miss:{} timestamp={}\n", data->pid,
                                 data->tid, data->time_enabled, data->addr, data->phys_addr, data->value,
                                 data->timestamp);
                    controller->insert(data->timestamp, data->tid, data->phys_addr, data->addr, data->value);
                    elem->total++;
                    elem->llcmiss = data->value; // this is the number of llc miss
                }
                break;
            case PERF_RECORD_THROTTLE:
                SPDLOG_DEBUG("received PERF_RECORD_THROTTLE\n");
                break;
            case PERF_RECORD_UNTHROTTLE:
                SPDLOG_DEBUG("received PERF_RECORD_UNTHROTTLE\n");
                break;
            case PERF_RECORD_LOST_SAMPLES:
                SPDLOG_DEBUG("received PERF_RECORD_LOST_SAMPLES\n");
                break;
            default:
                SPDLOG_DEBUG("other data received. type:{}\n", header->type);
                break;
            }

            this->rdlen += header->size;
        }

        mp->data_tail = last_head;
        barrier();
    } while (mp->lock != this->seq);

    return r;
}
int PEBS::start() const {
    if (this->fd < 0) {
        return 0;
    }
    if (ioctl(this->fd, PERF_EVENT_IOC_ENABLE, 0) < 0) {
        perror("ioctl");
        return -1;
    }

    return 0;
}
int PEBS::stop() const {
    if (this->fd < 0) {
        return 0;
    }
    if (ioctl(this->fd, PERF_EVENT_IOC_DISABLE, 0) < 0) {
        perror("ioctl");
        return -1;
    }
    return 0;
}
PEBS::~PEBS() {
    if (this->stop() < 0) {
        SPDLOG_ERROR("failed to stop PEBS");
    }

    if (this->fd < 0) {
        return;
    }

    if (this->mp != MAP_FAILED) {
        munmap(this->mp, this->mplen);
        this->mp = static_cast<perf_event_mmap_page *>(MAP_FAILED);
        this->mplen = 0;
    }

    if (this->fd != -1) {
        close(this->fd);
        this->fd = -1;
    }

    this->pid = -1;
}
