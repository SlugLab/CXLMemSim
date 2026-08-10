#include "vectordb_dirty_ranges.h"

#include <atomic>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace cxlmemsim;

namespace {

std::atomic<int> failures{};

#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            std::cerr << __func__ << ':' << __LINE__ << ": CHECK failed: " #condition << '\n';                         \
            failures.fetch_add(1, std::memory_order_relaxed);                                                          \
        }                                                                                                              \
    } while (false)

void testAdjacentMarksCoalesce() {
    DirtyRangeTracker tracker(64, 4096);

    CHECK(tracker.mark(64, 64));
    CHECK(tracker.mark(128, 128));
    CHECK((tracker.coalesced() == std::vector<ByteRange>{{64, 192}}));
    CHECK(tracker.bytesToCopy() == 192);
}

void testPartialMarksExpandOutwardToCacheLines() {
    DirtyRangeTracker tracker(0x1000, 256);

    CHECK(tracker.mark(0x103f, 2));
    CHECK((tracker.coalesced() == std::vector<ByteRange>{{0x1000, 128}}));
    CHECK(tracker.bytesToCopy() == 128);
}

void testDisjointMarksEnumerateInAddressOrder() {
    DirtyRangeTracker tracker(0x2000, 512);

    CHECK(tracker.mark(0x2180, 64));
    CHECK(tracker.mark(0x2040, 64));
    CHECK(tracker.coalesced() == (std::vector<ByteRange>{{0x2040, 64}, {0x2180, 64}}));
    CHECK(tracker.bytesToCopy() == 128);
}

void testInvalidMarksAreRejectedWithoutMutation() {
    DirtyRangeTracker tracker(0x1000, 256);
    CHECK(tracker.mark(0x1040, 64));
    const auto expected = tracker.coalesced();

    CHECK(!tracker.mark(0x1000, 0));
    CHECK(!tracker.mark(0x0fff, 1));
    CHECK(!tracker.mark(0x10ff, 2));
    CHECK(!tracker.mark(std::numeric_limits<std::uint64_t>::max() - 31, 64));
    CHECK(tracker.coalesced() == expected);
    CHECK(tracker.bytesToCopy() == 64);
}

void testCompletedRangesCanBeClearedIncrementally() {
    DirtyRangeTracker tracker(0x4000, 256);
    CHECK(tracker.mark(0x4000, 256));

    CHECK(tracker.clearCompleted(ByteRange{0x4040, 128}));
    CHECK(tracker.coalesced() == (std::vector<ByteRange>{{0x4000, 64}, {0x40c0, 64}}));
    CHECK(tracker.bytesToCopy() == 128);

    const auto expected = tracker.coalesced();
    CHECK(!tracker.clearCompleted(ByteRange{0x4001, 64}));
    CHECK(!tracker.clearCompleted(ByteRange{0x4000, 63}));
    CHECK(!tracker.clearCompleted(ByteRange{0x3fc0, 64}));
    CHECK(!tracker.clearCompleted(ByteRange{0x40c0, 128}));
    CHECK(!tracker.clearCompleted(ByteRange{std::numeric_limits<std::uint64_t>::max() - 31, 64}));
    CHECK(tracker.coalesced() == expected);
}

void testClearRemovesEveryDirtyRange() {
    DirtyRangeTracker tracker(0x8000, 256);
    CHECK(tracker.mark(0x8000, 64));
    CHECK(tracker.mark(0x80c0, 64));

    tracker.clear();
    CHECK(tracker.coalesced().empty());
    CHECK(tracker.bytesToCopy() == 0);
}

void testInvalidAllocationsAreRejected() {
    bool rejected_zero = false;
    bool rejected_unaligned_base = false;
    bool rejected_unaligned_size = false;
    bool rejected_overflow = false;

    try {
        DirtyRangeTracker tracker(0x1000, 0);
    } catch (const std::invalid_argument &) {
        rejected_zero = true;
    }
    try {
        DirtyRangeTracker tracker(0x1001, 64);
    } catch (const std::invalid_argument &) {
        rejected_unaligned_base = true;
    }
    try {
        DirtyRangeTracker tracker(0x1000, 65);
    } catch (const std::invalid_argument &) {
        rejected_unaligned_size = true;
    }
    try {
        DirtyRangeTracker tracker(std::numeric_limits<std::uint64_t>::max() - 63, 128);
    } catch (const std::invalid_argument &) {
        rejected_overflow = true;
    }

    CHECK(rejected_zero);
    CHECK(rejected_unaligned_base);
    CHECK(rejected_unaligned_size);
    CHECK(rejected_overflow);
}

} // namespace

int main() {
    testAdjacentMarksCoalesce();
    testPartialMarksExpandOutwardToCacheLines();
    testDisjointMarksEnumerateInAddressOrder();
    testInvalidMarksAreRejectedWithoutMutation();
    testCompletedRangesCanBeClearedIncrementally();
    testClearRemovesEveryDirtyRange();
    testInvalidAllocationsAreRejected();

    if (const auto count = failures.load(std::memory_order_relaxed); count != 0) {
        std::cerr << count << " dirty-range checks failed\n";
        return 1;
    }
    std::cout << "VectorDB dirty-range checks passed\n";
    return 0;
}
