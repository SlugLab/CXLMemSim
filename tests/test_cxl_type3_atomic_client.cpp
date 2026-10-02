#include "cxl_type3_atomic.h"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

namespace {

constexpr std::size_t kBarSize = 0x1000;
constexpr std::size_t kMagic = 0x00;
constexpr std::size_t kVersion = 0x04;
constexpr std::size_t kCaps = 0x08;
constexpr std::size_t kStatus = 0x0c;
constexpr std::size_t kOp = 0x10;
constexpr std::size_t kServerStatus = 0x14;
constexpr std::size_t kAddr = 0x18;
constexpr std::size_t kValue = 0x20;
constexpr std::size_t kExpected = 0x28;
constexpr std::size_t kOldValue = 0x30;
constexpr std::size_t kDoorbell = 0x38;

#define CHECK(expression)                                                                                              \
    do {                                                                                                               \
        if (!(expression)) {                                                                                           \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expression);                      \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (false)

template <typename T> T &reg(void *bar, std::size_t offset) {
    return *reinterpret_cast<T *>(static_cast<std::uint8_t *>(bar) + offset);
}

} // namespace

int main() {
    char path[] = "/tmp/cxl-type3-atomic-test-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    unlink(path);
    CHECK(ftruncate(fd, kBarSize) == 0);
    void *bar = mmap(nullptr, kBarSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(bar != MAP_FAILED);

    reg<std::uint32_t>(bar, kMagic) = 0x54415843U;
    reg<std::uint32_t>(bar, kVersion) = 1;
    reg<std::uint32_t>(bar, kCaps) = 7;

    std::atomic<bool> stop{false};
    std::uint64_t memory[4]{};
    std::thread device([&] {
        std::uint32_t previous_doorbell = 0;
        while (!stop.load(std::memory_order_acquire)) {
            const auto doorbell = __atomic_load_n(&reg<std::uint32_t>(bar, kDoorbell), __ATOMIC_ACQUIRE);
            if (doorbell == previous_doorbell) {
                std::this_thread::yield();
                continue;
            }
            previous_doorbell = doorbell;
            if (doorbell == 0) {
                __atomic_store_n(&reg<std::uint32_t>(bar, kStatus), 0U, __ATOMIC_RELEASE);
                continue;
            }
            if (doorbell != 1) {
                continue;
            }
            __atomic_store_n(&reg<std::uint32_t>(bar, kStatus), 1U, __ATOMIC_RELEASE);
            const auto op = reg<std::uint32_t>(bar, kOp);
            const auto addr = reg<std::uint64_t>(bar, kAddr);
            auto &word = memory[addr / sizeof(std::uint64_t)];
            std::uint64_t old = word;

            if (op == 3) {
                word += reg<std::uint64_t>(bar, kValue);
            } else if (op == 4 && word == reg<std::uint64_t>(bar, kExpected)) {
                word = reg<std::uint64_t>(bar, kValue);
            } else if (op != 4 && op != 5) {
                reg<std::uint32_t>(bar, kServerStatus) = 4;
                __atomic_store_n(&reg<std::uint32_t>(bar, kStatus), 3U, __ATOMIC_RELEASE);
                continue;
            }
            reg<std::uint64_t>(bar, kOldValue) = old;
            reg<std::uint32_t>(bar, kServerStatus) = 0;
            __atomic_store_n(&reg<std::uint32_t>(bar, kStatus), 2U, __ATOMIC_RELEASE);
        }
    });

    char fd_path[64];
    std::snprintf(fd_path, sizeof(fd_path), "/proc/self/fd/%d", fd);
    cxl_type3_atomic_t *ctx = nullptr;
    CHECK(cxl_type3_atomic_open_resource(fd_path, &ctx) == 0);

    std::uint64_t old = UINT64_MAX;
    CHECK(cxl_type3_atomic_auto_bind(ctx, memory, sizeof(memory), 0) == 0);
    old = cxl_type3_atomic_fetch_add_auto_u64(&memory[1], 11, __ATOMIC_SEQ_CST);
    CHECK(old == 0);
    CHECK(cxl_type3_atomic_auto_bind(nullptr, nullptr, 0, 0) == 0);

    CHECK(cxl_type3_atomic_fetch_add_u64(ctx, 0, 5, &old) == 0);
    CHECK(old == 0);
    CHECK(cxl_type3_atomic_fetch_add_u64(ctx, 0, 7, &old) == 0);
    CHECK(old == 5);
    CHECK(cxl_type3_atomic_compare_exchange_u64(ctx, 0, 12, 99, &old) == 0);
    CHECK(old == 12);
    CHECK(cxl_type3_atomic_compare_exchange_u64(ctx, 0, 12, 123, &old) == 0);
    CHECK(old == 99);
    CHECK(cxl_type3_atomic_fence(ctx) == 0);
    CHECK(cxl_type3_atomic_fetch_add_u64(ctx, 3, 1, &old) == -EINVAL);

    cxl_type3_atomic_t *ctx2 = nullptr;
    CHECK(cxl_type3_atomic_open_resource(fd_path, &ctx2) == 0);
    auto add_one_hundred = [](cxl_type3_atomic_t *worker_ctx) {
        for (int i = 0; i < 100; ++i) {
            std::uint64_t ignored;
            CHECK(cxl_type3_atomic_fetch_add_u64(worker_ctx, 0, 1, &ignored) == 0);
        }
    };
    std::thread worker1(add_one_hundred, ctx);
    std::thread worker2(add_one_hundred, ctx2);
    worker1.join();
    worker2.join();
    CHECK(cxl_type3_atomic_fetch_add_u64(ctx, 0, 0, &old) == 0);
    CHECK(old == 299);

    cxl_type3_atomic_close(ctx2);
    cxl_type3_atomic_close(ctx);
    stop.store(true, std::memory_order_release);
    device.join();
    munmap(bar, kBarSize);
    close(fd);
    return 0;
}
