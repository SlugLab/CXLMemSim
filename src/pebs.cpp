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

#include <cerrno>
#include <cpuid.h>
#include <cstring>
#include <fstream>
#include <optional>
#include <system_error>
#include <vector>

namespace {

struct ParsedPerfSample {
    uint32_t pid = 0;
    uint32_t tid = 0;
    uint64_t timestamp = 0;
    uint64_t addr = 0;
    uint64_t value = 0;
    uint64_t time_enabled = 0;
    uint64_t phys_addr = 0;
};

std::string cpu_vendor_id() {
#if defined(__x86_64__) || defined(__i386__)
    unsigned int eax = 0;
    unsigned int ebx = 0;
    unsigned int ecx = 0;
    unsigned int edx = 0;
    if (__get_cpuid(0, &eax, &ebx, &ecx, &edx) == 0) {
        return {};
    }
    char vendor[13] = {};
    std::memcpy(vendor, &ebx, sizeof(ebx));
    std::memcpy(vendor + 4, &edx, sizeof(edx));
    std::memcpy(vendor + 8, &ecx, sizeof(ecx));
    return vendor;
#else
    return {};
#endif
}

std::optional<uint32_t> read_event_source_type(const char *path) {
    std::ifstream input(path);
    uint32_t type = 0;
    if (input >> type) {
        return type;
    }
    return std::nullopt;
}

uint64_t pebs_sample_type(bool include_phys_addr) {
    uint64_t sample_type = PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_ADDR | PERF_SAMPLE_READ;
#ifdef PERF_SAMPLE_PHYS_ADDR
    if (include_phys_addr) {
        sample_type |= PERF_SAMPLE_PHYS_ADDR;
    }
#endif
    return sample_type;
}

perf_event_attr make_intel_pebs_attr(uint64_t sample_period, bool include_phys_addr) {
    return perf_event_attr{
        .type = PERF_TYPE_RAW,
        .size = sizeof(struct perf_event_attr),
        .config = 0x20d1, // mem_load_retired.l3_miss
        .sample_period = sample_period,
        .sample_type = pebs_sample_type(include_phys_addr),
        .read_format = PERF_FORMAT_TOTAL_TIME_ENABLED,
        .disabled = 1, // Event is initially disabled
        .exclude_kernel = 1,
        .precise_ip = 1,
        .config1 = 3,
    }; // excluding events that happen in the kernel-space
}

perf_event_attr make_amd_ibs_op_attr(uint32_t ibs_type, uint64_t sample_period, bool include_phys_addr) {
    return perf_event_attr{
        .type = ibs_type,
        .size = sizeof(struct perf_event_attr),
        .config = 0,
        .sample_period = sample_period,
        .sample_type = pebs_sample_type(include_phys_addr),
        .read_format = PERF_FORMAT_TOTAL_TIME_ENABLED,
        .disabled = 1,
        .exclude_kernel = 1,
    };
}

perf_event_attr make_generic_cache_miss_attr(uint64_t sample_period) {
    return perf_event_attr{
        .type = PERF_TYPE_HARDWARE,
        .size = sizeof(struct perf_event_attr),
        .config = PERF_COUNT_HW_CACHE_MISSES,
        .sample_period = sample_period,
        .sample_type = pebs_sample_type(false),
        .read_format = PERF_FORMAT_TOTAL_TIME_ENABLED,
        .disabled = 1,
        .exclude_kernel = 1,
    };
}

template <typename T> bool read_sample_field(const char *record, size_t record_size, size_t &offset, T &out) {
    if (offset + sizeof(T) > record_size) {
        return false;
    }
    std::memcpy(&out, record + offset, sizeof(T));
    offset += sizeof(T);
    return true;
}

bool parse_perf_sample_record(const perf_event_header *header, bool has_phys_addr, ParsedPerfSample &sample) {
    const char *record = reinterpret_cast<const char *>(header);
    size_t offset = sizeof(perf_event_header);

    if (!read_sample_field(record, header->size, offset, sample.pid) ||
        !read_sample_field(record, header->size, offset, sample.tid) ||
        !read_sample_field(record, header->size, offset, sample.timestamp) ||
        !read_sample_field(record, header->size, offset, sample.addr) ||
        !read_sample_field(record, header->size, offset, sample.value) ||
        !read_sample_field(record, header->size, offset, sample.time_enabled)) {
        return false;
    }

    if (has_phys_addr) {
        if (!read_sample_field(record, header->size, offset, sample.phys_addr)) {
            return false;
        }
    } else {
        sample.phys_addr = sample.addr;
    }

    return true;
}

} // namespace

PEBS::PEBS(pid_t pid, uint64_t sample_period)
    : fd(-1), pid(pid), sample_period(sample_period), sample_has_phys_addr(false),
      mp(static_cast<perf_event_mmap_page *>(MAP_FAILED)) {
    struct Candidate {
        const char *name;
        perf_event_attr attr;
        bool has_phys_addr;
    };

    std::vector<Candidate> candidates;
    const std::string vendor = cpu_vendor_id();
    if (vendor == "AuthenticAMD") {
        if (auto ibs_type = read_event_source_type("/sys/bus/event_source/devices/ibs_op/type")) {
            candidates.push_back(
                {"AMD IBS op with physical address", make_amd_ibs_op_attr(*ibs_type, sample_period, true), true});
            candidates.push_back({"AMD IBS op", make_amd_ibs_op_attr(*ibs_type, sample_period, false), false});
        }
        candidates.push_back({"generic hardware cache misses", make_generic_cache_miss_attr(sample_period), false});
    } else {
        candidates.push_back(
            {"Intel PEBS load miss with physical address", make_intel_pebs_attr(sample_period, true), true});
        candidates.push_back({"Intel PEBS load miss", make_intel_pebs_attr(sample_period, false), false});
        candidates.push_back({"generic hardware cache misses", make_generic_cache_miss_attr(sample_period), false});
    }

    int cpu = -1; // measure on any cpu
    int group_fd = -1;
    unsigned long flags = 0;

    int last_errno = 0;
    const char *last_candidate = "perf sampling";
    for (auto &candidate : candidates) {
        this->fd = perf_event_open(&candidate.attr, pid, cpu, group_fd, flags);
        if (this->fd != -1) {
            this->sample_has_phys_addr = candidate.has_phys_addr;
            SPDLOG_INFO("PEBS sampler initialized using {}", candidate.name);
            break;
        }
        last_errno = errno;
        last_candidate = candidate.name;
        SPDLOG_DEBUG("Failed to initialize {}: {}", candidate.name, std::strerror(errno));
    }

    if (this->fd == -1) {
        throw std::system_error(last_errno, std::generic_category(),
                                std::string("perf_event_open failed for ") + last_candidate +
                                    ". Check CPU PMU support and kernel.perf_event_paranoid");
    }

    this->mplen = MMAP_SIZE;
    this->mp = (perf_event_mmap_page *)mmap(nullptr, MMAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, this->fd, 0);

    if (this->mp == MAP_FAILED) {
        int saved_errno = errno;
        close(this->fd);
        this->fd = -1;
        throw std::system_error(saved_errno, std::generic_category(), "mmap failed for PEBS perf ring buffer");
    }

    if (this->start() < 0) {
        int saved_errno = errno;
        munmap(this->mp, this->mplen);
        this->mp = static_cast<perf_event_mmap_page *>(MAP_FAILED);
        close(this->fd);
        this->fd = -1;
        throw std::system_error(saved_errno, std::generic_category(), "failed to start PEBS perf event");
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
            case PERF_RECORD_SAMPLE: {
                ParsedPerfSample sample;
                if (!parse_perf_sample_record(header, sample_has_phys_addr, sample)) {
                    SPDLOG_DEBUG("invalid PEBS sample size: {}", header->size);
                    r = -1;
                    continue;
                }
                if (this->pid == static_cast<int>(sample.pid)) {
                    SPDLOG_TRACE("pid:{} tid:{} time:{} addr:{} phys_addr:{} llc_miss:{} timestamp={}\n", sample.pid,
                                 sample.tid, sample.time_enabled, sample.addr, sample.phys_addr, sample.value,
                                 sample.timestamp);
                    controller->insert(sample.timestamp, sample.tid, sample.phys_addr, sample.addr, sample.value);
                    elem->total++;
                    elem->llcmiss = sample.value; // this is the number of llc miss
                }
                break;
            }
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
