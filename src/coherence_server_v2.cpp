#include "coherence_server_v2.h"

#include "coherence_memory_backend.h"
#include "coherence_trace_v2.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <map>
#include <mutex>
#include <span>
#include <unordered_map>
#include <utility>

namespace cxlmemsim {

namespace {

using mesi_v2::AckDisposition;
using mesi_v2::DirectorySnapshot;
using mesi_v2::MesiState;
using mesi_v2::TransactionRequest;
using mesi_v2::TransactionResult;
using protocol_v2::CoherenceFrame;
using protocol_v2::LineState;
using protocol_v2::Opcode;
using protocol_v2::Status;

bool sameFrame(const CoherenceFrame &left, const CoherenceFrame &right) noexcept {
    return protocol_v2::encodeFrame(left) == protocol_v2::encodeFrame(right);
}

LineState lineState(MesiState state) noexcept {
    switch (state) {
    case MesiState::I:
        return LineState::I;
    case MesiState::S:
        return LineState::S;
    case MesiState::E:
        return LineState::E;
    case MesiState::M:
        return LineState::M;
    }
    return LineState::I;
}

LineState lineStateForHost(const DirectorySnapshot &snapshot, std::uint16_t host_id) noexcept {
    if ((snapshot.state == MesiState::M || snapshot.state == MesiState::E) && snapshot.owner == host_id)
        return lineState(snapshot.state);
    if (snapshot.state == MesiState::S && host_id < 64 && (snapshot.sharers & (std::uint64_t{1} << host_id)) != 0)
        return LineState::S;
    return LineState::I;
}

CoherenceFrame responseFor(const CoherenceFrame &request, Status response_status) {
    auto response = protocol_v2::initializeFrame(Opcode::Response);
    protocol_v2::setStatus(response, response_status);
    protocol_v2::setSrcHost(response, protocol_v2::kServerHost);
    protocol_v2::setDstHost(response, protocol_v2::srcHost(request));
    protocol_v2::setRequestId(response, protocol_v2::requestId(request));
    protocol_v2::setSessionId(response, protocol_v2::sessionId(request));
    protocol_v2::setAddress(response, protocol_v2::address(request));
    protocol_v2::setLineState(response, protocol_v2::lineState(request));
    protocol_v2::setEpoch(response, protocol_v2::epoch(request));
    if (response_status == Status::StaleRequest)
        protocol_v2::setOldValue(response, protocol_v2::requestId(request) + 1);
    return response;
}

CoherenceFrame registrationResponse(const CoherenceFrame &request, const RegistrationResult &registration) {
    auto response = responseFor(request, registration.status);
    if (registration.status != Status::Ok)
        return response;
    protocol_v2::setSessionId(response, registration.session_id);
    protocol_v2::setCapabilities(response, registration.negotiated_capabilities);
    protocol_v2::setAckStrength(response, registration.ack_strength);
    protocol_v2::setSize(response, registration.line_size);
    protocol_v2::setValue(response, registration.cache_capacity);
    protocol_v2::setExpected(response, registration.cache_ways);
    protocol_v2::setOldValue(response, 1);
    return response;
}

CoherenceFrame transactionResponse(const CoherenceFrame &request, const TransactionResult &result) {
    auto response = responseFor(request, result.status);
    if (result.status != Status::Ok) {
        if (result.transition.committed()) {
            const auto directory_state = lineStateForHost(result.transition.snapshot, protocol_v2::srcHost(request));
            const auto reported_state = protocol_v2::lineState(request);
            protocol_v2::setLineState(response,
                                      static_cast<LineState>(std::min(static_cast<std::uint8_t>(directory_state),
                                                                      static_cast<std::uint8_t>(reported_state))));
            protocol_v2::setEpoch(response, result.transition.snapshot.epoch);
        }
        return response;
    }

    const auto op = protocol_v2::opcode(request);
    const auto response_state =
        op == Opcode::Puts || op == Opcode::Putm ? LineState::I : lineState(result.transition.snapshot.state);
    protocol_v2::setLineState(response, response_state);
    protocol_v2::setEpoch(response, result.transition.snapshot.epoch);
    const bool returns_data =
        op == Opcode::Gets || op == Opcode::Getm || op == Opcode::AtomicFaa || op == Opcode::AtomicCas;
    if (returns_data) {
        protocol_v2::setPayloadLength(response, protocol_v2::kLineSize);
        std::transform(result.data.begin(), result.data.end(), response.data.begin(),
                       [](std::byte value) { return static_cast<std::uint8_t>(value); });
    }
    if (op == Opcode::AtomicFaa || op == Opcode::AtomicCas)
        protocol_v2::setOldValue(response, result.old_value);
    return response;
}

Status admissionStatus(RequestAdmissionResult result) noexcept {
    switch (result) {
    case RequestAdmissionResult::Accepted:
    case RequestAdmissionResult::Duplicate:
        return Status::Ok;
    case RequestAdmissionResult::Conflict:
    case RequestAdmissionResult::StaleRequest:
        return Status::StaleRequest;
    case RequestAdmissionResult::Backpressure:
        return Status::IoError;
    case RequestAdmissionResult::InvalidRequest:
        return Status::InvalidState;
    case RequestAdmissionResult::SessionUnavailable:
        return Status::StaleSession;
    }
    return Status::InvalidState;
}

bool isHolderOperation(Opcode op) noexcept {
    return op == Opcode::Gets || op == Opcode::Getm || op == Opcode::Upgrade || op == Opcode::Puts ||
           op == Opcode::Putm || op == Opcode::AtomicFaa || op == Opcode::AtomicCas;
}

HolderPermission desiredPermission(Opcode op) noexcept {
    switch (op) {
    case Opcode::Gets:
        return HolderPermission::Clean;
    case Opcode::Getm:
    case Opcode::Upgrade:
    case Opcode::AtomicFaa:
    case Opcode::AtomicCas:
        return HolderPermission::Modified;
    case Opcode::Puts:
    case Opcode::Putm:
        return HolderPermission::None;
    default:
        return HolderPermission::None;
    }
}

std::uint64_t cleanHosts(const DirectorySnapshot &snapshot) noexcept {
    if (snapshot.state == MesiState::S)
        return snapshot.sharers;
    if (snapshot.state == MesiState::E && snapshot.owner)
        return std::uint64_t{1} << *snapshot.owner;
    return 0;
}

std::uint64_t modifiedHosts(const DirectorySnapshot &snapshot) noexcept {
    return snapshot.state == MesiState::M && snapshot.owner ? std::uint64_t{1} << *snapshot.owner : 0;
}

} // namespace

struct CoherenceServerV2::Impl {
    struct ReplayEntry {
        CoherenceFrame request;
        CoherenceFrame response;
    };

    struct Connection {
        std::string transport_name;
        ResponseSender sender;
        bool registering{};
        bool closed{};
        std::uint16_t host_id{protocol_v2::kServerHost};
        SessionId session_id{};
        BindingId binding_id{};
        std::map<std::uint64_t, ReplayEntry> replay;
    };

    struct HolderCommitContext {
        EndpointSessionRegistry *registry{};
        OperationAuthority *authority{};
        std::uint64_t line_address{};
        bool called{};
        bool reconciled{};

        static void install(void *opaque, const TransactionResult &result) noexcept {
            auto &context = *static_cast<HolderCommitContext *>(opaque);
            context.called = true;
            context.reconciled = context.registry->reconcileCommittedLine(*context.authority, context.line_address,
                                                                          cleanHosts(result.transition.snapshot),
                                                                          modifiedHosts(result.transition.snapshot));
        }
    };

    Impl(mesi_v2::MesiTransactionEngine &engine_value, EndpointSessionRegistry &registry_value,
         CoherenceMemoryBackend &memory_value, std::shared_ptr<CoherenceTraceV2> trace_value)
        : engine(engine_value), registry(registry_value), memory(memory_value), trace(std::move(trace_value)) {}

    void record(std::string_view event, const CoherenceFrame &frame, bool dirty_data = false) const {
        if (trace)
            trace->record({event, frame, dirty_data});
    }

    void updateActiveBindings() const {
        if (trace)
            trace->setActiveBindings(by_host.size());
    }

    void completeDirtyData(std::uint64_t line_address) {
        if (!trace)
            return;
        std::lock_guard lock(trace_pending_mutex);
        const auto found = std::find_if(pending_dirty_acks.begin(), pending_dirty_acks.end(), [&](const auto &item) {
            return (protocol_v2::address(item.second) & ~(std::uint64_t{protocol_v2::kLineSize - 1})) == line_address;
        });
        if (found == pending_dirty_acks.end())
            return;
        record("dirty_completion", found->second, true);
        pending_dirty_acks.erase(found);
    }

    void discardDirtyData(std::uint64_t line_address) {
        if (!trace)
            return;
        std::lock_guard lock(trace_pending_mutex);
        std::erase_if(pending_dirty_acks, [&](const auto &item) {
            return (protocol_v2::address(item.second) & ~(std::uint64_t{protocol_v2::kLineSize - 1})) == line_address;
        });
    }

    std::shared_ptr<Connection> connection(CoherenceServerV2::ConnectionId id) const {
        std::lock_guard lock(mutex);
        const auto found = connections.find(id);
        return found == connections.end() ? nullptr : found->second;
    }

    CoherenceServerV2::DispatchResult reject(const CoherenceFrame &request, Status status,
                                             bool close_connection = false) const {
        if (close_connection)
            return {status, std::nullopt, true};
        auto response = responseFor(request, status);
        if (status == Status::StaleRequest) {
            const auto floor = registry.replayFloor(protocol_v2::sessionId(request));
            protocol_v2::setOldValue(response, std::max(floor, protocol_v2::requestId(request) + 1));
        }
        return {status, response, false};
    }

    mesi_v2::MesiTransactionEngine &engine;
    EndpointSessionRegistry &registry;
    CoherenceMemoryBackend &memory;
    std::shared_ptr<CoherenceTraceV2> trace;
    mutable std::mutex mutex;
    std::mutex trace_pending_mutex;
    std::unordered_map<std::uint64_t, CoherenceFrame> pending_dirty_acks;
    std::unordered_map<CoherenceServerV2::ConnectionId, std::shared_ptr<Connection>> connections;
    std::unordered_map<std::uint16_t, std::weak_ptr<Connection>> by_host;
};

CoherenceServerV2::CoherenceServerV2(mesi_v2::MesiTransactionEngine &engine, EndpointSessionRegistry &registry,
                                     CoherenceMemoryBackend &memory, std::chrono::milliseconds snoop_timeout,
                                     std::shared_ptr<CoherenceTraceV2> trace)
    : impl_(std::make_unique<Impl>(engine, registry, memory, std::move(trace))) {
    engine.configure(memory, *this, snoop_timeout);
}

CoherenceServerV2::~CoherenceServerV2() = default;

bool CoherenceServerV2::attachConnection(ConnectionId connection, std::string transport_name, ResponseSender sender) {
    if (connection == 0 || transport_name.empty() || !sender)
        return false;
    auto state = std::make_shared<Impl::Connection>();
    state->transport_name = std::move(transport_name);
    state->sender = std::move(sender);
    std::lock_guard lock(impl_->mutex);
    return impl_->connections.emplace(connection, std::move(state)).second;
}

bool CoherenceServerV2::detachConnection(ConnectionId connection) {
    std::shared_ptr<Impl::Connection> state;
    {
        std::lock_guard lock(impl_->mutex);
        const auto found = impl_->connections.find(connection);
        if (found == impl_->connections.end())
            return false;
        state = found->second;
        state->closed = true;
        impl_->connections.erase(found);
        if (state->host_id != protocol_v2::kServerHost) {
            const auto host = impl_->by_host.find(state->host_id);
            if (host != impl_->by_host.end() && host->second.lock() == state)
                impl_->by_host.erase(host);
        }
        impl_->updateActiveBindings();
    }
    if (state->session_id != 0) {
        impl_->engine.notifyDisconnect(state->host_id, state->session_id);
        (void)impl_->registry.disconnectAbruptly(state->host_id, state->session_id, state->binding_id);
    }
    return true;
}

CoherenceServerV2::DispatchResult CoherenceServerV2::dispatch(ConnectionId connection_id, const CoherenceFrame &frame) {
    const auto connection = impl_->connection(connection_id);
    if (!connection)
        return {Status::StaleSession, std::nullopt, true};

    if (protocol_v2::magic(frame) != protocol_v2::kMagic ||
        protocol_v2::version(frame) != protocol_v2::kProtocolVersion) {
        impl_->record("protocol_error", frame);
        std::lock_guard lock(impl_->mutex);
        connection->closed = true;
        return {Status::BadProtocol, std::nullopt, true};
    }

    if (protocol_v2::opcode(frame) == Opcode::Register) {
        const auto validation = protocol_v2::validateFrame(frame);
        if (!validation) {
            impl_->record("protocol_error", frame);
            std::lock_guard lock(impl_->mutex);
            connection->closed = true;
            return {Status::BadProtocol, std::nullopt, true};
        }

        std::string transport_name;
        ResponseSender sender;
        RegistrationResult registration;
        try {
            {
                std::lock_guard lock(impl_->mutex);
                if (connection->closed || connection->registering || connection->session_id != 0)
                    return impl_->reject(frame, Status::InvalidState);
                connection->registering = true;
                transport_name = connection->transport_name;
                sender = connection->sender;
            }

            const RegistrationRequest request{protocol_v2::srcHost(frame),
                                              protocol_v2::sessionId(frame),
                                              protocol_v2::capabilities(frame),
                                              static_cast<std::uint32_t>(protocol_v2::value(frame)),
                                              static_cast<std::uint16_t>(protocol_v2::expected(frame)),
                                              std::move(transport_name),
                                              sender,
                                              protocol_v2::sessionId(frame) != 0};
            registration = impl_->registry.registerEndpoint(request);
        } catch (...) {
            bool connection_live = false;
            {
                std::lock_guard lock(impl_->mutex);
                connection->registering = false;
                const auto attached = impl_->connections.find(connection_id);
                connection_live =
                    attached != impl_->connections.end() && attached->second == connection && !connection->closed;
            }
            return connection_live ? impl_->reject(frame, Status::IoError)
                                   : DispatchResult{Status::StaleSession, std::nullopt, true};
        }
        bool bound = registration.status == Status::Ok &&
                     impl_->engine.bindSession(protocol_v2::srcHost(frame), registration.session_id);
        bool connection_live = false;
        {
            std::lock_guard lock(impl_->mutex);
            connection->registering = false;
            const auto attached = impl_->connections.find(connection_id);
            connection_live =
                attached != impl_->connections.end() && attached->second == connection && !connection->closed;
            if (bound && connection_live) {
                connection->host_id = protocol_v2::srcHost(frame);
                connection->session_id = registration.session_id;
                connection->binding_id = registration.binding_id;
                if (protocol_v2::sessionId(frame) == 0)
                    impl_->by_host[connection->host_id] = connection;
                impl_->updateActiveBindings();
            }
        }
        if (!connection_live) {
            if (registration.status == Status::Ok) {
                if (bound && protocol_v2::sessionId(frame) == 0) {
                    // A fresh REGISTER cannot have granted cache permissions before its response. Remove that
                    // unpublished generation completely so the host identity is immediately reusable.
                    const auto cleanup = impl_->engine.evictHost(impl_->registry, protocol_v2::srcHost(frame),
                                                                 registration.session_id, registration.binding_id,
                                                                 mesi_v2::HostFailurePolicy::AssertProcessStopped);
                    if (cleanup.status != mesi_v2::AdministrativeStatus::Ok) {
                        impl_->engine.notifyDisconnect(protocol_v2::srcHost(frame), registration.session_id);
                        (void)impl_->registry.disconnectAbruptly(protocol_v2::srcHost(frame), registration.session_id,
                                                                 registration.binding_id);
                    }
                } else {
                    if (bound)
                        impl_->engine.notifyDisconnect(protocol_v2::srcHost(frame), registration.session_id);
                    (void)impl_->registry.disconnectAbruptly(protocol_v2::srcHost(frame), registration.session_id,
                                                             registration.binding_id);
                }
            }
            return {Status::StaleSession, std::nullopt, true};
        }
        if (!bound && registration.status == Status::Ok) {
            (void)impl_->registry.disconnectAbruptly(protocol_v2::srcHost(frame), registration.session_id,
                                                     registration.binding_id);
            return impl_->reject(frame, Status::StaleSession);
        }
        const auto response = registrationResponse(frame, registration);
        auto registration_event = frame;
        protocol_v2::setStatus(registration_event, registration.status);
        protocol_v2::setSessionId(registration_event, registration.session_id);
        if (registration.status == Status::Ok && protocol_v2::sessionId(frame) != 0) {
            bool response_sent = false;
            try {
                response_sent = sender && sender(response);
            } catch (...) {
            }
            bool route_published = false;
            if (response_sent) {
                std::lock_guard lock(impl_->mutex);
                const auto attached = impl_->connections.find(connection_id);
                if (attached != impl_->connections.end() && attached->second == connection && !connection->closed &&
                    connection->host_id == protocol_v2::srcHost(frame) &&
                    connection->session_id == registration.session_id &&
                    connection->binding_id == registration.binding_id) {
                    impl_->by_host[connection->host_id] = connection;
                    impl_->updateActiveBindings();
                    route_published = true;
                }
            }
            if (!route_published ||
                !impl_->registry.publishPendingResponses(registration.session_id, registration.binding_id)) {
                {
                    std::lock_guard lock(impl_->mutex);
                    const auto host = impl_->by_host.find(protocol_v2::srcHost(frame));
                    if (host != impl_->by_host.end() && host->second.lock() == connection)
                        impl_->by_host.erase(host);
                    impl_->updateActiveBindings();
                }
                (void)impl_->registry.disconnectAbruptly(protocol_v2::srcHost(frame), registration.session_id,
                                                         registration.binding_id);
                return {Status::IoError, std::nullopt, true, true, true};
            }
            impl_->record("registration", registration_event);
            return {registration.status, response, false, true};
        }
        impl_->record("registration", registration_event);
        return {registration.status, response, false};
    }

    std::uint16_t host_id;
    SessionId session_id;
    BindingId binding_id;
    {
        std::lock_guard lock(impl_->mutex);
        if (connection->closed)
            return {Status::StaleSession, std::nullopt, true};
        if (connection->session_id == 0)
            return impl_->reject(frame, Status::ProtocolRequired);
        host_id = connection->host_id;
        session_id = connection->session_id;
        binding_id = connection->binding_id;
    }
    if (protocol_v2::srcHost(frame) != host_id || protocol_v2::sessionId(frame) != session_id)
        return impl_->reject(frame, Status::StaleSession);

    if (protocol_v2::opcode(frame) == Opcode::SnoopAck) {
        const bool dirty_candidate = protocol_v2::payloadLength(frame) == protocol_v2::kLineSize &&
                                     protocol_v2::ackStrength(frame) == protocol_v2::AckStrength::MODEL &&
                                     protocol_v2::status(frame) == Status::Ok;
        std::unique_lock trace_lock(impl_->trace_pending_mutex, std::defer_lock);
        if (dirty_candidate && impl_->trace) {
            trace_lock.lock();
            impl_->pending_dirty_acks[protocol_v2::snoopId(frame)] = frame;
        }
        const auto disposition = impl_->registry.controlFrameAdmissible(session_id, binding_id, frame)
                                     ? impl_->engine.handleControlFrame(impl_->registry, session_id, binding_id, frame)
                                     : impl_->engine.handleSnoopAck(frame);
        if (disposition == AckDisposition::Accepted || disposition == AckDisposition::Deferred) {
            impl_->record("snoop_ack", frame, dirty_candidate);
            return {Status::Ok, std::nullopt, false};
        }
        if (dirty_candidate && impl_->trace)
            impl_->pending_dirty_acks.erase(protocol_v2::snoopId(frame));
        if (disposition == AckDisposition::Duplicate)
            impl_->record("snoop_ack", frame, false);
        if (disposition == AckDisposition::Duplicate)
            return {Status::Ok, std::nullopt, false};
        impl_->record("protocol_error", frame);
        return {Status::BadProtocol, std::nullopt, false};
    }

    if (!protocol_v2::validateFrame(frame)) {
        impl_->record("protocol_error", frame);
        return impl_->reject(frame, Status::BadProtocol);
    }

    const auto replay_floor = impl_->registry.replayFloor(session_id);
    {
        std::lock_guard lock(impl_->mutex);
        connection->replay.erase(connection->replay.begin(), connection->replay.lower_bound(replay_floor));
        if (const auto replay = connection->replay.find(protocol_v2::requestId(frame));
            replay != connection->replay.end()) {
            if (!sameFrame(replay->second.request, frame))
                return impl_->reject(frame, Status::StaleRequest);
            return {protocol_v2::status(replay->second.response), replay->second.response, false};
        }
    }

    const auto op = protocol_v2::opcode(frame);
    const auto replayPinnedResponse = [&]() -> DispatchResult {
        if (!impl_->registry.waitForOperationsThrough(session_id, binding_id, protocol_v2::requestId(frame)))
            return impl_->reject(frame, Status::StaleSession);
        const auto response = impl_->registry.pinnedResponse(session_id, binding_id, frame);
        if (!response) {
            return impl_->reject(frame, protocol_v2::requestId(frame) < impl_->registry.replayFloor(session_id)
                                            ? Status::StaleRequest
                                            : Status::IoError);
        }
        return {protocol_v2::status(*response), *response, false};
    };
    if (isHolderOperation(op)) {
        auto admission = impl_->registry.admitOperation(session_id, binding_id, frame);
        if (admission.result == RequestAdmissionResult::Duplicate)
            return replayPinnedResponse();
        if (admission.result != RequestAdmissionResult::Accepted)
            return impl_->reject(frame, admissionStatus(admission.result));
        impl_->record("request", frame);
        auto authority = std::move(admission.authority);
        const auto line_address = protocol_v2::address(frame) & ~(std::uint64_t{protocol_v2::kLineSize - 1});
        const bool holder_reserved =
            impl_->registry.reserveHolderTransition(authority, line_address, desiredPermission(op));
        TransactionRequest request{host_id, session_id, protocol_v2::requestId(frame)};
        request.local_state = protocol_v2::lineState(frame);
        request.installed_epoch = protocol_v2::epoch(frame);
        Impl::HolderCommitContext holder_commit{&impl_->registry, &authority, line_address};
        request.commit_context = &holder_commit;
        request.commit_installer = &Impl::HolderCommitContext::install;

        TransactionResult result;
        if (!holder_reserved) {
            result.status = Status::IoError;
        } else
            switch (op) {
            case Opcode::Gets:
                result = impl_->engine.gets(protocol_v2::address(frame), request);
                break;
            case Opcode::Getm:
                result = impl_->engine.getm(protocol_v2::address(frame), request);
                break;
            case Opcode::Upgrade:
                result = impl_->engine.upgrade(protocol_v2::address(frame), request);
                break;
            case Opcode::Puts:
                result = impl_->engine.puts(protocol_v2::address(frame), request, protocol_v2::epoch(frame));
                break;
            case Opcode::Putm: {
                std::array<std::byte, protocol_v2::kLineSize> data{};
                std::transform(frame.data.begin(), frame.data.end(), data.begin(),
                               [](std::uint8_t value) { return static_cast<std::byte>(value); });
                result = impl_->engine.putm(protocol_v2::address(frame), request, protocol_v2::epoch(frame), data);
                break;
            }

            case Opcode::AtomicFaa:
                result = impl_->engine.fetchAdd(protocol_v2::address(frame), request, protocol_v2::value(frame));
                break;
            case Opcode::AtomicCas:
                result = impl_->engine.compareExchange(protocol_v2::address(frame), request,
                                                       protocol_v2::expected(frame), protocol_v2::value(frame));
                break;
            default:
                result.status = Status::InvalidState;
                break;
            }

        if (!holder_commit.called)
            impl_->registry.abortHolderTransition(authority);
        if (result.transition.committed() && !holder_commit.reconciled)
            result.status = Status::IoError;
        if (result.status == Status::Ok) {
            impl_->completeDirtyData(line_address);
        } else if (result.status == Status::CoherenceTimeout) {
            impl_->discardDirtyData(line_address);
            impl_->record("timeout", frame);
        } else if (result.status == Status::IoError) {
            impl_->discardDirtyData(line_address);
            impl_->record("server_copy_failure", frame);
        } else {
            impl_->discardDirtyData(line_address);
        }
        auto response = transactionResponse(frame, result);
        const auto pinned = impl_->registry.pinAndCompleteOperation(authority, frame, response);
        if (pinned == PinResponseResult::DeliveryFailed)
            return {Status::IoError, response, true, true, true};
        if (pinned != PinResponseResult::Pinned && pinned != PinResponseResult::Duplicate) {
            (void)impl_->registry.completeOperation(authority);
            return impl_->reject(frame, Status::IoError);
        }
        {
            std::lock_guard lock(impl_->mutex);
            connection->replay.emplace(protocol_v2::requestId(frame), Impl::ReplayEntry{frame, response});
        }
        return {result.status, response, false, true};
    }

    const auto admission = impl_->registry.admitRequest(session_id, binding_id, frame);
    if (admission == RequestAdmissionResult::Duplicate)
        return replayPinnedResponse();
    if (admission != RequestAdmissionResult::Accepted)
        return impl_->reject(frame, admissionStatus(admission));

    Status command_status = Status::InvalidState;
    switch (op) {
    case Opcode::Heartbeat:
        command_status = protocol_v2::oldValue(frame) == 0 || impl_->registry.acknowledgeResponses(
                                                                  session_id, binding_id, protocol_v2::oldValue(frame))
                             ? Status::Ok
                             : Status::InvalidState;
        if (command_status == Status::Ok && protocol_v2::oldValue(frame) != 0) {
            std::lock_guard lock(impl_->mutex);
            connection->replay.erase(connection->replay.begin(),
                                     connection->replay.upper_bound(protocol_v2::oldValue(frame)));
        }
        break;
    case Opcode::Fence:
        command_status = impl_->engine.fence(impl_->registry, session_id, binding_id, protocol_v2::requestId(frame));
        break;
    case Opcode::Unregister:
        command_status = impl_->engine.unregisterSession(impl_->registry, host_id, session_id, binding_id, frame);
        break;
    default:
        command_status = Status::InvalidState;
        break;
    }
    auto response = responseFor(frame, command_status);
    const auto pinned = impl_->registry.pinResponse(session_id, frame, response);
    if (pinned == PinResponseResult::DeliveryFailed)
        return {Status::IoError, response, true, true, true};
    if (pinned != PinResponseResult::Pinned && pinned != PinResponseResult::Duplicate)
        return impl_->reject(frame, Status::IoError);
    {
        std::lock_guard lock(impl_->mutex);
        connection->replay.emplace(protocol_v2::requestId(frame), Impl::ReplayEntry{frame, response});
    }
    return {command_status, response, false, true};
}

bool CoherenceServerV2::sendToHost(std::uint16_t host_id, const CoherenceFrame &frame) {
    try {
        impl_->record("snoop_send", frame);
        ResponseSender sender;
        {
            std::lock_guard lock(impl_->mutex);
            const auto found = impl_->by_host.find(host_id);
            if (found == impl_->by_host.end()) {
                impl_->record("delivery_failure", frame);
                return false;
            }
            const auto connection = found->second.lock();
            if (!connection || connection->closed || connection->session_id != protocol_v2::sessionId(frame)) {
                impl_->record("delivery_failure", frame);
                return false;
            }
            sender = connection->sender;
        }
        const bool sent = sender && sender(frame);
        if (!sent)
            impl_->record("delivery_failure", frame);
        return sent;
    } catch (...) {
        try {
            impl_->record("delivery_failure", frame);
        } catch (...) {
        }
        return false;
    }
}

} // namespace cxlmemsim
