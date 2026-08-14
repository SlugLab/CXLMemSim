#include "coherence_trace_v2.h"

#include <chrono>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>

namespace cxlmemsim {

namespace {

using protocol_v2::AckStrength;
using protocol_v2::Opcode;

std::string_view ackName(AckStrength value) noexcept {
    switch (value) {
    case AckStrength::NONE:
        return "NONE";
    case AckStrength::MODEL:
        return "MODEL";
    case AckStrength::NATIVE:
        return "NATIVE";
    }
    return "UNKNOWN";
}

std::uint64_t load(const std::atomic<std::uint64_t> &value) noexcept { return value.load(std::memory_order_relaxed); }

} // namespace

struct CoherenceTraceV2::Impl {
    explicit Impl(const std::filesystem::path &path) : output(path, std::ios::out | std::ios::trunc) {
        if (!output)
            throw std::runtime_error("cannot create coherence v2 trace: " + path.string());
    }

    std::mutex mutex;
    std::ofstream output;
};

CoherenceTraceV2::CoherenceTraceV2(const std::filesystem::path &path) : impl_(std::make_unique<Impl>(path)) {}

CoherenceTraceV2::~CoherenceTraceV2() = default;

void CoherenceTraceV2::record(const CoherenceTraceEvent &event) {
    const auto op = protocol_v2::opcode(event.frame);
    std::lock_guard lock(impl_->mutex);

    if (event.event == "registration" && protocol_v2::status(event.frame) == protocol_v2::Status::Ok) {
        counters_.registrations.fetch_add(1, std::memory_order_relaxed);
    } else if (event.event == "request") {
        switch (op) {
        case Opcode::Gets:
            counters_.gets.fetch_add(1, std::memory_order_relaxed);
            break;
        case Opcode::Getm:
            counters_.getm.fetch_add(1, std::memory_order_relaxed);
            break;
        case Opcode::Upgrade:
            counters_.upgrade.fetch_add(1, std::memory_order_relaxed);
            break;
        case Opcode::Puts:
            counters_.puts.fetch_add(1, std::memory_order_relaxed);
            break;
        case Opcode::Putm:
            counters_.putm.fetch_add(1, std::memory_order_relaxed);
            break;
        default:
            break;
        }
    } else if (event.event == "snoop_send") {
        switch (op) {
        case Opcode::SnpInv:
            counters_.snp_inv.fetch_add(1, std::memory_order_relaxed);
            break;
        case Opcode::SnpDowngrade:
            counters_.snp_downgrade.fetch_add(1, std::memory_order_relaxed);
            break;
        case Opcode::SnpDataInv:
            counters_.snp_data_inv.fetch_add(1, std::memory_order_relaxed);
            break;
        case Opcode::SnpDataDowngrade:
            counters_.snp_data_downgrade.fetch_add(1, std::memory_order_relaxed);
            break;
        case Opcode::HostFence:
            counters_.host_fence.fetch_add(1, std::memory_order_relaxed);
            break;
        default:
            break;
        }
    } else if (event.event == "snoop_ack") {
        if (protocol_v2::ackStrength(event.frame) == AckStrength::MODEL)
            counters_.model_acks.fetch_add(1, std::memory_order_relaxed);
        else if (protocol_v2::ackStrength(event.frame) == AckStrength::NATIVE)
            counters_.native_acks.fetch_add(1, std::memory_order_relaxed);
    } else if (event.event == "dirty_completion") {
        counters_.dirty_data_completions.fetch_add(1, std::memory_order_relaxed);
    } else if (event.event == "timeout") {
        counters_.timeouts.fetch_add(1, std::memory_order_relaxed);
    } else if (event.event == "protocol_error") {
        counters_.protocol_errors.fetch_add(1, std::memory_order_relaxed);
    } else if (event.event == "delivery_failure") {
        counters_.delivery_failures.fetch_add(1, std::memory_order_relaxed);
    } else if (event.event == "server_copy_failure") {
        counters_.server_copy_failures.fetch_add(1, std::memory_order_relaxed);
    }

    const auto now =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
    const auto line_address = protocol_v2::address(event.frame) & ~(std::uint64_t{protocol_v2::kLineSize - 1});
    impl_->output << "{\"schema_version\":1,\"event\":\"" << event.event << "\",\"monotonic_ns\":" << now
                  << ",\"opcode\":\"" << protocol_v2::toString(op)
                  << "\",\"src_host\":" << protocol_v2::srcHost(event.frame)
                  << ",\"dst_host\":" << protocol_v2::dstHost(event.frame)
                  << ",\"session_id\":" << protocol_v2::sessionId(event.frame)
                  << ",\"request_id\":" << protocol_v2::requestId(event.frame)
                  << ",\"snoop_id\":" << protocol_v2::snoopId(event.frame) << ",\"line_address\":" << line_address
                  << ",\"epoch\":" << protocol_v2::epoch(event.frame)
                  << ",\"payload_len\":" << protocol_v2::payloadLength(event.frame) << ",\"status\":\""
                  << protocol_v2::toString(protocol_v2::status(event.frame)) << "\",\"ack_strength\":\""
                  << ackName(protocol_v2::ackStrength(event.frame))
                  << "\",\"dirty_data\":" << (event.dirty_data ? "true" : "false") << "}\n";
    impl_->output.flush();
    if (!impl_->output)
        throw std::runtime_error("cannot write coherence v2 trace");
}

void CoherenceTraceV2::setActiveBindings(std::uint64_t count) noexcept {
    counters_.active_bindings.store(count, std::memory_order_relaxed);
}

CoherenceV2Snapshot CoherenceTraceV2::snapshot() const noexcept {
    return {load(counters_.registrations),
            load(counters_.gets),
            load(counters_.getm),
            load(counters_.upgrade),
            load(counters_.puts),
            load(counters_.putm),
            load(counters_.snp_inv),
            load(counters_.snp_downgrade),
            load(counters_.snp_data_inv),
            load(counters_.snp_data_downgrade),
            load(counters_.host_fence),
            load(counters_.model_acks),
            load(counters_.native_acks),
            load(counters_.dirty_data_completions),
            load(counters_.timeouts),
            load(counters_.protocol_errors),
            load(counters_.delivery_failures),
            load(counters_.server_copy_failures),
            load(counters_.active_bindings)};
}

std::string CoherenceTraceV2::snapshotJson() const {
    const auto value = snapshot();
    std::ostringstream output;
    output << "{\"registrations\":" << value.registrations << ",\"gets\":" << value.gets << ",\"getm\":" << value.getm
           << ",\"upgrade\":" << value.upgrade << ",\"puts\":" << value.puts << ",\"putm\":" << value.putm
           << ",\"snp_inv\":" << value.snp_inv << ",\"snp_downgrade\":" << value.snp_downgrade
           << ",\"snp_data_inv\":" << value.snp_data_inv << ",\"snp_data_downgrade\":" << value.snp_data_downgrade
           << ",\"host_fence\":" << value.host_fence << ",\"model_acks\":" << value.model_acks
           << ",\"native_acks\":" << value.native_acks << ",\"dirty_data_completions\":" << value.dirty_data_completions
           << ",\"timeouts\":" << value.timeouts << ",\"protocol_errors\":" << value.protocol_errors
           << ",\"delivery_failures\":" << value.delivery_failures
           << ",\"server_copy_failures\":" << value.server_copy_failures
           << ",\"active_bindings\":" << value.active_bindings << '}';
    return output.str();
}

} // namespace cxlmemsim
