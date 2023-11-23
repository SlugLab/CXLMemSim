//
// Created by victoryang00 on 1/13/23.
//

#include "pebs.h"

#define PAGE_SIZE 4096
#define DATA_SIZE PAGE_SIZE
#define MMAP_SIZE (PAGE_SIZE + DATA_SIZE)

#define barrier() _mm_mfence()

struct perf_sample {
    struct perf_event_header header;
    uint32_t pid;
    uint32_t tid;
    uint64_t timestamp;
    uint64_t addr;
    uint64_t value;
    uint64_t time_enabled;
    uint64_t phys_addr;
};

long perf_event_open(struct perf_event_attr *event_attr, pid_t pid, int cpu, int group_fd, unsigned long flags) {
    return syscall(__NR_perf_event_open, event_attr, pid, cpu, group_fd, flags);
}
PEBS::PEBS(pid_t pid, uint64_t sample_period) : pid(pid), sample_period(sample_period) {
    // Configure perf_event_attr struct
    struct perf_event_attr pe = {
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

    this->start();
}
int PEBS::read(CXLController *controller, struct PEBSElem *elem) {
    if (this->fd < 0) {
        return 0;
    }

    if (mp == MAP_FAILED)
        return -1;

    int r = 0;
    struct perf_event_header *header;
    struct perf_sample *data;
    uint64_t last_head;
    char *dp = ((char *)mp) + PAGE_SIZE;

    do {
        this->seq = mp->lock; // explicit copy
        barrier();
        last_head = mp->data_head;
        while ((uint64_t)this->rdlen < last_head) {
            header = (struct perf_event_header *)(dp + this->rdlen % DATA_SIZE);

            switch (header->type) {
            case PERF_RECORD_LOST:
                LOG(DEBUG) << fmt::format("received PERF_RECORD_LOST\n");
                break;
            case PERF_RECORD_SAMPLE:
                data = (struct perf_sample *)(dp + this->rdlen % DATA_SIZE);

                if (header->size < sizeof(*data)) {
                    LOG(DEBUG) << fmt::format("size too small. size:{}\n", header->size);
                    r = -1;
                    continue;
                }
                if (this->pid == data->pid) {
                    LOG(ERROR) << fmt::format("pid:{} tid:{} time:{} addr:{} phys_addr:{} llc_miss:{} timestamp={}\n",
                                              data->pid, data->tid, data->time_enabled, data->addr, data->phys_addr,
                                              data->value, data->timestamp);
                    controller->insert(data->timestamp, data->phys_addr, data->addr, 0);
                    elem->total++;
                    elem->llcmiss = data->value; // this is the number of llc miss
                }
                break;
            case PERF_RECORD_THROTTLE:
                LOG(DEBUG) << "received PERF_RECORD_THROTTLE\n";
                break;
            case PERF_RECORD_UNTHROTTLE:
                LOG(DEBUG) << "received PERF_RECORD_UNTHROTTLE\n";
                break;
            case PERF_RECORD_LOST_SAMPLES:
                LOG(DEBUG) << "received PERF_RECORD_LOST_SAMPLES\n";
                break;
            default:
                LOG(DEBUG) << fmt::format("other data received. type:{}\n", header->type);
                break;
            }

            this->rdlen += header->size;
        }

        mp->data_tail = last_head;
        barrier();
    } while (mp->lock != this->seq);
    
    return r;
}
int PEBS::start() {
    if (this->fd < 0) {
        return 0;
    }
    if (ioctl(this->fd, PERF_EVENT_IOC_ENABLE, 0) < 0) {
        perror("ioctl");
        return -1;
    }

    return 0;
}
int PEBS::stop() {
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
    this->stop();

    if (this->fd < 0) {
        return;
    }

    if (this->mp != MAP_FAILED) {
        munmap(this->mp, this->mplen);
        this->mp = reinterpret_cast<struct perf_event_mmap_page *>(MAP_FAILED);
        this->mplen = 0;
    }

    if (this->fd != -1) {
        close(this->fd);
        this->fd = -1;
    }

    this->pid = -1;
}
