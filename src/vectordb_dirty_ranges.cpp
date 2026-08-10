#include "vectordb_dirty_ranges.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <stdexcept>

namespace cxlmemsim {
namespace {

constexpr std::uint64_t kCacheLineSize = 64;

} // namespace

DirtyRangeTracker::DirtyRangeTracker(std::uint64_t allocation_base, std::uint64_t allocation_size)
    : allocation_base_(allocation_base), allocation_size_(allocation_size) {
    const bool invalid_alignment = allocation_base % kCacheLineSize != 0 || allocation_size % kCacheLineSize != 0;
    const bool invalid_extent =
        allocation_size == 0 || allocation_base > std::numeric_limits<std::uint64_t>::max() - allocation_size;
    const std::uint64_t line_count = allocation_size / kCacheLineSize;
    if (invalid_alignment || invalid_extent || line_count > std::numeric_limits<std::size_t>::max()) {
        throw std::invalid_argument("dirty-range allocation must be a nonempty, aligned, non-overflowing range");
    }
    dirty_lines_.resize(static_cast<std::size_t>(line_count));
}

bool DirtyRangeTracker::mark(std::uint64_t address, std::uint64_t size) noexcept {
    if (!contains(address, size)) {
        return false;
    }

    const auto first_line = static_cast<std::size_t>((address - allocation_base_) / kCacheLineSize);
    const auto last_line = static_cast<std::size_t>((address + size - 1 - allocation_base_) / kCacheLineSize);
    std::fill(dirty_lines_.begin() + first_line, dirty_lines_.begin() + last_line + 1, true);
    return true;
}

std::vector<ByteRange> DirtyRangeTracker::coalesced() const {
    std::vector<ByteRange> ranges;
    std::size_t line = 0;
    while (line < dirty_lines_.size()) {
        if (!dirty_lines_[line]) {
            ++line;
            continue;
        }

        const std::size_t first_line = line;
        while (line < dirty_lines_.size() && dirty_lines_[line]) {
            ++line;
        }
        ranges.push_back(ByteRange{allocation_base_ + static_cast<std::uint64_t>(first_line) * kCacheLineSize,
                                   static_cast<std::uint64_t>(line - first_line) * kCacheLineSize});
    }
    return ranges;
}

std::uint64_t DirtyRangeTracker::bytesToCopy() const noexcept {
    return static_cast<std::uint64_t>(std::count(dirty_lines_.begin(), dirty_lines_.end(), true)) * kCacheLineSize;
}

bool DirtyRangeTracker::clearCompleted(ByteRange completed_range) noexcept {
    if (!contains(completed_range.address, completed_range.size) || completed_range.address % kCacheLineSize != 0 ||
        completed_range.size % kCacheLineSize != 0) {
        return false;
    }

    const auto first_line = static_cast<std::size_t>((completed_range.address - allocation_base_) / kCacheLineSize);
    const auto line_count = static_cast<std::size_t>(completed_range.size / kCacheLineSize);
    std::fill(dirty_lines_.begin() + first_line, dirty_lines_.begin() + first_line + line_count, false);
    return true;
}

void DirtyRangeTracker::clear() noexcept { std::fill(dirty_lines_.begin(), dirty_lines_.end(), false); }

bool DirtyRangeTracker::contains(std::uint64_t address, std::uint64_t size) const noexcept {
    if (size == 0 || address < allocation_base_ || address > std::numeric_limits<std::uint64_t>::max() - size) {
        return false;
    }
    return address + size <= allocation_base_ + allocation_size_;
}

} // namespace cxlmemsim
