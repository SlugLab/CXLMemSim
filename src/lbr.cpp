/*
 * CXLMemSim lbr
 *
 *  By: Andrew Quinn
 *      Yiwei Yang
 *      Brian Zhao
 *  SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
 *  Copyright 2025 Regents of the University of California
 *  UC Santa Cruz Sluglab.
 */

#include "lbr.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <system_error>
#include <vector>

namespace {

constexpr size_t kMaxLbrEntries = 32;
constexpr uint64_t kLbrSampleType = PERF_SAMPLE_TID | PERF_SAMPLE_CPU | PERF_SAMPLE_TIME | PERF_SAMPLE_BRANCH_STACK;

struct ParsedLbrSample {
    uint32_t pid = 0;
    uint32_t tid = 0;
    uint32_t cpu = 0;
    uint64_t timestamp = 0;
    uint64_t nr = 0;
    size_t copied_entries = 0;
    lbr lbrs[kMaxLbrEntries]{};
    cntr counters[kMaxLbrEntries]{};
};

template <typename T> bool read_sample_field(const char *record, size_t record_size, size_t &offset, T &out) {
    if (offset + sizeof(T) > record_size) {
        return false;
    }
    std::memcpy(&out, record + offset, sizeof(T));
    offset += sizeof(T);
    return true;
}

perf_event_attr make_lbr_attr(uint64_t sample_period, bool branch_counters, bool precise, bool generic_event) {
    perf_event_attr attr{
        .type = generic_event ? PERF_TYPE_HARDWARE : PERF_TYPE_RAW,
        .size = sizeof(perf_event_attr),
        .config = static_cast<__u64>(generic_event ? PERF_COUNT_HW_CACHE_MISSES : 0x20d1),
        .sample_period = sample_period,
        .sample_type = kLbrSampleType,
        .read_format = PERF_FORMAT_TOTAL_TIME_ENABLED,
        .disabled = 1,
        .exclude_user = 0,
        .exclude_kernel = 1,
        .exclude_hv = 1,
        .branch_sample_type = PERF_SAMPLE_BRANCH_USER | PERF_SAMPLE_BRANCH_ANY,
    };

    if (precise) {
        attr.precise_ip = 3;
        attr.config1 = 3;
    }
#ifdef PERF_SAMPLE_BRANCH_COUNTERS
    if (branch_counters) {
        attr.branch_sample_type |= PERF_SAMPLE_BRANCH_COUNTERS;
    }
#else
    (void)branch_counters;
#endif
    return attr;
}

bool parse_lbr_sample_record(const perf_event_header *header, bool has_branch_counters, ParsedLbrSample &sample) {
    static_assert(sizeof(lbr) == sizeof(perf_branch_entry), "lbr must match Linux perf_branch_entry layout");

    const char *record = reinterpret_cast<const char *>(header);
    size_t offset = sizeof(perf_event_header);
    uint32_t cpu_reserved = 0;

    if (!read_sample_field(record, header->size, offset, sample.pid) ||
        !read_sample_field(record, header->size, offset, sample.tid) ||
        !read_sample_field(record, header->size, offset, sample.timestamp) ||
        !read_sample_field(record, header->size, offset, sample.cpu) ||
        !read_sample_field(record, header->size, offset, cpu_reserved) ||
        !read_sample_field(record, header->size, offset, sample.nr)) {
        return false;
    }

    const size_t entries_to_copy = std::min<uint64_t>(sample.nr, kMaxLbrEntries);
    for (uint64_t i = 0; i < sample.nr; ++i) {
        lbr entry{};
        if (!read_sample_field(record, header->size, offset, entry)) {
            return false;
        }
        if (i < entries_to_copy) {
            sample.lbrs[i] = entry;
        }
    }

    if (has_branch_counters) {
        for (uint64_t i = 0; i < sample.nr; ++i) {
            cntr counter{};
            if (!read_sample_field(record, header->size, offset, counter)) {
                return false;
            }
            if (i < entries_to_copy) {
                sample.counters[i] = counter;
            }
        }
    }

    sample.copied_entries = entries_to_copy;
    return true;
}

void copy_lbr_stack_to_elem(const ParsedLbrSample &sample, LBRElem *elem) {
    std::fill(std::begin(elem->branch_stack), std::end(elem->branch_stack), 0);
    for (size_t i = 0; i < sample.copied_entries; ++i) {
        elem->branch_stack[i * 3] = sample.lbrs[i].from;
        elem->branch_stack[i * 3 + 1] = sample.lbrs[i].to;
        elem->branch_stack[i * 3 + 2] = sample.lbrs[i].flags;
    }
}

} // namespace

/*
 * struct {
 *	struct perf_event_header	header;
 *
 *	#
 *	# Note that PERF_SAMPLE_IDENTIFIER duplicates PERF_SAMPLE_ID.
 *	# The advantage of PERF_SAMPLE_IDENTIFIER is that its position
 *	# is fixed relative to header.
 *	#
 *
 *	{ u64			id;	  } && PERF_SAMPLE_IDENTIFIER
 *	{ u64			ip;	  } && PERF_SAMPLE_IP
 *	{ u32			pid, tid; } && PERF_SAMPLE_TID
 *	{ u64			time;
    } && PERF_SAMPLE_TIME
 *	{ u64			addr;
    } && PERF_SAMPLE_ADDR
 *	{ u64			id;	  } && PERF_SAMPLE_ID
 *	{ u64			stream_id;} && PERF_SAMPLE_STREAM_ID
 *	{ u32			cpu, res; } && PERF_SAMPLE_CPU
 *	{ u64			period;   } && PERF_SAMPLE_PERIOD
 *
 *	{ struct read_format	values;	  } && PERF_SAMPLE_READ
 *
 *	{ u64			nr,
 *	  u64			ips[nr];  } && PERF_SAMPLE_CALLCHAIN
 *
 *	#
 *	# The RAW record below is opaque data wrt the ABI
 *	#
 *	# That is, the ABI doesn't make any promises wrt to
 *	# the stability of its content, it may vary depending
 *	# on event, hardware, kernel version and phase of
 *	# the moon.
 *	#
 *	# In other words, PERF_SAMPLE_RAW contents are not an ABI.
 *	#
 *
 *	{ u32			size;
 *	  char                  data[size];}&& PERF_SAMPLE_RAW
 *
 *	{ u64                   nr;
 *	  { u64	hw_idx; } && PERF_SAMPLE_BRANCH_HW_INDEX
 *        { u64 from, to, flags } lbr[nr];
 *        #
 *        # The format of the counters is decided by the
 *        # "branch_counter_nr" and "branch_counter_width",
 *        # which are defined in the ABI.
 *        #
 *        { u64 counters; } cntr[nr] && PERF_SAMPLE_BRANCH_COUNTERS
 *   } && PERF_SAMPLE_BRANCH_STACK */

LBR::LBR(pid_t pid, uint64_t sample_period)
    : fd(-1), pid(pid), sample_period(sample_period), mplen(0), use_pe2(false), sample_has_branch_counters(false),
      mp(static_cast<perf_event_mmap_page *>(MAP_FAILED)) {
    struct Candidate {
        const char *name;
        perf_event_attr attr;
        bool has_branch_counters;
    };

    std::vector<Candidate> candidates{
        {"Intel LBR load miss with branch counters", make_lbr_attr(sample_period, true, true, false), true},
        {"Intel LBR load miss", make_lbr_attr(sample_period, false, true, false), false},
        {"generic cache-miss LBR", make_lbr_attr(sample_period, false, false, true), false},
    };

    int cpu = -1; // measure on any cpu
    int group_fd = -1;
    unsigned long flags = 0;

    int last_errno = 0;
    const char *last_candidate = "LBR sampling";
    for (const auto &candidate : candidates) {
        this->fd = perf_event_open(const_cast<perf_event_attr *>(&candidate.attr), pid, cpu, group_fd, flags);
        if (this->fd != -1) {
            this->sample_has_branch_counters = candidate.has_branch_counters;
            this->use_pe2 = !candidate.has_branch_counters;
            SPDLOG_INFO("LBR sampler initialized using {} (branch_counters={})", candidate.name,
                        candidate.has_branch_counters);
            break;
        }
        last_errno = errno;
        last_candidate = candidate.name;
        SPDLOG_DEBUG("Failed to initialize {}: {}", candidate.name, std::strerror(errno));
    }

    if (this->fd == -1) {
        throw std::system_error(last_errno, std::generic_category(),
                                std::string("perf_event_open failed for ") + last_candidate);
    }
    this->mplen = MMAP_SIZE;
    this->mp = (perf_event_mmap_page *)mmap(nullptr, MMAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, this->fd, 0);

    if (this->mp == MAP_FAILED) {
        int saved_errno = errno;
        close(this->fd);
        this->fd = -1;
        throw std::system_error(saved_errno, std::generic_category(), "mmap failed for LBR perf ring buffer");
    }
    if (this->start() < 0) {
        int saved_errno = errno;
        munmap(this->mp, this->mplen);
        this->mp = static_cast<perf_event_mmap_page *>(MAP_FAILED);
        close(this->fd);
        this->fd = -1;
        throw std::system_error(saved_errno, std::generic_category(), "failed to start LBR perf event");
    }
}

int LBR::read(CXLController *controller, LBRElem *elem) {
    if (this->fd < 0) {
        return -1;
    }

    if (mp == MAP_FAILED)
        return -1;

    int r = 0;
    char *dp = (char *)mp + PAGE_SIZE;

    do {
        this->seq = mp->lock; // explicit copy
        barrier();
        const uint64_t last_head = mp->data_head;
        while (this->rdlen < last_head) {
            const auto *header = reinterpret_cast<perf_event_header *>(dp + this->rdlen % DATA_SIZE);

            // printf("read lbr\n");
            switch (header->type) {
            case PERF_RECORD_LOST:
                SPDLOG_DEBUG("received PERF_RECORD_LOST");
                break;
            case PERF_RECORD_SAMPLE: {
                ParsedLbrSample sample;
                if (!parse_lbr_sample_record(header, sample_has_branch_counters, sample)) {
                    SPDLOG_DEBUG("invalid LBR sample size: {}", header->size);
                    r = -1;
                    continue;
                }
                if (this->pid == static_cast<int>(sample.pid)) {
                    SPDLOG_DEBUG("pid:{} tid:{} size:{} entries:{} copied:{} cpu:{} timestamp:{} first_from:{:#x} "
                                 "first_to:{:#x} first_flags:{:#x} first_counter:{}",
                                 sample.pid, sample.tid, header->size, sample.nr, sample.copied_entries, sample.cpu,
                                 sample.timestamp, sample.lbrs[0].from, sample.lbrs[0].to, sample.lbrs[0].flags,
                                 sample.counters[0].counters);

                    copy_lbr_stack_to_elem(sample, elem);
                    controller->insert(sample.timestamp, sample.tid, sample.lbrs, sample.counters);
                    elem->tid = sample.tid;

                    elem->total++;
                    r = 1;
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
int LBR::start() const {
    if (this->fd < 0) {
        return 0;
    }
    if (ioctl(this->fd, PERF_EVENT_IOC_ENABLE, 0) < 0) {
        perror("ioctl");
        return -1;
    }

    return 0;
}

int LBR::stop() const {
    if (this->fd < 0) {
        return 0;
    }
    if (ioctl(this->fd, PERF_EVENT_IOC_DISABLE, 0) < 0) {
        perror("ioctl");
        return -1;
    }
    return 0;
}

LBR::~LBR() {
    if (this->stop() < 0) {
        SPDLOG_ERROR("failed to stop LBR");
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
