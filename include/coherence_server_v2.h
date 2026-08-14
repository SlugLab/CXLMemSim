#pragma once

#include "coherence_transport.h"
#include "endpoint_session_registry.h"
#include "mesi_transaction_engine.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace cxlmemsim {

class CoherenceMemoryBackend;
class CoherenceTraceV2;

// Transport-neutral protocol-v2 service. Wire transports decode one complete
// frame, dispatch it here, and use the registered sender for unsolicited snoops.
class CoherenceServerV2 final : private CoherenceTransport {
public:
    using ConnectionId = std::uint64_t;

    struct DispatchResult {
        protocol_v2::Status status{protocol_v2::Status::InvalidState};
        std::optional<protocol_v2::CoherenceFrame> response;
        bool close_connection{};
        // A newly pinned ordinary response is synchronously published through
        // the connection sender. Transport loops must not send its mirror twice.
        bool response_via_sender{};
        // The response remains pinned for replay, but its sender failed. The
        // transport must fail and close this binding without a fallback send.
        bool delivery_failed{};
    };

    CoherenceServerV2(mesi_v2::MesiTransactionEngine &engine, EndpointSessionRegistry &registry,
                      CoherenceMemoryBackend &memory, std::chrono::milliseconds snoop_timeout,
                      std::shared_ptr<CoherenceTraceV2> trace = {});
    ~CoherenceServerV2() override;

    CoherenceServerV2(const CoherenceServerV2 &) = delete;
    CoherenceServerV2 &operator=(const CoherenceServerV2 &) = delete;

    // The sender is the single serialized outbound path for pinned responses
    // and unsolicited snoops on this duplex connection.
    bool attachConnection(ConnectionId connection, std::string transport_name, ResponseSender sender);
    bool detachConnection(ConnectionId connection);
    DispatchResult dispatch(ConnectionId connection, const protocol_v2::CoherenceFrame &frame);

private:
    bool sendToHost(std::uint16_t host_id, const protocol_v2::CoherenceFrame &frame) override;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cxlmemsim
