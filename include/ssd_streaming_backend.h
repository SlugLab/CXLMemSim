/*
 * Persistent SSD streaming backend for CXLMemSim.
 *
 * This backend keeps a bounded in-memory page cache over a persistent backing
 * file or block device.  The MVP is synchronous internally, but keeps explicit
 * page states so the same API can grow into an io_uring-backed fault service.
 */

#ifndef SSD_STREAMING_BACKEND_H
#define SSD_STREAMING_BACKEND_H

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <sys/types.h>
#include <unordered_map>

enum class SsdPageState : uint32_t {
    NonResident = 0,
    Loading = 1,
    ResidentClean = 2,
    ResidentDirty = 3,
    Evicting = 4,
};

struct SsdStreamingConfig {
    std::string backing_path;
    uint64_t capacity_bytes = 0;
    uint32_t page_size = 4096;
    uint32_t io_chunk_size = 65536;
    size_t cache_pages = 0;
    uint32_t read_ahead_pages = 16;
    bool use_io_uring = true;
    bool use_odirect = true;
};

struct SsdStreamingStats {
    uint64_t reads = 0;
    uint64_t writes = 0;
    uint64_t page_faults = 0;
    uint64_t cache_hits = 0;
    uint64_t evictions = 0;
    uint64_t dirty_flushes = 0;
    uint64_t bytes_read = 0;
    uint64_t bytes_written = 0;
};

class SsdStreamingBackend {
public:
    explicit SsdStreamingBackend(SsdStreamingConfig config);
    ~SsdStreamingBackend();

    SsdStreamingBackend(const SsdStreamingBackend &) = delete;
    SsdStreamingBackend &operator=(const SsdStreamingBackend &) = delete;

    bool initialize();
    void shutdown();

    bool read(uint64_t addr, uint8_t *dst, size_t size);
    bool write(uint64_t addr, const uint8_t *src, size_t size);
    bool flush();

    SsdStreamingStats get_stats() const;

    bool prefetch(uint64_t addr, size_t size);
    bool evict_after(uint64_t addr, size_t size);
    bool pin(uint64_t addr, size_t size);
    bool unpin(uint64_t addr, size_t size);
    bool set_streaming(uint64_t addr, size_t size);

    const SsdStreamingConfig &config() const { return config_; }

private:
    struct PageBufferDeleter {
        void operator()(uint8_t *ptr) const noexcept;
    };

    struct IoUringState;

    struct PageMeta {
        uint64_t va_page_no = 0;
        uint64_t backing_slot = 0;
        SsdPageState state = SsdPageState::NonResident;
        uint32_t refcnt = 0;
        uint64_t generation = 0;
        bool dirty = false;
        bool accessed = false;
        bool pinned = false;
        bool streaming = false;
        std::unique_ptr<uint8_t, PageBufferDeleter> data;
        std::condition_variable cv;
    };

    using PagePtr = std::shared_ptr<PageMeta>;

    std::unique_ptr<uint8_t, PageBufferDeleter> allocate_page_buffer() const;
    bool validate_config() const;
    bool open_backing();
    bool ensure_size();
    bool initialize_io_uring();
    void shutdown_io_uring();
    bool read_page(uint8_t *dst, off_t offset);
    bool write_page(const uint8_t *src, off_t offset);
    bool submit_io_uring_rw(uint8_t opcode, uint8_t *buffer, off_t offset);
    bool ensure_page_loaded_locked(uint64_t page_no, bool issue_readahead, std::unique_lock<std::mutex> &lock);
    bool load_page_locked(const PagePtr &page, std::unique_lock<std::mutex> &lock);
    bool flush_page_locked(const PagePtr &page, std::unique_lock<std::mutex> &lock);
    bool evict_one_locked(std::unique_lock<std::mutex> &lock);
    bool evict_page_locked(uint64_t page_no, std::unique_lock<std::mutex> &lock);
    PagePtr get_or_create_page_locked(uint64_t page_no);
    uint64_t addr_to_page(uint64_t addr) const { return addr / config_.page_size; }
    size_t page_offset(uint64_t addr) const { return static_cast<size_t>(addr % config_.page_size); }
    bool range_valid(uint64_t addr, size_t size) const;

    SsdStreamingConfig config_;
    int fd_ = -1;
    bool initialized_ = false;
    bool odirect_enabled_ = false;
    bool io_uring_enabled_ = false;
    uint64_t generation_ = 0;
    std::list<uint64_t> clock_queue_;
    std::unordered_map<uint64_t, PagePtr> pages_;
    std::unique_ptr<IoUringState> io_uring_;
    std::unique_ptr<uint8_t, PageBufferDeleter> io_buffer_;
    mutable std::mutex mutex_;
    mutable std::mutex io_mutex_;
    SsdStreamingStats stats_;
};

#endif // SSD_STREAMING_BACKEND_H
