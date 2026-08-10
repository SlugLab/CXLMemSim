#pragma once

#include "coherence_transport.h"
#include "mesi_transaction_engine.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace cxlmemsim {

enum class EndpointWritePolicy : std::uint8_t { WriteBack, WriteThrough };
enum class RangeIntent : std::uint8_t { Read, Write };

struct RangeAcquireResult {
    protocol_v2::Status status{protocol_v2::Status::InvalidState};
    std::size_t lines_requested{};
    std::size_t lines_granted{};
    bool complete{};
};

struct CoherenceEndpointConfig {
    std::uint16_t endpoint_id{};
    std::uint64_t session_id{};
    std::size_t capacity_lines{};
    EndpointWritePolicy write_policy{EndpointWritePolicy::WriteBack};
};

struct CoherenceEndpointCounters {
    std::uint64_t loads{};
    std::uint64_t stores{};
    std::uint64_t hits{};
    std::uint64_t misses{};
    std::uint64_t gets{};
    std::uint64_t getm{};
    std::uint64_t upgrades{};
    std::uint64_t puts{};
    std::uint64_t putm{};
    std::uint64_t atomics{};
    std::uint64_t fetch_adds{};
    std::uint64_t compare_exchanges{};
    std::uint64_t evictions{};
    std::uint64_t clean_evictions{};
    std::uint64_t dirty_evictions{};
    std::uint64_t writebacks{};
    std::uint64_t snoop_inv{};
    std::uint64_t snoop_downgrade{};
    std::uint64_t snoop_data_inv{};
    std::uint64_t snoop_data_downgrade{};
    std::uint64_t snoop_acks{};
    std::uint64_t rejected_snoops{};
};

class CoherenceEndpointCache;

// Optional-like handle for a completed model snoop. The cache effect is complete
// before this handle is returned; retained completion state permits ACK replay.
class PreparedSnoopAck {
public:
    PreparedSnoopAck() = default;
    PreparedSnoopAck(const PreparedSnoopAck &) = delete;
    PreparedSnoopAck &operator=(const PreparedSnoopAck &) = delete;
    PreparedSnoopAck(PreparedSnoopAck &&other) noexcept;
    PreparedSnoopAck &operator=(PreparedSnoopAck &&other) noexcept;
    ~PreparedSnoopAck();

    explicit operator bool() const noexcept;
    bool has_value() const noexcept;
    const protocol_v2::CoherenceFrame &operator*() const;
    const protocol_v2::CoherenceFrame *operator->() const;

private:
    friend class CoherenceEndpointCache;

    PreparedSnoopAck(CoherenceEndpointCache &endpoint, std::uint64_t line_address, std::uint64_t token,
                     protocol_v2::CoherenceFrame ack) noexcept;
    void cancel() noexcept;

    mutable CoherenceEndpointCache *endpoint_{};
    std::uint64_t line_address_{};
    std::uint64_t token_{};
    mutable protocol_v2::CoherenceFrame ack_{};
    mutable bool committed_{};
};

class CoherenceEndpointCache {
public:
    CoherenceEndpointCache(mesi_v2::MesiTransactionEngine &engine, CoherenceEndpointConfig config);
    ~CoherenceEndpointCache();

    CoherenceEndpointCache(const CoherenceEndpointCache &) = delete;
    CoherenceEndpointCache &operator=(const CoherenceEndpointCache &) = delete;

    protocol_v2::Status load(std::uint64_t address, std::span<std::byte> destination);
    protocol_v2::Status store(std::uint64_t address, std::span<const std::byte> source);
    RangeAcquireResult acquireRange(std::uint64_t address, std::size_t size, RangeIntent intent);
    mesi_v2::TransactionResult fetchAdd(std::uint64_t address, std::uint64_t value);
    mesi_v2::TransactionResult compareExchange(std::uint64_t address, std::uint64_t expected, std::uint64_t desired);

    PreparedSnoopAck processSnoop(const protocol_v2::CoherenceFrame &snoop);

    std::uint16_t endpointId() const noexcept;
    std::uint64_t sessionId() const noexcept;
    bool contains(std::uint64_t address) const;
    bool fenced() const;
    std::size_t size() const;
    CoherenceEndpointCounters counters() const;

private:
    friend class PreparedSnoopAck;
    struct Impl;

    bool commitPreparedSnoop(std::uint64_t line_address, std::uint64_t token,
                             const protocol_v2::CoherenceFrame &ack) noexcept;
    void cancelPreparedSnoop(std::uint64_t line_address, std::uint64_t token) noexcept;
    protocol_v2::Status acquireLine(std::uint64_t line_address, RangeIntent intent);
    protocol_v2::Status ensureCapacity(std::uint64_t incoming_line);
    protocol_v2::Status writeThrough(std::uint64_t line_address);

    std::unique_ptr<Impl> impl_;
};

struct InProcessCoherenceTransportCounters {
    std::uint64_t snoops{};
    std::uint64_t snoop_acks{};
    std::uint64_t send_failures{};
};

class InProcessCoherenceTransport final : public CoherenceTransport {
public:
    InProcessCoherenceTransport();
    ~InProcessCoherenceTransport() override;

    InProcessCoherenceTransport(const InProcessCoherenceTransport &) = delete;
    InProcessCoherenceTransport &operator=(const InProcessCoherenceTransport &) = delete;

    void bindEngine(mesi_v2::MesiTransactionEngine &engine);
    bool registerEndpoint(CoherenceEndpointCache &endpoint);
    bool sendToHost(std::uint16_t host_id, const protocol_v2::CoherenceFrame &frame) override;
    InProcessCoherenceTransportCounters counters() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cxlmemsim
