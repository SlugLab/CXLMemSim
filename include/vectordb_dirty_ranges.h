#pragma once

#include <cstdint>
#include <vector>

namespace cxlmemsim {

struct ByteRange {
    std::uint64_t address;
    std::uint64_t size;

    bool operator==(const ByteRange &) const = default;
};

class DirtyRangeTracker {
public:
    // The registered allocation must be nonempty and 64-byte aligned.
    DirtyRangeTracker(std::uint64_t allocation_base, std::uint64_t allocation_size);

    // Valid updates are rounded outward to all cache lines they touch.
    [[nodiscard]] bool mark(std::uint64_t address, std::uint64_t size) noexcept;
    [[nodiscard]] std::vector<ByteRange> coalesced() const;
    [[nodiscard]] std::uint64_t bytesToCopy() const noexcept;

    [[nodiscard]] bool clearCompleted(ByteRange completed_range) noexcept;
    void clear() noexcept;

private:
    [[nodiscard]] bool contains(std::uint64_t address, std::uint64_t size) const noexcept;

    std::uint64_t allocation_base_;
    std::uint64_t allocation_size_;
    std::vector<bool> dirty_lines_;
};

} // namespace cxlmemsim
