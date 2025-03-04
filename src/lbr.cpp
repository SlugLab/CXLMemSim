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
 *	{ u64			time;     } && PERF_SAMPLE_TIME
 *	{ u64			addr;     } && PERF_SAMPLE_ADDR
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

LBR::LBR(pid_t pid, uint64_t sample_period) : pid(pid), sample_period(sample_period) {
    // Configure perf_event_attr struct
    /*perf_event_attr pe = {
        .type = PERF_TYPE_RAW,
        .size = sizeof(perf_event_attr),
        .config = 0x00D3, // mem_load_retired.l3_miss
        .sample_period = sample_period,
        .sample_type = PERF_SAMPLE_TID | PERF_SAMPLE_CPU | PERF_SAMPLE_TIME | PERF_SAMPLE_BRANCH_STACK,

        .read_format = PERF_FORMAT_TOTAL_TIME_ENABLED,
        .disabled = 1, // Event is initially disabled
        .exclude_user = 0,
        .exclude_kernel = 1,
        .exclude_hv = 1,
        .precise_ip = 1,
        .branch_sample_type = PERF_SAMPLE_BRANCH_USER | PERF_SAMPLE_BRANCH_ANY |PERF_SAMPLE_BRANCH_COUNTERS,
        //.config1 = 3,
    };*/
    perf_event_attr pe = {
        .type = PERF_TYPE_RAW,
        .size = sizeof(perf_event_attr),
        .config = 0x20d1, // mem_load_retired.l3_miss
        .sample_freq = sample_period,
        .sample_type = PERF_SAMPLE_TID | PERF_SAMPLE_CPU | PERF_SAMPLE_TIME | PERF_SAMPLE_BRANCH_STACK,
        .read_format = PERF_FORMAT_TOTAL_TIME_ENABLED,
        .disabled = 1, // Event is initially disabled
        .exclude_user = 0,
        .exclude_kernel = 1,
        .exclude_hv = 1,
        .precise_ip = 3,
        .config1 = 3,
        .branch_sample_type = PERF_SAMPLE_BRANCH_USER | PERF_SAMPLE_BRANCH_ANY | (1 << 19),
    };

    int cpu = -1; // measure on any cpu
    int group_fd = -1;
    unsigned long flags = 0;

    this->fd = perf_event_open(&pe, pid, cpu, group_fd, flags);
    if (this->fd == -1) {
        perror("perf_event_open");
        exit(EXIT_FAILURE);
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

int LBR::read(CXLController *controller, LBRElem *elem) {
    if (this->fd < 0) {
        return -1;
    }

    if (mp == MAP_FAILED)
        return -1;

    int r = 0;
    lbr_sample *data;
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
            case PERF_RECORD_SAMPLE:
                data = reinterpret_cast<lbr_sample *>(dp + this->rdlen % DATA_SIZE);

                if (header->size < sizeof(*data)) {
                    SPDLOG_DEBUG("size too small. size:{}", header->size);
                    r = -1;
                    return r;
                }
                if (header->size > sizeof(*data)) {
                    SPDLOG_DEBUG("size too big. size:{} / {}", header->size, sizeof(*data));
                }
                if (this->pid == data->pid) {
                    SPDLOG_DEBUG("pid:{} tid:{} size:{} nr2:{} data-size:{} cpu:{} timestamp:{} hw_idx: lbrs:{} "
                                 "counters:{} {} {}",
                                 data->pid, data->tid, header->size, data->nr2, sizeof(*data),
                                 data->cpu, data->timestamp, data->lbrs[0].from,
                                 data->counters[0].counters, data->counters[1].counters, data->counters[2].counters);

                    memcpy(&elem->branch_stack,
                           (char *)&data->counters + (32 * 8), // Cast to char* before arithmetic
                           92 * 8);
                    controller->insert(data->timestamp, data->tid, data->lbrs, data->counters);
                    elem->tid = data->tid;

                    elem->total++;
                    r = 1;
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
        perror("stop");
        throw;
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
