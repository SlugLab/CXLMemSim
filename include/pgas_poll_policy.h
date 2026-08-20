#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

enum class PgasPollAction {
    Spin,
    Yield,
    Sleep,
};

struct PgasPollConfig {
    std::size_t workers = 4;
    std::uint64_t spin_us = 50;
    std::size_t yield_count = 10;
    std::uint64_t idle_sleep_us = 100;
};

inline bool isValidPgasPollConfig(const PgasPollConfig &config, std::size_t max_workers) noexcept {
    return config.workers > 0 && config.workers <= max_workers &&
           config.spin_us <= std::numeric_limits<std::uint64_t>::max() / 1000;
}

class PgasPollPolicy {
public:
    PgasPollPolicy(std::uint64_t active_spin_ns, std::size_t yield_count) noexcept
        : active_spin_ns_(active_spin_ns), max_yields_(yield_count) {}

    PgasPollAction next(bool processed, std::uint64_t now_ns) noexcept {
        if (processed) {
            last_active_ns_ = now_ns;
            idle_yields_ = 0;
            return PgasPollAction::Spin;
        }

        if (last_active_ns_ != 0 && (now_ns < last_active_ns_ || now_ns - last_active_ns_ < active_spin_ns_)) {
            return PgasPollAction::Spin;
        }

        if (idle_yields_ < max_yields_) {
            ++idle_yields_;
            return PgasPollAction::Yield;
        }

        return PgasPollAction::Sleep;
    }

private:
    std::uint64_t active_spin_ns_;
    std::size_t max_yields_;
    std::uint64_t last_active_ns_ = 0;
    std::size_t idle_yields_ = 0;
};
