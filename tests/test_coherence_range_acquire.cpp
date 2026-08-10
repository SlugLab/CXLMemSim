#include "coherence_endpoint_cache.h"
#include "coherence_memory_backend.h"
#include "mesi_directory.h"
#include "mesi_transaction_engine.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <span>
#include <stdexcept>

using namespace cxlmemsim;
using namespace cxlmemsim::mesi_v2;
using namespace cxlmemsim::protocol_v2;

namespace {

std::atomic<int> failures{};

#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            std::cerr << __func__ << ':' << __LINE__ << ": CHECK failed: " #condition << '\n';                         \
            failures.fetch_add(1, std::memory_order_relaxed);                                                          \
        }                                                                                                              \
    } while (false)

constexpr std::uint64_t kFirstLine = 0x1000;
constexpr std::uint64_t kSecondLine = kFirstLine + kLineSize;

class FailingMemory final : public CoherenceMemoryBackend {
public:
    std::array<std::byte, kLineSize> readLine(std::uint64_t address) override {
        std::lock_guard lock(mutex_);
        if (address == failed_read_)
            throw std::runtime_error("injected range read failure");
        return lines_[address];
    }

    void writeLine(std::uint64_t address, std::span<const std::byte, kLineSize> data) override {
        std::lock_guard lock(mutex_);
        std::copy(data.begin(), data.end(), lines_[address].begin());
    }

    void failRead(std::uint64_t address) {
        std::lock_guard lock(mutex_);
        failed_read_ = address;
    }

private:
    std::mutex mutex_;
    std::map<std::uint64_t, std::array<std::byte, kLineSize>> lines_;
    std::uint64_t failed_read_{std::numeric_limits<std::uint64_t>::max()};
};

class RangeHarness {
public:
    RangeHarness()
        : engine(directory, memory, transport), endpoint(engine, {0, 101, 8, EndpointWritePolicy::WriteBack}) {
        transport.bindEngine(engine);
        CHECK(engine.bindSession(endpoint.endpointId(), endpoint.sessionId()));
        CHECK(transport.registerEndpoint(endpoint));
    }

    FailingMemory memory;
    MesiDirectory directory;
    InProcessCoherenceTransport transport;
    MesiTransactionEngine engine;
    CoherenceEndpointCache endpoint;
};

void testReadRangeGrantsEveryLine() {
    RangeHarness harness;
    const auto result = harness.endpoint.acquireRange(kFirstLine, 2 * kLineSize, RangeIntent::Read);

    CHECK(result.status == Status::Ok);
    CHECK(result.lines_requested == 2);
    CHECK(result.lines_granted == 2);
    CHECK(result.complete);
    CHECK(harness.endpoint.contains(kFirstLine));
    CHECK(harness.endpoint.contains(kSecondLine));
    CHECK(harness.endpoint.counters().gets == 2);
    CHECK(harness.endpoint.counters().loads == 0);
}

void testWriteRangeObtainsExclusivePermissionWithoutStore() {
    RangeHarness harness;
    const auto result = harness.endpoint.acquireRange(kFirstLine, 2 * kLineSize, RangeIntent::Write);

    CHECK(result.status == Status::Ok);
    CHECK(result.lines_requested == 2);
    CHECK(result.lines_granted == 2);
    CHECK(result.complete);
    CHECK(harness.endpoint.counters().getm == 2);
    CHECK(harness.endpoint.counters().stores == 0);
}

void testWriteRangeExplicitlyUpgradesReadPermission() {
    RangeHarness harness;
    CHECK(harness.endpoint.acquireRange(kFirstLine, 2 * kLineSize, RangeIntent::Read).complete);

    const auto result = harness.endpoint.acquireRange(kFirstLine, 2 * kLineSize, RangeIntent::Write);
    CHECK(result.status == Status::Ok);
    CHECK(result.lines_requested == 2);
    CHECK(result.lines_granted == 2);
    CHECK(result.complete);
    CHECK(harness.endpoint.counters().upgrades == 2);
    CHECK(harness.endpoint.counters().getm == 0);
    CHECK(harness.endpoint.counters().stores == 0);
}

void testUnalignedRangeExpandsToCoveredLines() {
    RangeHarness harness;
    const auto result = harness.endpoint.acquireRange(kFirstLine + 31, 66, RangeIntent::Read);

    CHECK(result.status == Status::Ok);
    CHECK(result.lines_requested == 2);
    CHECK(result.lines_granted == 2);
    CHECK(result.complete);
    CHECK(harness.endpoint.contains(kFirstLine));
    CHECK(harness.endpoint.contains(kSecondLine));
}

void testZeroSizeIsCompleteNoOp() {
    RangeHarness harness;
    const auto result = harness.endpoint.acquireRange(kFirstLine + 7, 0, RangeIntent::Read);

    CHECK(result.status == Status::Ok);
    CHECK(result.lines_requested == 0);
    CHECK(result.lines_granted == 0);
    CHECK(result.complete);
    CHECK(harness.endpoint.size() == 0);
}

void testOverflowFailsBeforeAnyGrant() {
    RangeHarness harness;
    const auto result =
        harness.endpoint.acquireRange(std::numeric_limits<std::uint64_t>::max() - 31, 64, RangeIntent::Read);

    CHECK(result.status == Status::InvalidState);
    CHECK(result.lines_requested == 0);
    CHECK(result.lines_granted == 0);
    CHECK(!result.complete);
    CHECK(harness.endpoint.size() == 0);
}

void testPartialGrantFailsClosed() {
    RangeHarness harness;
    harness.memory.failRead(kSecondLine);
    const auto result = harness.endpoint.acquireRange(kFirstLine, 2 * kLineSize, RangeIntent::Read);

    CHECK(result.status == Status::IoError);
    CHECK(result.lines_requested == 2);
    CHECK(result.lines_granted == 1);
    CHECK(!result.complete);
    CHECK(harness.endpoint.contains(kFirstLine));
    CHECK(!harness.endpoint.contains(kSecondLine));
}

} // namespace

int main() {
    testReadRangeGrantsEveryLine();
    testWriteRangeObtainsExclusivePermissionWithoutStore();
    testWriteRangeExplicitlyUpgradesReadPermission();
    testUnalignedRangeExpandsToCoveredLines();
    testZeroSizeIsCompleteNoOp();
    testOverflowFailsBeforeAnyGrant();
    testPartialGrantFailsClosed();

    if (const auto count = failures.load(std::memory_order_relaxed); count != 0) {
        std::cerr << count << " range-acquire checks failed\n";
        return 1;
    }
    std::cout << "coherence range-acquire checks passed\n";
    return 0;
}
