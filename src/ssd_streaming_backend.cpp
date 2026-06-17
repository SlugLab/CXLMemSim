/*
 * Persistent SSD streaming backend for CXLMemSim.
 */

#include "../include/ssd_streaming_backend.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <memory>
#include <sys/stat.h>
#include <sys/types.h>
#include <utility>
#include <vector>
#include <unistd.h>

#ifdef __linux__
#include <linux/io_uring.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#endif

#ifndef O_NOATIME
#define O_NOATIME 0
#endif

#ifndef O_DIRECT
#define O_DIRECT 0
#endif

namespace {

constexpr size_t kDirectIoAlignment = 4096;

bool is_power_of_two(uint64_t value) { return value != 0 && (value & (value - 1)) == 0; }

bool full_pread(int fd, uint8_t *dst, size_t size, off_t offset) {
    size_t done = 0;
    while (done < size) {
        ssize_t ret = pread(fd, dst + done, size - done, offset + static_cast<off_t>(done));
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (ret == 0) {
            memset(dst + done, 0, size - done);
            return true;
        }
        done += static_cast<size_t>(ret);
    }
    return true;
}

bool full_pwrite(int fd, const uint8_t *src, size_t size, off_t offset) {
    size_t done = 0;
    while (done < size) {
        ssize_t ret = pwrite(fd, src + done, size - done, offset + static_cast<off_t>(done));
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (ret == 0) {
            return false;
        }
        done += static_cast<size_t>(ret);
    }
    return true;
}

} // namespace

struct SsdStreamingBackend::IoUringState {
#ifdef __linux__
    int ring_fd = -1;
    void *sq_ring = MAP_FAILED;
    void *cq_ring = MAP_FAILED;
    struct io_uring_sqe *sqes = nullptr;
    size_t sq_ring_size = 0;
    size_t cq_ring_size = 0;
    size_t sqes_size = 0;
    bool single_mmap = false;

    uint32_t *sq_head = nullptr;
    uint32_t *sq_tail = nullptr;
    uint32_t *sq_ring_mask = nullptr;
    uint32_t *sq_ring_entries = nullptr;
    uint32_t *sq_flags = nullptr;
    uint32_t *sq_array = nullptr;

    uint32_t *cq_head = nullptr;
    uint32_t *cq_tail = nullptr;
    uint32_t *cq_ring_mask = nullptr;
    struct io_uring_cqe *cqes = nullptr;
#endif
};

SsdStreamingBackend::SsdStreamingBackend(SsdStreamingConfig config) : config_(std::move(config)) {}

SsdStreamingBackend::~SsdStreamingBackend() { shutdown(); }

void SsdStreamingBackend::PageBufferDeleter::operator()(uint8_t *ptr) const noexcept { std::free(ptr); }

std::unique_ptr<uint8_t, SsdStreamingBackend::PageBufferDeleter> SsdStreamingBackend::allocate_page_buffer() const {
    void *raw = nullptr;
    const size_t alignment = std::max<size_t>(config_.page_size, kDirectIoAlignment);
    const int ret = posix_memalign(&raw, alignment, config_.page_size);
    if (ret != 0) {
        errno = ret;
        return nullptr;
    }
    memset(raw, 0, config_.page_size);
    return std::unique_ptr<uint8_t, PageBufferDeleter>(static_cast<uint8_t *>(raw));
}

bool SsdStreamingBackend::validate_config() const {
    if (config_.backing_path.empty()) {
        std::cerr << "SSD streaming backend requires a backing path\n";
        return false;
    }
    if (config_.capacity_bytes == 0) {
        std::cerr << "SSD streaming backend requires non-zero capacity\n";
        return false;
    }
    if (!is_power_of_two(config_.page_size) || config_.page_size < 512) {
        std::cerr << "SSD streaming page size must be a power of two and at least 512 bytes\n";
        return false;
    }
    if (config_.use_odirect && O_DIRECT != 0 && config_.page_size < kDirectIoAlignment) {
        std::cerr << "SSD streaming O_DIRECT page size must be at least " << kDirectIoAlignment << " bytes\n";
        return false;
    }
    if (config_.capacity_bytes % config_.page_size != 0) {
        std::cerr << "SSD streaming capacity must be aligned to page size for direct I/O\n";
        return false;
    }
    if (config_.capacity_bytes > static_cast<uint64_t>(std::numeric_limits<off_t>::max())) {
        std::cerr << "SSD streaming capacity exceeds host off_t range\n";
        return false;
    }
    return true;
}

bool SsdStreamingBackend::open_backing() {
    const auto open_with_flags = [this](int extra_flags) {
        int flags = O_RDWR | O_CREAT | O_NOATIME | extra_flags;
        int fd = open(config_.backing_path.c_str(), flags, 0666);
        if (fd < 0 && O_NOATIME != 0) {
            flags = O_RDWR | O_CREAT | extra_flags;
            fd = open(config_.backing_path.c_str(), flags, 0666);
        }
        return fd;
    };

    int extra_flags = 0;
    if (config_.use_odirect) {
        extra_flags |= O_DIRECT;
    }

    fd_ = open_with_flags(extra_flags);
    if (fd_ >= 0) {
        odirect_enabled_ = (extra_flags & O_DIRECT) != 0;
        return true;
    }

    if (config_.use_odirect && O_DIRECT != 0) {
        std::cerr << "O_DIRECT open failed for " << config_.backing_path << ": " << strerror(errno)
                  << "; falling back to buffered I/O\n";
        fd_ = open_with_flags(0);
        odirect_enabled_ = false;
    }

    if (fd_ < 0) {
        std::cerr << "Failed to open SSD streaming backing " << config_.backing_path << ": " << strerror(errno)
                  << "\n";
        return false;
    }
    return true;
}

bool SsdStreamingBackend::ensure_size() {
    struct stat st {};
    if (fstat(fd_, &st) != 0) {
        std::cerr << "fstat failed for SSD streaming backing " << config_.backing_path << ": " << strerror(errno)
                  << "\n";
        return false;
    }

    if (S_ISREG(st.st_mode) && st.st_size < static_cast<off_t>(config_.capacity_bytes)) {
        if (ftruncate(fd_, static_cast<off_t>(config_.capacity_bytes)) != 0) {
            std::cerr << "ftruncate failed for SSD streaming backing " << config_.backing_path << ": "
                      << strerror(errno) << "\n";
            return false;
        }
    }
    return true;
}

bool SsdStreamingBackend::initialize_io_uring() {
    io_uring_enabled_ = false;
    if (!config_.use_io_uring) {
        return false;
    }

#ifndef __linux__
    return false;
#else
    io_buffer_ = allocate_page_buffer();
    if (!io_buffer_) {
        std::cerr << "Failed to allocate io_uring registered buffer: " << strerror(errno) << "\n";
        return false;
    }

    auto ring = std::make_unique<IoUringState>();
    struct io_uring_params params {};
    ring->ring_fd = static_cast<int>(syscall(__NR_io_uring_setup, 64U, &params));
    if (ring->ring_fd < 0) {
        std::cerr << "io_uring_setup failed for " << config_.backing_path << ": " << strerror(errno)
                  << "; falling back to pread/pwrite\n";
        io_buffer_.reset();
        return false;
    }

    ring->sq_ring_size = params.sq_off.array + params.sq_entries * sizeof(uint32_t);
    ring->cq_ring_size = params.cq_off.cqes + params.cq_entries * sizeof(struct io_uring_cqe);
    if ((params.features & IORING_FEAT_SINGLE_MMAP) != 0) {
        ring->single_mmap = true;
        ring->sq_ring_size = std::max(ring->sq_ring_size, ring->cq_ring_size);
        ring->cq_ring_size = ring->sq_ring_size;
    }

    ring->sq_ring = mmap(nullptr, ring->sq_ring_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                         ring->ring_fd, IORING_OFF_SQ_RING);
    if (ring->sq_ring == MAP_FAILED) {
        std::cerr << "io_uring SQ mmap failed: " << strerror(errno) << "; falling back to pread/pwrite\n";
        close(ring->ring_fd);
        io_buffer_.reset();
        return false;
    }

    if (ring->single_mmap) {
        ring->cq_ring = ring->sq_ring;
    } else {
        ring->cq_ring = mmap(nullptr, ring->cq_ring_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                             ring->ring_fd, IORING_OFF_CQ_RING);
        if (ring->cq_ring == MAP_FAILED) {
            std::cerr << "io_uring CQ mmap failed: " << strerror(errno) << "; falling back to pread/pwrite\n";
            munmap(ring->sq_ring, ring->sq_ring_size);
            close(ring->ring_fd);
            io_buffer_.reset();
            return false;
        }
    }

    ring->sqes_size = params.sq_entries * sizeof(struct io_uring_sqe);
    ring->sqes = static_cast<struct io_uring_sqe *>(
        mmap(nullptr, ring->sqes_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, ring->ring_fd,
             IORING_OFF_SQES));
    if (ring->sqes == MAP_FAILED) {
        std::cerr << "io_uring SQE mmap failed: " << strerror(errno) << "; falling back to pread/pwrite\n";
        if (!ring->single_mmap) {
            munmap(ring->cq_ring, ring->cq_ring_size);
        }
        munmap(ring->sq_ring, ring->sq_ring_size);
        close(ring->ring_fd);
        io_buffer_.reset();
        return false;
    }

    auto *sq_base = static_cast<uint8_t *>(ring->sq_ring);
    auto *cq_base = static_cast<uint8_t *>(ring->cq_ring);
    ring->sq_head = reinterpret_cast<uint32_t *>(sq_base + params.sq_off.head);
    ring->sq_tail = reinterpret_cast<uint32_t *>(sq_base + params.sq_off.tail);
    ring->sq_ring_mask = reinterpret_cast<uint32_t *>(sq_base + params.sq_off.ring_mask);
    ring->sq_ring_entries = reinterpret_cast<uint32_t *>(sq_base + params.sq_off.ring_entries);
    ring->sq_flags = reinterpret_cast<uint32_t *>(sq_base + params.sq_off.flags);
    ring->sq_array = reinterpret_cast<uint32_t *>(sq_base + params.sq_off.array);
    ring->cq_head = reinterpret_cast<uint32_t *>(cq_base + params.cq_off.head);
    ring->cq_tail = reinterpret_cast<uint32_t *>(cq_base + params.cq_off.tail);
    ring->cq_ring_mask = reinterpret_cast<uint32_t *>(cq_base + params.cq_off.ring_mask);
    ring->cqes = reinterpret_cast<struct io_uring_cqe *>(cq_base + params.cq_off.cqes);

    int fixed_fd = fd_;
    if (syscall(__NR_io_uring_register, ring->ring_fd, IORING_REGISTER_FILES, &fixed_fd, 1U) != 0) {
        std::cerr << "io_uring file registration failed: " << strerror(errno) << "; falling back to pread/pwrite\n";
        munmap(ring->sqes, ring->sqes_size);
        if (!ring->single_mmap) {
            munmap(ring->cq_ring, ring->cq_ring_size);
        }
        munmap(ring->sq_ring, ring->sq_ring_size);
        close(ring->ring_fd);
        io_buffer_.reset();
        return false;
    }

    struct iovec iov {};
    iov.iov_base = io_buffer_.get();
    iov.iov_len = config_.page_size;
    if (syscall(__NR_io_uring_register, ring->ring_fd, IORING_REGISTER_BUFFERS, &iov, 1U) != 0) {
        std::cerr << "io_uring buffer registration failed: " << strerror(errno) << "; falling back to pread/pwrite\n";
        syscall(__NR_io_uring_register, ring->ring_fd, IORING_UNREGISTER_FILES, nullptr, 0U);
        munmap(ring->sqes, ring->sqes_size);
        if (!ring->single_mmap) {
            munmap(ring->cq_ring, ring->cq_ring_size);
        }
        munmap(ring->sq_ring, ring->sq_ring_size);
        close(ring->ring_fd);
        io_buffer_.reset();
        return false;
    }

    io_uring_ = std::move(ring);
    io_uring_enabled_ = true;
    return true;
#endif
}

void SsdStreamingBackend::shutdown_io_uring() {
#ifdef __linux__
    if (!io_uring_) {
        io_buffer_.reset();
        io_uring_enabled_ = false;
        return;
    }

    IoUringState *ring = io_uring_.get();
    if (ring->ring_fd >= 0) {
        syscall(__NR_io_uring_register, ring->ring_fd, IORING_UNREGISTER_BUFFERS, nullptr, 0U);
        syscall(__NR_io_uring_register, ring->ring_fd, IORING_UNREGISTER_FILES, nullptr, 0U);
    }
    if (ring->sqes && ring->sqes != MAP_FAILED) {
        munmap(ring->sqes, ring->sqes_size);
    }
    if (ring->cq_ring && ring->cq_ring != MAP_FAILED && !ring->single_mmap) {
        munmap(ring->cq_ring, ring->cq_ring_size);
    }
    if (ring->sq_ring && ring->sq_ring != MAP_FAILED) {
        munmap(ring->sq_ring, ring->sq_ring_size);
    }
    if (ring->ring_fd >= 0) {
        close(ring->ring_fd);
    }
    io_uring_.reset();
#endif
    io_buffer_.reset();
    io_uring_enabled_ = false;
}

bool SsdStreamingBackend::submit_io_uring_rw(uint8_t opcode, uint8_t *buffer, off_t offset) {
#ifndef __linux__
    (void)opcode;
    (void)buffer;
    (void)offset;
    return false;
#else
    if (!io_uring_) {
        return false;
    }

    IoUringState *ring = io_uring_.get();
    uint32_t tail = *ring->sq_tail;
    uint32_t next_tail = tail + 1;
    if (next_tail - *ring->sq_head > *ring->sq_ring_entries) {
        return false;
    }

    uint32_t index = tail & *ring->sq_ring_mask;
    struct io_uring_sqe *sqe = &ring->sqes[index];
    memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = opcode;
    sqe->flags = IOSQE_FIXED_FILE;
    sqe->fd = 0;
    sqe->off = static_cast<uint64_t>(offset);
    sqe->addr = reinterpret_cast<uint64_t>(buffer);
    sqe->len = config_.page_size;
    sqe->buf_index = 0;
    sqe->user_data = 1;
    ring->sq_array[index] = index;

    std::atomic_thread_fence(std::memory_order_release);
    *ring->sq_tail = next_tail;

    int ret = static_cast<int>(
        syscall(__NR_io_uring_enter, ring->ring_fd, 1U, 1U, IORING_ENTER_GETEVENTS, nullptr, 0U));
    if (ret < 0) {
        std::cerr << "io_uring_enter failed: " << strerror(errno) << "\n";
        return false;
    }

    for (;;) {
        std::atomic_thread_fence(std::memory_order_acquire);
        uint32_t head = *ring->cq_head;
        if (head != *ring->cq_tail) {
            struct io_uring_cqe *cqe = &ring->cqes[head & *ring->cq_ring_mask];
            int result = cqe->res;
            *ring->cq_head = head + 1;
            if (result < 0) {
                errno = -result;
                return false;
            }
            return static_cast<uint32_t>(result) == config_.page_size;
        }

        ret = static_cast<int>(
            syscall(__NR_io_uring_enter, ring->ring_fd, 0U, 1U, IORING_ENTER_GETEVENTS, nullptr, 0U));
        if (ret < 0 && errno != EINTR) {
            std::cerr << "io_uring completion wait failed: " << strerror(errno) << "\n";
            return false;
        }
    }
#endif
}

bool SsdStreamingBackend::read_page(uint8_t *dst, off_t offset) {
#ifdef __linux__
    if (io_uring_enabled_) {
        std::lock_guard<std::mutex> io_lock(io_mutex_);
        if (submit_io_uring_rw(IORING_OP_READ_FIXED, io_buffer_.get(), offset)) {
            memcpy(dst, io_buffer_.get(), config_.page_size);
            return true;
        }
        std::cerr << "io_uring read failed at offset " << offset << ": " << strerror(errno)
                  << "; retrying with pread\n";
    }
#endif
    return full_pread(fd_, dst, config_.page_size, offset);
}

bool SsdStreamingBackend::write_page(const uint8_t *src, off_t offset) {
#ifdef __linux__
    if (io_uring_enabled_) {
        std::lock_guard<std::mutex> io_lock(io_mutex_);
        memcpy(io_buffer_.get(), src, config_.page_size);
        if (submit_io_uring_rw(IORING_OP_WRITE_FIXED, io_buffer_.get(), offset)) {
            return true;
        }
        std::cerr << "io_uring write failed at offset " << offset << ": " << strerror(errno)
                  << "; retrying with pwrite\n";
    }
#endif
    return full_pwrite(fd_, src, config_.page_size, offset);
}

bool SsdStreamingBackend::initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) {
        return true;
    }
    if (!validate_config() || !open_backing() || !ensure_size()) {
        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1;
        }
        return false;
    }

    if (config_.cache_pages == 0) {
        config_.cache_pages = std::max<size_t>(1024, (64ULL * 1024 * 1024) / config_.page_size);
    }
    if (config_.read_ahead_pages == 0 && config_.io_chunk_size > config_.page_size) {
        config_.read_ahead_pages = config_.io_chunk_size / config_.page_size;
    }

    (void)initialize_io_uring();
    initialized_ = true;
    std::cerr << "SSD streaming backend initialized: path=" << config_.backing_path
              << " capacity=" << config_.capacity_bytes << " page=" << config_.page_size
              << " cache_pages=" << config_.cache_pages << " readahead=" << config_.read_ahead_pages
              << " odirect=" << odirect_enabled_ << " io_uring=" << io_uring_enabled_ << "\n";
    return true;
}

void SsdStreamingBackend::shutdown() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!initialized_ && fd_ < 0) {
        return;
    }

    initialized_ = false;
    std::vector<PagePtr> snapshot;
    snapshot.reserve(pages_.size());
    for (auto &entry : pages_) {
        snapshot.push_back(entry.second);
    }

    for (const PagePtr &page : snapshot) {
        while (page->state == SsdPageState::Loading || page->state == SsdPageState::Evicting) {
            page->cv.wait(lock);
        }
        flush_page_locked(page, lock);
    }

    pages_.clear();
    clock_queue_.clear();
    shutdown_io_uring();
    const int fd = fd_;
    fd_ = -1;
    lock.unlock();

    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
}

bool SsdStreamingBackend::range_valid(uint64_t addr, size_t size) const {
    if (size == 0) {
        return false;
    }
    if (addr > std::numeric_limits<uint64_t>::max() - (size - 1)) {
        return false;
    }
    return addr + size <= config_.capacity_bytes;
}

SsdStreamingBackend::PagePtr SsdStreamingBackend::get_or_create_page_locked(uint64_t page_no) {
    auto it = pages_.find(page_no);
    if (it != pages_.end()) {
        return it->second;
    }

    auto page = std::make_shared<PageMeta>();
    page->va_page_no = page_no;
    page->backing_slot = page_no;
    page->data = allocate_page_buffer();
    if (!page->data) {
        std::cerr << "Failed to allocate aligned SSD streaming page buffer: " << strerror(errno) << "\n";
        return nullptr;
    }
    pages_.emplace(page_no, page);
    clock_queue_.push_back(page_no);
    return page;
}

bool SsdStreamingBackend::load_page_locked(const PagePtr &page, std::unique_lock<std::mutex> &lock) {
    const uint64_t offset = page->backing_slot * config_.page_size;
    if (offset >= config_.capacity_bytes) {
        return false;
    }

    page->state = SsdPageState::Loading;
    page->refcnt++;

    lock.unlock();
    bool ok = read_page(page->data.get(), static_cast<off_t>(offset));
    lock.lock();

    page->refcnt--;
    if (ok) {
        page->state = SsdPageState::ResidentClean;
        page->dirty = false;
        page->accessed = true;
        page->generation = ++generation_;
        stats_.page_faults++;
    } else {
        std::cerr << "SSD streaming read failed at offset " << offset << ": " << strerror(errno) << "\n";
        page->state = SsdPageState::NonResident;
    }
    page->cv.notify_all();
    return ok;
}

bool SsdStreamingBackend::flush_page_locked(const PagePtr &page, std::unique_lock<std::mutex> &lock) {
    if (!page) {
        return true;
    }
    while (page->state == SsdPageState::Loading || page->state == SsdPageState::Evicting) {
        page->cv.wait(lock);
    }
    if (!page->dirty) {
        return true;
    }
    const uint64_t offset = page->backing_slot * config_.page_size;
    page->state = SsdPageState::Evicting;

    lock.unlock();
    bool ok = write_page(page->data.get(), static_cast<off_t>(offset));
    lock.lock();

    if (!ok) {
        std::cerr << "SSD streaming writeback failed at offset " << offset << ": " << strerror(errno) << "\n";
        page->state = SsdPageState::ResidentDirty;
        return false;
    }
    page->dirty = false;
    page->state = SsdPageState::ResidentClean;
    stats_.dirty_flushes++;
    stats_.bytes_written += config_.page_size;
    page->cv.notify_all();
    return true;
}

bool SsdStreamingBackend::evict_page_locked(uint64_t page_no, std::unique_lock<std::mutex> &lock) {
    auto it = pages_.find(page_no);
    if (it == pages_.end()) {
        return true;
    }
    const PagePtr page = it->second;
    while (page->state == SsdPageState::Loading || page->state == SsdPageState::Evicting) {
        page->cv.wait(lock);
    }
    if (page->pinned || page->refcnt != 0) {
        return false;
    }
    if (!flush_page_locked(page, lock)) {
        return false;
    }
    page->state = SsdPageState::NonResident;
    pages_.erase(page_no);
    stats_.evictions++;
    return true;
}

bool SsdStreamingBackend::evict_one_locked(std::unique_lock<std::mutex> &lock) {
    if (pages_.empty()) {
        return true;
    }

    const size_t attempts = clock_queue_.size() * 2 + 1;
    for (size_t i = 0; i < attempts && !clock_queue_.empty(); i++) {
        uint64_t page_no = clock_queue_.front();
        clock_queue_.pop_front();
        auto it = pages_.find(page_no);
        if (it == pages_.end()) {
            continue;
        }
        PagePtr page = it->second;
        if (page->pinned || page->refcnt != 0 || page->state == SsdPageState::Loading ||
            page->state == SsdPageState::Evicting) {
            clock_queue_.push_back(page_no);
            continue;
        }
        if (page->accessed) {
            page->accessed = false;
            clock_queue_.push_back(page_no);
            continue;
        }
        return evict_page_locked(page_no, lock);
    }
    return false;
}

bool SsdStreamingBackend::ensure_page_loaded_locked(uint64_t page_no, bool issue_readahead,
                                                    std::unique_lock<std::mutex> &lock) {
    if (pages_.find(page_no) == pages_.end()) {
        while (pages_.size() >= config_.cache_pages && !pages_.empty()) {
            if (!evict_one_locked(lock)) {
                return false;
            }
        }
    }

    PagePtr page = get_or_create_page_locked(page_no);
    if (!page) {
        return false;
    }

    while (page->state == SsdPageState::Loading || page->state == SsdPageState::Evicting) {
        page->cv.wait(lock);
    }

    if (page->state == SsdPageState::ResidentClean || page->state == SsdPageState::ResidentDirty) {
        page->accessed = true;
        page->generation = ++generation_;
        stats_.cache_hits++;
        return true;
    }

    while (pages_.size() > config_.cache_pages) {
        if (!evict_one_locked(lock)) {
            break;
        }
    }

    if (!load_page_locked(page, lock)) {
        return false;
    }

    if (issue_readahead) {
        page->refcnt++;
        const uint64_t last_page = (config_.capacity_bytes - 1) / config_.page_size;
        for (uint32_t i = 1; i <= config_.read_ahead_pages && page_no + i <= last_page; i++) {
            uint64_t ra_page_no = page_no + i;
            if (pages_.find(ra_page_no) == pages_.end()) {
                while (pages_.size() >= config_.cache_pages && !pages_.empty()) {
                    if (!evict_one_locked(lock)) {
                        page->refcnt--;
                        return true;
                    }
                }
            }
            PagePtr ra_page = get_or_create_page_locked(ra_page_no);
            if (!ra_page) {
                page->refcnt--;
                return true;
            }
            if (ra_page->state == SsdPageState::NonResident) {
                (void)load_page_locked(ra_page, lock);
            }
            while (pages_.size() > config_.cache_pages) {
                if (!evict_one_locked(lock)) {
                    break;
                }
            }
        }
        page->refcnt--;
    }
    return true;
}

bool SsdStreamingBackend::read(uint64_t addr, uint8_t *dst, size_t size) {
    if (!dst || !range_valid(addr, size)) {
        return false;
    }

    std::unique_lock<std::mutex> lock(mutex_);
    if (!initialized_) {
        return false;
    }

    size_t copied = 0;
    while (copied < size) {
        uint64_t current = addr + copied;
        uint64_t page_no = addr_to_page(current);
        size_t offset = page_offset(current);
        size_t chunk = std::min(size - copied, static_cast<size_t>(config_.page_size - offset));

        if (!ensure_page_loaded_locked(page_no, copied == 0, lock)) {
            return false;
        }
        PagePtr page = pages_.at(page_no);
        memcpy(dst + copied, page->data.get() + offset, chunk);
        copied += chunk;
    }

    stats_.reads++;
    stats_.bytes_read += size;
    return true;
}

bool SsdStreamingBackend::write(uint64_t addr, const uint8_t *src, size_t size) {
    if (!src || !range_valid(addr, size)) {
        return false;
    }

    std::unique_lock<std::mutex> lock(mutex_);
    if (!initialized_) {
        return false;
    }

    size_t copied = 0;
    while (copied < size) {
        uint64_t current = addr + copied;
        uint64_t page_no = addr_to_page(current);
        size_t offset = page_offset(current);
        size_t chunk = std::min(size - copied, static_cast<size_t>(config_.page_size - offset));

        if (!ensure_page_loaded_locked(page_no, false, lock)) {
            return false;
        }
        PagePtr page = pages_.at(page_no);
        memcpy(page->data.get() + offset, src + copied, chunk);
        page->dirty = true;
        page->state = SsdPageState::ResidentDirty;
        page->accessed = true;
        page->generation = ++generation_;
        copied += chunk;
    }

    stats_.writes++;
    stats_.bytes_written += size;
    return true;
}

bool SsdStreamingBackend::flush() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!initialized_) {
        return false;
    }

    std::vector<PagePtr> snapshot;
    snapshot.reserve(pages_.size());
    for (auto &entry : pages_) {
        entry.second->refcnt++;
        snapshot.push_back(entry.second);
    }

    bool ok = true;
    for (const PagePtr &page : snapshot) {
        ok = flush_page_locked(page, lock) && ok;
    }
    for (const PagePtr &page : snapshot) {
        page->refcnt--;
    }

    if (!ok) {
        return false;
    }
    if (fsync(fd_) != 0) {
        std::cerr << "SSD streaming fsync failed: " << strerror(errno) << "\n";
        return false;
    }
    return true;
}

SsdStreamingStats SsdStreamingBackend::get_stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

bool SsdStreamingBackend::prefetch(uint64_t addr, size_t size) {
    if (!range_valid(addr, size)) {
        return false;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    if (!initialized_) {
        return false;
    }
    uint64_t first = addr_to_page(addr);
    uint64_t last = addr_to_page(addr + size - 1);
    for (uint64_t page_no = first; page_no <= last; page_no++) {
        if (!ensure_page_loaded_locked(page_no, false, lock)) {
            return false;
        }
    }
    return true;
}

bool SsdStreamingBackend::evict_after(uint64_t addr, size_t size) {
    if (!range_valid(addr, size)) {
        return false;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    if (!initialized_) {
        return false;
    }
    uint64_t first = addr_to_page(addr);
    uint64_t last = addr_to_page(addr + size - 1);
    bool ok = true;
    for (uint64_t page_no = first; page_no <= last; page_no++) {
        ok = evict_page_locked(page_no, lock) && ok;
    }
    return ok;
}

bool SsdStreamingBackend::pin(uint64_t addr, size_t size) {
    if (!range_valid(addr, size)) {
        return false;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    if (!initialized_) {
        return false;
    }
    uint64_t first = addr_to_page(addr);
    uint64_t last = addr_to_page(addr + size - 1);
    for (uint64_t page_no = first; page_no <= last; page_no++) {
        if (!ensure_page_loaded_locked(page_no, false, lock)) {
            return false;
        }
        pages_.at(page_no)->pinned = true;
    }
    return true;
}

bool SsdStreamingBackend::unpin(uint64_t addr, size_t size) {
    if (!range_valid(addr, size)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
        return false;
    }
    uint64_t first = addr_to_page(addr);
    uint64_t last = addr_to_page(addr + size - 1);
    for (uint64_t page_no = first; page_no <= last; page_no++) {
        auto it = pages_.find(page_no);
        if (it != pages_.end()) {
            it->second->pinned = false;
        }
    }
    return true;
}

bool SsdStreamingBackend::set_streaming(uint64_t addr, size_t size) {
    if (!range_valid(addr, size)) {
        return false;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    if (!initialized_) {
        return false;
    }
    uint64_t first = addr_to_page(addr);
    uint64_t last = addr_to_page(addr + size - 1);
    for (uint64_t page_no = first; page_no <= last; page_no++) {
        if (pages_.find(page_no) == pages_.end()) {
            while (pages_.size() >= config_.cache_pages && !pages_.empty()) {
                if (!evict_one_locked(lock)) {
                    return false;
                }
            }
        }
        PagePtr page = get_or_create_page_locked(page_no);
        if (!page) {
            return false;
        }
        page->streaming = true;
        page->accessed = false;
    }
    return true;
}
