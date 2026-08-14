#pragma once

#include "coherence_protocol_v2.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace cxlmemsim {

struct CoherenceV2Snapshot {
    std::uint64_t registrations{};
    std::uint64_t gets{};
    std::uint64_t getm{};
    std::uint64_t upgrade{};
    std::uint64_t puts{};
    std::uint64_t putm{};
    std::uint64_t snp_inv{};
    std::uint64_t snp_downgrade{};
    std::uint64_t snp_data_inv{};
    std::uint64_t snp_data_downgrade{};
    std::uint64_t host_fence{};
    std::uint64_t model_acks{};
    std::uint64_t native_acks{};
    std::uint64_t dirty_data_completions{};
    std::uint64_t timeouts{};
    std::uint64_t protocol_errors{};
    std::uint64_t delivery_failures{};
    std::uint64_t server_copy_failures{};
    std::uint64_t active_bindings{};
};

struct CoherenceV2Counters {
    std::atomic<std::uint64_t> registrations{};
    std::atomic<std::uint64_t> gets{};
    std::atomic<std::uint64_t> getm{};
    std::atomic<std::uint64_t> upgrade{};
    std::atomic<std::uint64_t> puts{};
    std::atomic<std::uint64_t> putm{};
    std::atomic<std::uint64_t> snp_inv{};
    std::atomic<std::uint64_t> snp_downgrade{};
    std::atomic<std::uint64_t> snp_data_inv{};
    std::atomic<std::uint64_t> snp_data_downgrade{};
    std::atomic<std::uint64_t> host_fence{};
    std::atomic<std::uint64_t> model_acks{};
    std::atomic<std::uint64_t> native_acks{};
    std::atomic<std::uint64_t> dirty_data_completions{};
    std::atomic<std::uint64_t> timeouts{};
    std::atomic<std::uint64_t> protocol_errors{};
    std::atomic<std::uint64_t> delivery_failures{};
    std::atomic<std::uint64_t> server_copy_failures{};
    std::atomic<std::uint64_t> active_bindings{};
};

struct CoherenceTraceEvent {
    std::string_view event;
    protocol_v2::CoherenceFrame frame;
    bool dirty_data{};
};

class CoherenceTraceV2 final {
public:
    explicit CoherenceTraceV2(const std::filesystem::path &path);
    ~CoherenceTraceV2();

    CoherenceTraceV2(const CoherenceTraceV2 &) = delete;
    CoherenceTraceV2 &operator=(const CoherenceTraceV2 &) = delete;

    void record(const CoherenceTraceEvent &event);
    void setActiveBindings(std::uint64_t count) noexcept;
    CoherenceV2Snapshot snapshot() const noexcept;
    std::string snapshotJson() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    CoherenceV2Counters counters_;
};

} // namespace cxlmemsim
