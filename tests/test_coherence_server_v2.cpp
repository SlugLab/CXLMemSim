#include "coherence_server_v2.h"

#include "coherence_memory_backend.h"
#include "coherence_protocol_v2.h"
#include "coherence_trace_v2.h"
#include "endpoint_session_registry.h"
#include "mesi_directory.h"
#include "mesi_transaction_engine.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

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

constexpr std::uint64_t kModelSnoop = static_cast<std::uint64_t>(Capability::MODEL_SNOOP);
constexpr std::uint64_t kNativeFlush = static_cast<std::uint64_t>(Capability::NATIVE_FLUSH);
constexpr std::uint64_t kLineA = 0x1000;
constexpr std::uint64_t kLineB = 0x2000;
constexpr std::uint64_t kLineC = 0x3000;
constexpr std::uint64_t kLineD = 0x4000;
constexpr auto kSnoopTimeout = std::chrono::milliseconds(30);

template <typename Tag, typename Tag::Type member> struct PrivateMemberAccess {
    friend typename Tag::Type privateMember(Tag) { return member; }
};

struct SendToHostTag {
    using Type = bool (CoherenceServerV2::*)(std::uint16_t, const CoherenceFrame &);
    friend Type privateMember(SendToHostTag);
};

template struct PrivateMemberAccess<SendToHostTag, &CoherenceServerV2::sendToHost>;

std::uint64_t holder(std::uint16_t host_id) { return std::uint64_t{1} << host_id; }

std::array<std::byte, kLineSize> lineBytes(std::uint8_t value) {
    std::array<std::byte, kLineSize> line{};
    line.fill(static_cast<std::byte>(value));
    return line;
}

std::array<std::uint8_t, kLineSize> wireBytes(const std::array<std::byte, kLineSize> &line) {
    std::array<std::uint8_t, kLineSize> wire{};
    std::transform(line.begin(), line.end(), wire.begin(),
                   [](std::byte value) { return static_cast<std::uint8_t>(value); });
    return wire;
}

std::uint64_t scalar(const CoherenceFrame &frame, std::size_t offset = 0) {
    std::uint64_t result{};
    std::memcpy(&result, frame.data.data() + offset, sizeof(result));
    return result;
}

class FakeMemory final : public CoherenceMemoryBackend {
public:
    std::array<std::byte, kLineSize> readLine(std::uint64_t address) override {
        std::unique_lock lock(mutex_);
        ++read_count_;
        if (block_reads_) {
            read_entered_ = true;
            changed_.notify_all();
            changed_.wait(lock, [&] { return !block_reads_; });
        }
        return lines_[address];
    }

    void writeLine(std::uint64_t address, std::span<const std::byte, kLineSize> data) override {
        std::lock_guard lock(mutex_);
        ++write_count_;
        if (fail_next_write_) {
            fail_next_write_ = false;
            throw std::runtime_error("injected coherence write failure");
        }
        std::copy(data.begin(), data.end(), lines_[address].begin());
    }

    void failNextWrite() {
        std::lock_guard lock(mutex_);
        fail_next_write_ = true;
    }

    void blockReads() {
        std::lock_guard lock(mutex_);
        block_reads_ = true;
        read_entered_ = false;
    }

    void waitForRead() {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [&] { return read_entered_; });
    }

    void releaseReads() {
        std::lock_guard lock(mutex_);
        block_reads_ = false;
        changed_.notify_all();
    }

    void store(std::uint64_t address, const std::array<std::byte, kLineSize> &data) {
        std::lock_guard lock(mutex_);
        lines_[address] = data;
    }

    void storeScalar(std::uint64_t address, std::uint64_t value) {
        auto line = lineBytes(0);
        std::memcpy(line.data() + (address & (kLineSize - 1)), &value, sizeof(value));
        store(address & ~(kLineSize - 1), line);
    }

    std::array<std::byte, kLineSize> inspect(std::uint64_t address) const {
        std::lock_guard lock(mutex_);
        const auto found = lines_.find(address);
        return found == lines_.end() ? std::array<std::byte, kLineSize>{} : found->second;
    }

    std::size_t readCount() const {
        std::lock_guard lock(mutex_);
        return read_count_;
    }

    std::size_t writeCount() const {
        std::lock_guard lock(mutex_);
        return write_count_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::map<std::uint64_t, std::array<std::byte, kLineSize>> lines_;
    std::size_t read_count_{};
    std::size_t write_count_{};
    bool fail_next_write_{};
    bool block_reads_{};
    bool read_entered_{};
};

struct Harness {
    // Required transport-neutral service surface: inject the existing engine, registry, memory backend and timeout;
    // attach an opaque connection plus its unsolicited-frame sender; dispatch already-decoded protocol-v2 frames.
    MesiDirectory directory;
    EndpointSessionRegistry registry;
    FakeMemory memory;
    MesiTransactionEngine engine{directory};
    std::shared_ptr<CoherenceTraceV2> trace;
    CoherenceServerV2 server;

    explicit Harness(std::shared_ptr<CoherenceTraceV2> trace_value = {})
        : trace(std::move(trace_value)), server(engine, registry, memory, kSnoopTimeout, trace) {}
};

using ConnectionId = CoherenceServerV2::ConnectionId;
using DispatchResult = CoherenceServerV2::DispatchResult;

const CoherenceFrame &requireResponse(const DispatchResult &result, const char *context) {
    static const CoherenceFrame empty{};
    if (!result.response) {
        std::cerr << context << ": response missing\n";
        failures.fetch_add(1, std::memory_order_relaxed);
        return empty;
    }
    return *result.response;
}

CoherenceFrame registration(std::uint16_t host_id, SessionId requested_session = 0,
                            std::uint64_t capabilities_value = kModelSnoop | kNativeFlush,
                            std::uint32_t cache_capacity = 256 * 1024, std::uint16_t cache_ways = 4) {
    auto frame = initializeFrame(Opcode::Register);
    setSrcHost(frame, host_id);
    setDstHost(frame, kServerHost);
    setSessionId(frame, requested_session);
    setCapabilities(frame, capabilities_value);
    setSize(frame, kLineSize);
    setValue(frame, cache_capacity);
    setExpected(frame, cache_ways);
    return frame;
}

CoherenceFrame command(Opcode opcode_value, std::uint16_t host_id, SessionId session_id, std::uint64_t request_id,
                       std::uint64_t address = 0, LineState state = LineState::I, std::uint64_t installed_epoch = 0) {
    auto frame = initializeFrame(opcode_value);
    setSrcHost(frame, host_id);
    setDstHost(frame, kServerHost);
    setSessionId(frame, session_id);
    setRequestId(frame, request_id);
    setAddress(frame, address);
    setLineState(frame, state);
    setEpoch(frame, installed_epoch);
    if (opcode_value == Opcode::AtomicFaa || opcode_value == Opcode::AtomicCas)
        setSize(frame, sizeof(std::uint64_t));
    return frame;
}

CoherenceFrame putm(std::uint16_t host_id, SessionId session_id, std::uint64_t request_id, std::uint64_t address,
                    std::uint64_t installed_epoch, const std::array<std::byte, kLineSize> &data) {
    auto frame = command(Opcode::Putm, host_id, session_id, request_id, address, LineState::M, installed_epoch);
    setPayloadLength(frame, kLineSize);
    frame.data = wireBytes(data);
    return frame;
}

CoherenceFrame faa(std::uint16_t host_id, SessionId session_id, std::uint64_t request_id, std::uint64_t address,
                   LineState state, std::uint64_t installed_epoch, std::uint64_t addend) {
    auto frame = command(Opcode::AtomicFaa, host_id, session_id, request_id, address, state, installed_epoch);
    setValue(frame, addend);
    return frame;
}

CoherenceFrame cas(std::uint16_t host_id, SessionId session_id, std::uint64_t request_id, std::uint64_t address,
                   LineState state, std::uint64_t installed_epoch, std::uint64_t expected_value,
                   std::uint64_t desired) {
    auto frame = command(Opcode::AtomicCas, host_id, session_id, request_id, address, state, installed_epoch);
    setExpected(frame, expected_value);
    setValue(frame, desired);
    return frame;
}

bool isSnoop(const CoherenceFrame &frame) {
    const auto op = opcode(frame);
    return op == Opcode::SnpInv || op == Opcode::SnpDowngrade || op == Opcode::SnpDataInv ||
           op == Opcode::SnpDataDowngrade || op == Opcode::HostFence;
}

CoherenceFrame ackFor(const CoherenceFrame &snoop, Status ack_status = Status::Ok) {
    auto ack = initializeFrame(Opcode::SnoopAck);
    setSrcHost(ack, dstHost(snoop));
    setDstHost(ack, kServerHost);
    setSessionId(ack, sessionId(snoop));
    setSnoopId(ack, snoopId(snoop));
    setAddress(ack, address(snoop));
    setEpoch(ack, epoch(snoop));
    setStatus(ack, ack_status);
    setAckStrength(ack, AckStrength::MODEL);
    const auto op = opcode(snoop);
    const bool downgrade = op == Opcode::SnpDowngrade || op == Opcode::SnpDataDowngrade;
    setLineState(ack, downgrade ? LineState::S : LineState::I);
    if (ack_status == Status::Ok && (op == Opcode::SnpDataInv || op == Opcode::SnpDataDowngrade))
        setPayloadLength(ack, kLineSize);
    return ack;
}

enum class AckMode { Good, CorruptEpoch, Drop };

struct EndpointObserver {
    struct CachedLine {
        std::array<std::uint8_t, kLineSize> data{};
        LineState state{LineState::I};
        std::uint64_t epoch{};
    };

    Harness *harness{};
    ConnectionId connection{};
    AckMode ack_mode{AckMode::Good};
    std::vector<CoherenceFrame> snoops;
    std::vector<Status> ack_dispatch_statuses;
    std::map<std::uint64_t, CachedLine> cache;
    bool saw_nonterminal_response{};

    bool receive(const CoherenceFrame &frame) {
        if (opcode(frame) == Opcode::Response) {
            if (requestId(frame) != 0 &&
                harness->registry.operationCompletionWatermark(sessionId(frame)) < requestId(frame))
                saw_nonterminal_response = true;
            if (status(frame) == Status::Ok) {
                if (lineState(frame) == LineState::I) {
                    cache.erase(address(frame));
                } else {
                    auto &line = cache[address(frame)];
                    if (payloadLength(frame) == kLineSize)
                        line.data = frame.data;
                    line.state = lineState(frame);
                    line.epoch = epoch(frame);
                }
            }
            return true;
        }
        if (!isSnoop(frame))
            return true;
        snoops.push_back(frame);
        if (ack_mode == AckMode::Drop)
            return true;
        auto ack = ackFor(frame);
        const auto found = cache.find(address(frame));
        if (payloadLength(ack) == kLineSize && found != cache.end())
            ack.data = found->second.data;
        if (lineState(ack) == LineState::I) {
            cache.erase(address(frame));
        } else if (found != cache.end()) {
            found->second.state = lineState(ack);
            found->second.epoch = epoch(ack);
        }
        if (ack_mode == AckMode::CorruptEpoch)
            setEpoch(ack, epoch(ack) + 1);
        const auto result = harness->server.dispatch(connection, ack);
        ack_dispatch_statuses.push_back(result.status);
        CHECK(!result.response.has_value());
        return true;
    }
};

struct Peer {
    ConnectionId connection{};
    std::uint16_t host{};
    SessionId session{};
};

class SenderCopyGate {
public:
    void arm() {
        std::lock_guard lock(mutex_);
        copies_ = 0;
        blocked_ = false;
        released_ = false;
        armed_ = true;
    }

    void copied() {
        std::unique_lock lock(mutex_);
        if (!armed_ || ++copies_ != 2)
            return;
        blocked_ = true;
        changed_.notify_all();
        changed_.wait(lock, [&] { return released_; });
    }

    bool waitUntilBlocked() {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, std::chrono::seconds(1), [&] { return blocked_; });
    }

    void release() {
        std::lock_guard lock(mutex_);
        released_ = true;
        changed_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::size_t copies_{};
    bool armed_{};
    bool blocked_{};
    bool released_{};
};

struct GatedSender {
    SenderCopyGate *gate{};
    std::atomic<int> *deliveries{};

    GatedSender() = default;
    GatedSender(SenderCopyGate &copy_gate, std::atomic<int> &delivery_count)
        : gate(&copy_gate), deliveries(&delivery_count) {}
    GatedSender(const GatedSender &other) : gate(other.gate), deliveries(other.deliveries) { gate->copied(); }
    GatedSender(GatedSender &&) noexcept = default;
    GatedSender &operator=(const GatedSender &) = default;
    GatedSender &operator=(GatedSender &&) noexcept = default;

    bool operator()(const CoherenceFrame &) const {
        deliveries->fetch_add(1, std::memory_order_relaxed);
        return true;
    }
};

struct ThrowOnNthCopySender {
    std::atomic<std::size_t> *copies{};
    std::size_t throw_on_copy{};

    ThrowOnNthCopySender(std::atomic<std::size_t> &copy_count, std::size_t throw_on)
        : copies(&copy_count), throw_on_copy(throw_on) {}
    ThrowOnNthCopySender(const ThrowOnNthCopySender &other) : copies(other.copies), throw_on_copy(other.throw_on_copy) {
        if (copies->fetch_add(1, std::memory_order_relaxed) + 1 == throw_on_copy)
            throw std::runtime_error("sender copy failed");
    }
    ThrowOnNthCopySender(ThrowOnNthCopySender &&) noexcept = default;

    bool operator()(const CoherenceFrame &) const { return true; }
};

Peer registerPeer(Harness &harness, ConnectionId connection, std::uint16_t host, EndpointObserver &observer,
                  const char *transport_name = "test", std::uint32_t cache_capacity = 256 * 1024,
                  std::uint16_t cache_ways = 4) {
    observer.harness = &harness;
    observer.connection = connection;
    CHECK(harness.server.attachConnection(
        connection, transport_name, [&observer](const CoherenceFrame &frame) { return observer.receive(frame); }));
    const auto request = registration(host, 0, kModelSnoop | kNativeFlush, cache_capacity, cache_ways);
    const auto result = harness.server.dispatch(connection, request);
    CHECK(result.status == Status::Ok);
    CHECK(!result.close_connection);
    const auto &response = requireResponse(result, "REGISTER");
    CHECK(validateResponse(response, request));
    CHECK(capabilities(response) == kModelSnoop);
    CHECK(ackStrength(response) == AckStrength::MODEL);
    CHECK(sessionId(response) != 0);
    CHECK(oldValue(response) != 0);
    return {connection, host, sessionId(response)};
}

void checkResponse(const DispatchResult &result, const CoherenceFrame &request, Status expected_status) {
    if (result.status != expected_status) {
        std::cerr << __func__ << ": dispatch status=" << static_cast<unsigned>(result.status)
                  << " expected=" << static_cast<unsigned>(expected_status) << '\n';
        failures.fetch_add(1, std::memory_order_relaxed);
    }
    CHECK(!result.close_connection);
    const auto &response = requireResponse(result, toString(opcode(request)).data());
    CHECK(validateResponse(response, request));
    if (status(response) != expected_status) {
        std::cerr << __func__ << ": response status=" << static_cast<unsigned>(status(response))
                  << " expected=" << static_cast<unsigned>(expected_status) << '\n';
        failures.fetch_add(1, std::memory_order_relaxed);
    }
}

void testRegisterIsRequiredAndNegotiatesModelSnoop() {
    Harness harness;
    constexpr ConnectionId connection = 101;
    CHECK(harness.server.attachConnection(connection, "tcp", [](const CoherenceFrame &) { return true; }));

    const auto premature = command(Opcode::Gets, 3, 0x1234, 1, kLineA);
    const auto rejected = harness.server.dispatch(connection, premature);
    CHECK(rejected.status == Status::ProtocolRequired);
    CHECK(!rejected.close_connection);
    CHECK(harness.directory.allocatedLineCount() == 0);
    CHECK(harness.engine.sessionFor(3) == 0);

    const auto no_model = registration(3, 0, kNativeFlush);
    const auto no_model_result = harness.server.dispatch(connection, no_model);
    CHECK(no_model_result.status == Status::BadProtocol);
    CHECK(no_model_result.close_connection);
    CHECK(!no_model_result.response.has_value());
    CHECK(harness.engine.sessionFor(3) == 0);

    constexpr ConnectionId valid_connection = 102;
    CHECK(harness.server.attachConnection(valid_connection, "shm", [](const CoherenceFrame &) { return true; }));
    const auto request = registration(3);
    const auto result = harness.server.dispatch(valid_connection, request);
    CHECK(result.status == Status::Ok);
    CHECK(!result.close_connection);
    const auto &response = requireResponse(result, "valid REGISTER");
    CHECK(validateResponse(response, request));
    CHECK(capabilities(response) == kModelSnoop);
    CHECK(ackStrength(response) == AckStrength::MODEL);
    CHECK(size(response) == kLineSize);
    CHECK(value(response) == value(request));
    CHECK(expected(response) == expected(request));
    CHECK(oldValue(response) != 0);
    CHECK(harness.engine.sessionFor(3) == sessionId(response));

    const auto snapshot = harness.registry.inspect(sessionId(response));
    CHECK(snapshot.has_value());
    if (snapshot) {
        CHECK(snapshot->host_id == 3);
        CHECK(snapshot->session_id == sessionId(response));
        CHECK(snapshot->capabilities == kModelSnoop);
        CHECK(snapshot->transport_name == "shm");
        CHECK(snapshot->has_sender);
    }
}

void testDetachWinningConcurrentRegisterLeavesNoSessionOrSender() {
    Harness harness;
    constexpr ConnectionId connection = 151;
    constexpr std::uint16_t host = 23;
    SenderCopyGate gate;
    std::atomic<int> stale_sender_deliveries{};
    ResponseSender sender = GatedSender{gate, stale_sender_deliveries};
    CHECK(harness.server.attachConnection(connection, "tcp", std::move(sender)));

    const auto request = registration(host);
    gate.arm();
    auto registering = std::async(std::launch::async, [&] { return harness.server.dispatch(connection, request); });
    const bool registration_blocked = gate.waitUntilBlocked();
    CHECK(registration_blocked);
    if (!registration_blocked) {
        gate.release();
        (void)registering.get();
        return;
    }

    CHECK(harness.server.detachConnection(connection));
    gate.release();
    const auto result = registering.get();
    CHECK(result.status == Status::StaleSession);
    CHECK(result.close_connection);
    CHECK(!result.response.has_value());
    CHECK(harness.engine.sessionFor(host) == 0);
    CHECK(!harness.registry.inspect(1).has_value());
    CHECK(stale_sender_deliveries.load(std::memory_order_relaxed) == 0);

    EndpointObserver replacement_observer;
    const auto replacement = registerPeer(harness, 152, host, replacement_observer);
    CHECK(replacement.session != 0);
    CHECK(harness.engine.sessionFor(host) == replacement.session);
    CHECK(stale_sender_deliveries.load(std::memory_order_relaxed) == 0);
}

void testRegisterSenderCopyFailureClearsRegisteringState() {
    Harness harness;
    constexpr ConnectionId connection = 153;
    constexpr std::uint16_t host = 24;
    std::atomic<std::size_t> copies{};
    ResponseSender sender{ThrowOnNthCopySender{copies, 1}};
    CHECK(harness.server.attachConnection(connection, "tcp", std::move(sender)));

    const auto request = registration(host);
    bool threw = false;
    DispatchResult failed;
    try {
        failed = harness.server.dispatch(connection, request);
    } catch (const std::runtime_error &) {
        threw = true;
    }
    CHECK(!threw);
    if (threw)
        return;

    CHECK(failed.status == Status::IoError);
    CHECK(!failed.close_connection);
    const auto &failure_response = requireResponse(failed, "sender-copy REGISTER failure");
    CHECK(validateResponse(failure_response, request));
    CHECK(status(failure_response) == Status::IoError);
    CHECK(!harness.registry.inspect(1).has_value());
    CHECK(harness.engine.sessionFor(host) == 0);

    const auto retried = harness.server.dispatch(connection, request);
    CHECK(retried.status == Status::Ok);
    CHECK(!retried.close_connection);
    const auto &retry_response = requireResponse(retried, "sender-copy REGISTER retry");
    CHECK(validateResponse(retry_response, request));
    CHECK(sessionId(retry_response) != 0);
    CHECK(harness.engine.sessionFor(host) == sessionId(retry_response));
}

void testThrowingActiveSnoopSenderCopyFailsClosed() {
    Harness harness;
    constexpr ConnectionId connection = 154;
    constexpr std::uint16_t host = 25;
    std::atomic<std::size_t> copies{};
    ResponseSender sender{ThrowOnNthCopySender{copies, 4}};
    CHECK(harness.server.attachConnection(connection, "tcp", std::move(sender)));

    const auto register_request = registration(host);
    const auto registered = harness.server.dispatch(connection, register_request);
    CHECK(registered.status == Status::Ok);
    const auto session = sessionId(requireResponse(registered, "snoop-copy REGISTER"));
    const auto before = harness.registry.inspect(session);
    CHECK(before.has_value());
    CHECK(copies.load(std::memory_order_relaxed) == 3);

    auto snoop = initializeFrame(Opcode::SnpInv);
    setSrcHost(snoop, kServerHost);
    setDstHost(snoop, host);
    setSessionId(snoop, session);
    setSnoopId(snoop, 1);
    setAddress(snoop, kLineA);
    setEpoch(snoop, 1);

    bool threw = false;
    bool sent = true;
    try {
        sent = (harness.server.*privateMember(SendToHostTag{}))(host, snoop);
    } catch (const std::runtime_error &) {
        threw = true;
    }
    CHECK(!threw);
    if (threw)
        return;

    CHECK(!sent);
    CHECK(copies.load(std::memory_order_relaxed) == 4);
    const auto after = harness.registry.inspect(session);
    CHECK(after.has_value());
    CHECK(after->state == SessionState::Active);
    CHECK(after->binding_id == before->binding_id);
    CHECK(after->has_sender);
    CHECK(harness.engine.sessionFor(host) == session);
}

void testConnectionHostAndSessionAreExplicitlyBound() {
    Harness harness;
    EndpointObserver host_observer;
    EndpointObserver device_observer;
    const auto host = registerPeer(harness, 201, 4, host_observer, "tcp");
    const auto device = registerPeer(harness, 202, 5, device_observer, "rdma");
    harness.memory.store(kLineA, lineBytes(0x31));

    const auto valid = command(Opcode::Gets, host.host, host.session, 1, kLineA);
    const auto valid_result = harness.server.dispatch(host.connection, valid);
    checkResponse(valid_result, valid, Status::Ok);
    const auto &valid_response = requireResponse(valid_result, "bound GETS");
    CHECK(lineState(valid_response) == LineState::E);
    CHECK(harness.memory.readCount() == 1);

    const auto wrong_connection = harness.server.dispatch(device.connection, valid);
    CHECK(wrong_connection.status == Status::StaleSession);
    CHECK(harness.memory.readCount() == 1);

    auto wrong_host = command(Opcode::Gets, device.host, host.session, 2, kLineB);
    const auto wrong_host_result = harness.server.dispatch(host.connection, wrong_host);
    CHECK(wrong_host_result.status == Status::StaleSession);

    auto wrong_session = command(Opcode::Gets, host.host, device.session, 2, kLineB);
    const auto wrong_session_result = harness.server.dispatch(host.connection, wrong_session);
    CHECK(wrong_session_result.status == Status::StaleSession);
    CHECK(harness.directory.allocatedLineCount() == 1);
    CHECK(harness.engine.sessionFor(host.host) == host.session);
    CHECK(harness.engine.sessionFor(device.host) == device.session);
}

void testDispatchesMesiWritebackAndAtomicsWithDuplexSnoops() {
    Harness harness;
    EndpointObserver host_observer;
    EndpointObserver device_observer;
    const auto host = registerPeer(harness, 301, 6, host_observer, "shm");
    const auto device = registerPeer(harness, 302, 7, device_observer, "rdma");
    const auto clean_a = lineBytes(0x41);
    const auto clean_b = lineBytes(0x52);
    const auto dirty_a = lineBytes(0xd3);
    harness.memory.store(kLineA, clean_a);
    harness.memory.store(kLineB, clean_b);
    harness.memory.storeScalar(kLineC, 10);

    const auto host_gets = command(Opcode::Gets, host.host, host.session, 1, kLineA);
    const auto host_gets_result = harness.server.dispatch(host.connection, host_gets);
    checkResponse(host_gets_result, host_gets, Status::Ok);
    const auto &host_gets_response = requireResponse(host_gets_result, "host GETS");
    CHECK(lineState(host_gets_response) == LineState::E);
    CHECK(epoch(host_gets_response) == 1);
    CHECK(host_gets_response.data == wireBytes(clean_a));

    const auto device_gets = command(Opcode::Gets, device.host, device.session, 1, kLineA);
    const auto device_gets_result = harness.server.dispatch(device.connection, device_gets);
    checkResponse(device_gets_result, device_gets, Status::Ok);
    const auto &device_gets_response = requireResponse(device_gets_result, "device GETS");
    CHECK(lineState(device_gets_response) == LineState::S);
    CHECK(epoch(device_gets_response) == 2);
    CHECK(device_gets_response.data == wireBytes(clean_a));
    CHECK(host_observer.snoops.size() == 1);
    CHECK(opcode(host_observer.snoops.front()) == Opcode::SnpDowngrade);
    CHECK(dstHost(host_observer.snoops.front()) == host.host);
    CHECK(sessionId(host_observer.snoops.front()) == host.session);
    CHECK(host_observer.ack_dispatch_statuses == std::vector<Status>{Status::Ok});

    const auto host_upgrade =
        command(Opcode::Upgrade, host.host, host.session, 2, kLineA, LineState::S, epoch(host_observer.snoops.front()));
    const auto host_upgrade_result = harness.server.dispatch(host.connection, host_upgrade);
    checkResponse(host_upgrade_result, host_upgrade, Status::Ok);
    const auto &host_upgrade_response = requireResponse(host_upgrade_result, "host UPGRADE");
    CHECK(lineState(host_upgrade_response) == LineState::M);
    CHECK(epoch(host_upgrade_response) == 3);
    CHECK(payloadLength(host_upgrade_response) == 0);
    CHECK(device_observer.snoops.size() == 1);
    CHECK(opcode(device_observer.snoops.front()) == Opcode::SnpInv);
    CHECK(dstHost(device_observer.snoops.front()) == device.host);
    CHECK(sessionId(device_observer.snoops.front()) == device.session);
    CHECK(device_observer.ack_dispatch_statuses == std::vector<Status>{Status::Ok});

    const auto host_putm = putm(host.host, host.session, 3, kLineA, epoch(host_upgrade_response), dirty_a);
    const auto host_putm_result = harness.server.dispatch(host.connection, host_putm);
    checkResponse(host_putm_result, host_putm, Status::Ok);
    const auto &host_putm_response = requireResponse(host_putm_result, "host PUTM");
    CHECK(lineState(host_putm_response) == LineState::I);
    CHECK(epoch(host_putm_response) == 4);
    CHECK(harness.memory.inspect(kLineA) == dirty_a);

    const auto device_getm = command(Opcode::Getm, device.host, device.session, 2, kLineB);
    const auto device_getm_result = harness.server.dispatch(device.connection, device_getm);
    checkResponse(device_getm_result, device_getm, Status::Ok);
    const auto &device_getm_response = requireResponse(device_getm_result, "device GETM");
    CHECK(lineState(device_getm_response) == LineState::M);
    CHECK(epoch(device_getm_response) == 1);
    CHECK(device_getm_response.data == wireBytes(clean_b));

    const auto atomic_faa = faa(device.host, device.session, 3, kLineC, LineState::I, 0, 5);
    const auto faa_result = harness.server.dispatch(device.connection, atomic_faa);
    checkResponse(faa_result, atomic_faa, Status::Ok);
    const auto &faa_response = requireResponse(faa_result, "ATOMIC_FAA");
    CHECK(lineState(faa_response) == LineState::M);
    CHECK(epoch(faa_response) == 1);
    CHECK(oldValue(faa_response) == 10);
    CHECK(scalar(faa_response) == 15);

    const auto atomic_cas = cas(device.host, device.session, 4, kLineC, LineState::M, epoch(faa_response), 15, 42);
    const auto cas_result = harness.server.dispatch(device.connection, atomic_cas);
    checkResponse(cas_result, atomic_cas, Status::Ok);
    const auto &cas_response = requireResponse(cas_result, "ATOMIC_CAS");
    CHECK(lineState(cas_response) == LineState::M);
    CHECK(epoch(cas_response) == 2);
    CHECK(oldValue(cas_response) == 15);
    CHECK(scalar(cas_response) == 42);

    const auto counters = harness.directory.transitionCounters();
    CHECK(counters.gets == 2);
    CHECK(counters.getm == 1);
    CHECK(counters.upgrade == 1);
    CHECK(counters.putm == 1);
    CHECK(counters.atomic == 2);

    const auto host_binding = harness.registry.inspect(host.session)->binding_id;
    const auto device_binding = harness.registry.inspect(device.session)->binding_id;
    CHECK(harness.registry.holderSnapshot(host.session, host_binding).clean.empty());
    CHECK(harness.registry.holderSnapshot(host.session, host_binding).modified.empty());
    CHECK(harness.registry.holderSnapshot(device.session, device_binding).clean.empty());
    CHECK(harness.registry.holderSnapshot(device.session, device_binding).modified ==
          (std::vector<std::uint64_t>{kLineB, kLineC}));
}

void testDirtyBackInvalidationIsTracedAfterServerCopyCompletion() {
    const auto path =
        std::filesystem::temp_directory_path() / ("cxlmemsim-server-trace-" + std::to_string(getpid()) + ".jsonl");
    auto trace = std::make_shared<CoherenceTraceV2>(path);
    Harness harness(trace);
    EndpointObserver owner_observer;
    EndpointObserver requester_observer;
    const auto owner = registerPeer(harness, 351, 8, owner_observer);
    const auto requester = registerPeer(harness, 352, 9, requester_observer);
    const auto clean = lineBytes(0x31);
    const auto dirty = lineBytes(0xc7);
    harness.memory.store(kLineA, clean);

    const auto owner_getm = command(Opcode::Getm, owner.host, owner.session, 1, kLineA);
    checkResponse(harness.server.dispatch(owner.connection, owner_getm), owner_getm, Status::Ok);
    owner_observer.cache[kLineA].data = wireBytes(dirty);

    const auto requester_getm = command(Opcode::Getm, requester.host, requester.session, 1, kLineA);
    checkResponse(harness.server.dispatch(requester.connection, requester_getm), requester_getm, Status::Ok);

    CHECK(harness.memory.inspect(kLineA) == dirty);
    CHECK(owner_observer.snoops.size() == 1);
    if (!owner_observer.snoops.empty())
        CHECK(opcode(owner_observer.snoops.front()) == Opcode::SnpDataInv);
    const auto snapshot = trace->snapshot();
    CHECK(snapshot.registrations == 2);
    CHECK(snapshot.getm == 2);
    CHECK(snapshot.snp_data_inv == 1);
    CHECK(snapshot.model_acks == 1);
    CHECK(snapshot.dirty_data_completions == 1);
    CHECK(snapshot.active_bindings == 2);
    trace.reset();
    std::filesystem::remove(path);
}

void testPutsResponseIsInvalidForReleaserWhileDirectoryKeepsOtherSharer() {
    Harness harness;
    EndpointObserver first_observer;
    EndpointObserver second_observer;
    const auto first = registerPeer(harness, 351, 21, first_observer);
    const auto second = registerPeer(harness, 352, 22, second_observer);
    harness.memory.store(kLineA, lineBytes(0x5a));

    const auto first_gets = command(Opcode::Gets, first.host, first.session, 1, kLineA);
    checkResponse(harness.server.dispatch(first.connection, first_gets), first_gets, Status::Ok);
    const auto second_gets = command(Opcode::Gets, second.host, second.session, 1, kLineA);
    const auto second_result = harness.server.dispatch(second.connection, second_gets);
    checkResponse(second_result, second_gets, Status::Ok);
    CHECK(first_observer.snoops.size() == 1);
    if (first_observer.snoops.empty())
        return;

    const auto puts =
        command(Opcode::Puts, first.host, first.session, 2, kLineA, LineState::S, epoch(first_observer.snoops.front()));
    const auto puts_result = harness.server.dispatch(first.connection, puts);
    checkResponse(puts_result, puts, Status::Ok);
    const auto &puts_response = requireResponse(puts_result, "PUTS with remaining sharer");
    CHECK(lineState(puts_response) == LineState::I);
    CHECK(epoch(puts_response) == 3);

    const auto snapshot = harness.directory.inspect(kLineA);
    CHECK(snapshot.has_value());
    if (snapshot) {
        CHECK(snapshot->state == MesiState::S);
        CHECK(snapshot->sharers == holder(second.host));
        CHECK(snapshot->epoch == 3);
    }
}

void testMalformedAckCannotGrantOwnership() {
    Harness harness;
    EndpointObserver owner_observer;
    EndpointObserver requester_observer;
    owner_observer.ack_mode = AckMode::CorruptEpoch;
    const auto owner = registerPeer(harness, 401, 8, owner_observer);
    const auto requester = registerPeer(harness, 402, 9, requester_observer);
    harness.memory.store(kLineA, lineBytes(0x61));

    const auto gets = command(Opcode::Gets, owner.host, owner.session, 1, kLineA);
    checkResponse(harness.server.dispatch(owner.connection, gets), gets, Status::Ok);

    const auto getm = command(Opcode::Getm, requester.host, requester.session, 1, kLineA);
    const auto result = harness.server.dispatch(requester.connection, getm);
    checkResponse(result, getm, Status::CoherenceTimeout);
    const auto &response = requireResponse(result, "GETM after malformed ACK");
    CHECK(lineState(response) == LineState::I);
    CHECK(epoch(response) == 0);
    CHECK(payloadLength(response) == 0);
    CHECK(owner_observer.snoops.size() == 1);
    CHECK(owner_observer.ack_dispatch_statuses == std::vector<Status>{Status::BadProtocol});

    const auto snapshot = harness.directory.inspect(kLineA);
    CHECK(snapshot.has_value());
    if (snapshot) {
        CHECK(snapshot->state == MesiState::E);
        CHECK(snapshot->owner == owner.host);
        CHECK(snapshot->epoch == 1);
        CHECK(snapshot->owner != requester.host);
    }
    const auto audit = harness.engine.auditCounters();
    CHECK(audit.timeout == 1);
    CHECK(audit.partial_ack == 0);
}

void testPartialAckCommitsOnlyAckedInvalidationsAndNeverGrants() {
    Harness harness;
    EndpointObserver first_observer;
    EndpointObserver second_observer;
    EndpointObserver requester_observer;
    const auto first = registerPeer(harness, 501, 10, first_observer);
    const auto second = registerPeer(harness, 502, 11, second_observer);
    const auto requester = registerPeer(harness, 503, 12, requester_observer);
    harness.memory.store(kLineA, lineBytes(0x71));

    const auto first_gets = command(Opcode::Gets, first.host, first.session, 1, kLineA);
    checkResponse(harness.server.dispatch(first.connection, first_gets), first_gets, Status::Ok);
    const auto second_gets = command(Opcode::Gets, second.host, second.session, 1, kLineA);
    checkResponse(harness.server.dispatch(second.connection, second_gets), second_gets, Status::Ok);
    const auto requester_gets = command(Opcode::Gets, requester.host, requester.session, 1, kLineA);
    const auto requester_gets_result = harness.server.dispatch(requester.connection, requester_gets);
    checkResponse(requester_gets_result, requester_gets, Status::Ok);
    const auto &requester_gets_response = requireResponse(requester_gets_result, "requester GETS");
    CHECK(lineState(requester_gets_response) == LineState::S);
    CHECK(epoch(requester_gets_response) == 3);

    first_observer.ack_mode = AckMode::Good;
    second_observer.ack_mode = AckMode::Drop;
    const auto upgrade = command(Opcode::Upgrade, requester.host, requester.session, 2, kLineA, LineState::S,
                                 epoch(requester_gets_response));
    const auto result = harness.server.dispatch(requester.connection, upgrade);
    checkResponse(result, upgrade, Status::CoherenceTimeout);
    const auto &response = requireResponse(result, "UPGRADE after partial ACK");
    CHECK(lineState(response) == LineState::S);
    CHECK(epoch(response) == 4);
    CHECK(payloadLength(response) == 0);

    CHECK(first_observer.snoops.size() == 2);
    CHECK(opcode(first_observer.snoops.back()) == Opcode::SnpInv);
    CHECK(second_observer.snoops.size() == 1);
    CHECK(opcode(second_observer.snoops.back()) == Opcode::SnpInv);
    CHECK(first_observer.ack_dispatch_statuses == (std::vector<Status>{Status::Ok, Status::Ok}));
    CHECK(second_observer.ack_dispatch_statuses.empty());

    const auto snapshot = harness.directory.inspect(kLineA);
    CHECK(snapshot.has_value());
    if (snapshot) {
        CHECK(snapshot->state == MesiState::S);
        CHECK(!snapshot->owner.has_value());
        CHECK((snapshot->sharers & holder(first.host)) == 0);
        CHECK((snapshot->sharers & holder(second.host)) != 0);
        CHECK((snapshot->sharers & holder(requester.host)) != 0);
        CHECK(snapshot->epoch == 4);
        CHECK(isValidSnapshot(*snapshot));
    }
    const auto audit = harness.engine.auditCounters();
    CHECK(audit.timeout == 1);
    CHECK(audit.partial_ack == 1);

    const auto first_binding = harness.registry.inspect(first.session)->binding_id;
    const auto second_binding = harness.registry.inspect(second.session)->binding_id;
    const auto requester_binding = harness.registry.inspect(requester.session)->binding_id;
    CHECK(harness.registry.cleanHolders(first.session, first_binding).empty());
    CHECK(harness.registry.cleanHolders(second.session, second_binding) == std::vector<std::uint64_t>{kLineA});
    CHECK(harness.registry.cleanHolders(requester.session, requester_binding) == std::vector<std::uint64_t>{kLineA});
}

void expectPartialMissLeavesRequesterInvalid(Opcode request_opcode) {
    Harness harness;
    EndpointObserver first_observer;
    EndpointObserver second_observer;
    EndpointObserver requester_observer;
    const auto first = registerPeer(harness, 511, 13, first_observer);
    const auto second = registerPeer(harness, 512, 14, second_observer);
    const auto requester = registerPeer(harness, 513, 15, requester_observer);
    harness.memory.storeScalar(kLineA, 9);

    const auto first_gets = command(Opcode::Gets, first.host, first.session, 1, kLineA);
    checkResponse(harness.server.dispatch(first.connection, first_gets), first_gets, Status::Ok);
    const auto second_gets = command(Opcode::Gets, second.host, second.session, 1, kLineA);
    checkResponse(harness.server.dispatch(second.connection, second_gets), second_gets, Status::Ok);

    first_observer.ack_mode = AckMode::Good;
    second_observer.ack_mode = AckMode::Drop;
    auto request = request_opcode == Opcode::Getm
                       ? command(Opcode::Getm, requester.host, requester.session, 1, kLineA)
                       : faa(requester.host, requester.session, 1, kLineA, LineState::I, 0, 1);
    const auto result = harness.server.dispatch(requester.connection, request);
    checkResponse(result, request, Status::CoherenceTimeout);
    const auto &response = requireResponse(result, "partial miss");
    CHECK(lineState(response) == LineState::I);
    CHECK(epoch(response) == 3);
    CHECK(payloadLength(response) == 0);

    const auto snapshot = harness.directory.inspect(kLineA);
    CHECK(snapshot.has_value());
    if (snapshot) {
        CHECK(snapshot->state == MesiState::S);
        CHECK(snapshot->sharers == holder(second.host));
        CHECK(snapshot->epoch == 3);
    }
    const auto requester_binding = harness.registry.inspect(requester.session)->binding_id;
    CHECK(harness.registry.cleanHolders(requester.session, requester_binding).empty());
    CHECK(harness.registry.modifiedHolders(requester.session, requester_binding).empty());
}

void testPartialGetmReportsRequesterInvalidAtAdvancedEpoch() { expectPartialMissLeavesRequesterInvalid(Opcode::Getm); }

void testPartialAtomicReportsRequesterInvalidAtAdvancedEpoch() {
    expectPartialMissLeavesRequesterInvalid(Opcode::AtomicFaa);
}

void expectPartialReacquireCapsRequesterAtReportedInvalid(Opcode request_opcode) {
    Harness harness;
    EndpointObserver first_observer;
    EndpointObserver second_observer;
    EndpointObserver requester_observer;
    const auto first = registerPeer(harness, 521, 16, first_observer);
    const auto second = registerPeer(harness, 522, 17, second_observer);
    const auto requester = registerPeer(harness, 523, 18, requester_observer);
    harness.memory.storeScalar(kLineA, 9);

    checkResponse(
        harness.server.dispatch(first.connection, command(Opcode::Gets, first.host, first.session, 1, kLineA)),
        command(Opcode::Gets, first.host, first.session, 1, kLineA), Status::Ok);
    const auto second_gets = command(Opcode::Gets, second.host, second.session, 1, kLineA);
    checkResponse(harness.server.dispatch(second.connection, second_gets), second_gets, Status::Ok);
    const auto requester_gets = command(Opcode::Gets, requester.host, requester.session, 1, kLineA);
    checkResponse(harness.server.dispatch(requester.connection, requester_gets), requester_gets, Status::Ok);

    first_observer.ack_mode = AckMode::Good;
    second_observer.ack_mode = AckMode::Drop;
    CoherenceFrame request;
    if (request_opcode == Opcode::Getm) {
        request = command(Opcode::Getm, requester.host, requester.session, 2, kLineA, LineState::I, 0);
    } else if (request_opcode == Opcode::AtomicFaa) {
        request = faa(requester.host, requester.session, 2, kLineA, LineState::I, 0, 1);
    } else {
        request = cas(requester.host, requester.session, 2, kLineA, LineState::I, 0, 9, 10);
    }
    const auto result = harness.server.dispatch(requester.connection, request);
    checkResponse(result, request, Status::CoherenceTimeout);
    const auto &response = requireResponse(result, "partial reacquisition");
    CHECK(lineState(response) == LineState::I);
    CHECK(payloadLength(response) == 0);
}

void testPartialReacquisitionCapsRequesterPermission() {
    expectPartialReacquireCapsRequesterAtReportedInvalid(Opcode::Getm);
    expectPartialReacquireCapsRequesterAtReportedInvalid(Opcode::AtomicFaa);
    expectPartialReacquireCapsRequesterAtReportedInvalid(Opcode::AtomicCas);
}

void testThrowingPinnedResponseSenderBecomesDeliveryFailure() {
    Harness harness;
    constexpr ConnectionId connection = 524;
    constexpr std::uint16_t host = 19;
    CHECK(harness.server.attachConnection(connection, "throwing", [](const CoherenceFrame &) -> bool {
        throw std::runtime_error("injected response delivery failure");
    }));
    const auto register_request = registration(host);
    const auto registered = harness.server.dispatch(connection, register_request);
    const auto session = sessionId(requireResponse(registered, "throwing sender REGISTER"));
    const auto heartbeat = command(Opcode::Heartbeat, host, session, 1);
    bool threw = false;
    DispatchResult result;
    try {
        result = harness.server.dispatch(connection, heartbeat);
    } catch (const std::runtime_error &) {
        threw = true;
    }
    CHECK(!threw);
    if (!threw) {
        CHECK(result.status == Status::IoError);
        CHECK(result.close_connection);
        CHECK(result.response_via_sender);
        CHECK(result.delivery_failed);
    }
}

void testResumedHostIsNotSnoopVisibleBeforeRegisterResponse() {
    Harness harness;
    EndpointObserver owner_observer;
    EndpointObserver requester_observer;
    const auto owner = registerPeer(harness, 525, 20, owner_observer);
    const auto requester = registerPeer(harness, 526, 21, requester_observer);
    harness.memory.store(kLineA, lineBytes(0x4a));
    const auto owner_gets = command(Opcode::Gets, owner.host, owner.session, 1, kLineA);
    checkResponse(harness.server.dispatch(owner.connection, owner_gets), owner_gets, Status::Ok);
    CHECK(harness.server.detachConnection(owner.connection));

    std::mutex gate_mutex;
    std::condition_variable gate_changed;
    bool register_entered = false;
    bool release_register = false;
    bool snoop_before_register = false;
    constexpr ConnectionId resumed_connection = 527;
    CHECK(harness.server.attachConnection(resumed_connection, "resumed", [&](const CoherenceFrame &frame) {
        if (opcode(frame) == Opcode::Response && requestId(frame) == 0) {
            std::unique_lock lock(gate_mutex);
            register_entered = true;
            gate_changed.notify_all();
            gate_changed.wait(lock, [&] { return release_register; });
            return true;
        }
        if (isSnoop(frame)) {
            std::lock_guard lock(gate_mutex);
            snoop_before_register = !release_register;
            return false;
        }
        return true;
    }));

    const auto resume_request = registration(owner.host, owner.session, kModelSnoop);
    auto resumed =
        std::async(std::launch::async, [&] { return harness.server.dispatch(resumed_connection, resume_request); });
    {
        std::unique_lock lock(gate_mutex);
        CHECK(gate_changed.wait_for(lock, std::chrono::seconds(1), [&] { return register_entered; }));
    }
    const auto requester_getm = command(Opcode::Getm, requester.host, requester.session, 1, kLineA);
    (void)harness.server.dispatch(requester.connection, requester_getm);
    {
        std::lock_guard lock(gate_mutex);
        release_register = true;
    }
    gate_changed.notify_all();
    CHECK(resumed.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    if (resumed.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        CHECK(resumed.get().status == Status::Ok);
    CHECK(!snoop_before_register);
}

void testResumedRegisterResponsePrecedesPinnedReplay() {
    Harness harness;
    constexpr ConnectionId old_connection = 504;
    constexpr ConnectionId resumed_connection = 505;
    constexpr std::uint16_t host = 13;
    CHECK(harness.server.attachConnection(old_connection, "old", [](const CoherenceFrame &) { return false; }));

    const auto register_request = registration(host);
    const auto registered = harness.server.dispatch(old_connection, register_request);
    const auto &register_response = requireResponse(registered, "initial REGISTER");
    const auto session = sessionId(register_response);
    const auto heartbeat = command(Opcode::Heartbeat, host, session, 1);
    const auto heartbeat_result = harness.server.dispatch(old_connection, heartbeat);
    CHECK(heartbeat_result.status == Status::IoError);
    CHECK(heartbeat_result.close_connection);
    CHECK(heartbeat_result.response_via_sender);
    CHECK(heartbeat_result.delivery_failed);
    const auto &pinned_heartbeat = requireResponse(heartbeat_result, "failed heartbeat delivery");
    CHECK(status(pinned_heartbeat) == Status::Ok);
    CHECK(harness.registry.pinnedResponseIds(session) == std::vector<std::uint64_t>{1});
    CHECK(harness.server.detachConnection(old_connection));

    std::vector<CoherenceFrame> delivered;
    const auto sender = [&](const CoherenceFrame &frame) {
        delivered.push_back(frame);
        return true;
    };
    CHECK(harness.server.attachConnection(resumed_connection, "resumed", sender));
    const auto resume_request = registration(host, session, kModelSnoop);
    const auto resumed = harness.server.dispatch(resumed_connection, resume_request);
    if (resumed.response && !resumed.response_via_sender)
        CHECK(sender(*resumed.response));

    CHECK(resumed.status == Status::Ok);
    CHECK(delivered.size() == 2);
    if (delivered.size() == 2) {
        CHECK(validateResponse(delivered[0], resume_request));
        CHECK(validateResponse(delivered[1], heartbeat));
    }
}

void testV1AndBadVersionsFailClosed() {
    Harness harness;
    constexpr std::array<std::uint16_t, 2> bad_versions{1, static_cast<std::uint16_t>(kProtocolVersion + 1)};
    for (std::size_t index = 0; index < bad_versions.size(); ++index) {
        const auto connection = static_cast<ConnectionId>(601 + index);
        CHECK(harness.server.attachConnection(connection, "tcp", [](const CoherenceFrame &) { return true; }));
        auto request = registration(static_cast<std::uint16_t>(20 + index));
        setVersion(request, bad_versions[index]);
        const auto result = harness.server.dispatch(connection, request);
        CHECK(result.status == Status::BadProtocol);
        CHECK(result.close_connection);
        CHECK(!result.response.has_value());
        CHECK(harness.engine.sessionFor(static_cast<std::uint16_t>(20 + index)) == 0);
    }
    CHECK(harness.directory.allocatedLineCount() == 0);
}

void testMalformedOrdinaryReturnsBadProtocolAndKeepsConnectionUsable() {
    Harness harness;
    EndpointObserver observer;
    const auto peer = registerPeer(harness, 651, 24, observer);
    harness.memory.store(kLineA, lineBytes(0x81));

    auto malformed = command(Opcode::Gets, peer.host, peer.session, 1, kLineA);
    setReserved0(malformed, 1);
    CHECK(!validateFrame(malformed));
    const auto rejected = harness.server.dispatch(peer.connection, malformed);
    CHECK(rejected.status == Status::BadProtocol);
    CHECK(!rejected.close_connection);
    CHECK(!rejected.response_via_sender);
    const auto &response = requireResponse(rejected, "malformed ordinary request");
    CHECK(opcode(response) == Opcode::Response);
    CHECK(status(response) == Status::BadProtocol);

    auto valid = malformed;
    setReserved0(valid, 0);
    CHECK(validateFrame(valid));
    CHECK(validateResponse(response, valid));
    CHECK(harness.registry.pinnedResponseIds(peer.session).empty());
    CHECK(harness.directory.allocatedLineCount() == 0);
    CHECK(harness.memory.readCount() == 0);

    const auto retry = harness.server.dispatch(peer.connection, valid);
    checkResponse(retry, valid, Status::Ok);
    CHECK(harness.memory.readCount() == 1);
}

void testDuplicateRequestReturnsByteIdenticalPinnedResponse() {
    Harness harness;
    EndpointObserver observer;
    const auto peer = registerPeer(harness, 701, 14, observer);
    harness.memory.store(kLineD, lineBytes(0x91));

    const auto gets = command(Opcode::Gets, peer.host, peer.session, 1, kLineD);
    const auto first = harness.server.dispatch(peer.connection, gets);
    checkResponse(first, gets, Status::Ok);
    const auto &first_response = requireResponse(first, "initial replay-pinned GETS");
    const auto first_wire = encodeFrame(first_response);
    const auto reads_after_first = harness.memory.readCount();
    const auto counters_after_first = harness.directory.transitionCounters();

    const auto duplicate = harness.server.dispatch(peer.connection, gets);
    checkResponse(duplicate, gets, Status::Ok);
    const auto &duplicate_response = requireResponse(duplicate, "duplicate replay-pinned GETS");
    CHECK(encodeFrame(duplicate_response) == first_wire);
    CHECK(harness.memory.readCount() == reads_after_first);
    CHECK(harness.directory.transitionCounters().gets == counters_after_first.gets);
    CHECK(harness.registry.pinnedResponseIds(peer.session) == std::vector<std::uint64_t>{1});

    auto conflicting = gets;
    setAddress(conflicting, kLineA);
    const auto conflict = harness.server.dispatch(peer.connection, conflicting);
    CHECK(conflict.status == Status::StaleRequest);
    CHECK(harness.directory.allocatedLineCount() == 1);
    CHECK(harness.registry.pinnedResponseIds(peer.session) == std::vector<std::uint64_t>{1});
}

void testCapacityFailureCannotEscapeDirectoryGrant() {
    Harness harness;
    EndpointObserver observer;
    const auto peer = registerPeer(harness, 801, 15, observer, "tcp", kLineSize, 1);
    harness.memory.store(kLineA, lineBytes(0xa1));
    harness.memory.store(kLineB, lineBytes(0xb2));

    const auto first = command(Opcode::Gets, peer.host, peer.session, 1, kLineA);
    checkResponse(harness.server.dispatch(peer.connection, first), first, Status::Ok);
    CHECK(harness.registry.cleanHolders(peer.session, harness.registry.inspect(peer.session)->binding_id) ==
          std::vector<std::uint64_t>{kLineA});

    const auto overflow = command(Opcode::Gets, peer.host, peer.session, 2, kLineB);
    const auto result = harness.server.dispatch(peer.connection, overflow);
    checkResponse(result, overflow, Status::IoError);
    const auto line_b = harness.directory.inspect(kLineB);
    CHECK(line_b.has_value());
    if (line_b) {
        CHECK(line_b->state == MesiState::I);
        CHECK(!line_b->owner.has_value());
        CHECK(line_b->epoch == 0);
    }
    const auto binding = harness.registry.inspect(peer.session)->binding_id;
    CHECK(harness.registry.cleanHolders(peer.session, binding) == std::vector<std::uint64_t>{kLineA});
}

void testDirtyPersistenceFailureRetainsDirectoryOwnerIndex() {
    Harness harness;
    EndpointObserver owner_observer;
    EndpointObserver requester_observer;
    const auto owner = registerPeer(harness, 811, 16, owner_observer);
    const auto requester = registerPeer(harness, 812, 17, requester_observer);
    harness.memory.store(kLineA, lineBytes(0xc3));

    const auto getm = command(Opcode::Getm, owner.host, owner.session, 1, kLineA);
    checkResponse(harness.server.dispatch(owner.connection, getm), getm, Status::Ok);
    const auto owner_binding = harness.registry.inspect(owner.session)->binding_id;
    CHECK(harness.registry.modifiedHolders(owner.session, owner_binding) == std::vector<std::uint64_t>{kLineA});

    harness.memory.failNextWrite();
    const auto gets = command(Opcode::Gets, requester.host, requester.session, 1, kLineA);
    checkResponse(harness.server.dispatch(requester.connection, gets), gets, Status::CoherenceTimeout);
    const auto snapshot = harness.directory.inspect(kLineA);
    CHECK(snapshot.has_value());
    if (snapshot) {
        CHECK(snapshot->state == MesiState::M);
        CHECK(snapshot->owner == owner.host);
    }
    CHECK(harness.registry.modifiedHolders(owner.session, owner_binding) == std::vector<std::uint64_t>{kLineA});
    CHECK(harness.registry.cleanHolders(owner.session, owner_binding).empty());
}

void testHeartbeatAcknowledgesAndReclaimsReplayWindow() {
    Harness harness;
    EndpointObserver observer;
    const auto peer = registerPeer(harness, 821, 18, observer);
    harness.memory.store(kLineA, lineBytes(0xd4));

    const auto gets = command(Opcode::Gets, peer.host, peer.session, 1, kLineA);
    checkResponse(harness.server.dispatch(peer.connection, gets), gets, Status::Ok);
    auto heartbeat = command(Opcode::Heartbeat, peer.host, peer.session, 2);
    setOldValue(heartbeat, 1);
    checkResponse(harness.server.dispatch(peer.connection, heartbeat), heartbeat, Status::Ok);
    CHECK(harness.registry.pinnedResponseIds(peer.session) == std::vector<std::uint64_t>{2});

    const auto replay = harness.server.dispatch(peer.connection, gets);
    checkResponse(replay, gets, Status::StaleRequest);
    const auto &response = requireResponse(replay, "reclaimed replay");
    CHECK(oldValue(response) == harness.registry.replayFloor(peer.session));
}

void testHostFenceAckUsesContextualRoute() {
    Harness harness;
    EndpointObserver observer;
    const auto peer = registerPeer(harness, 831, 19, observer);
    const auto session = harness.registry.inspect(peer.session);
    CHECK(session.has_value());
    if (!session)
        return;

    const auto result = harness.engine.evictHost(harness.registry, peer.host, peer.session, session->binding_id,
                                                 HostFailurePolicy::RequireFenceAck);
    CHECK(result.status == AdministrativeStatus::Ok);
    CHECK(observer.snoops.size() == 1);
    if (!observer.snoops.empty())
        CHECK(opcode(observer.snoops.front()) == Opcode::HostFence);
    CHECK(observer.ack_dispatch_statuses == std::vector<Status>{Status::Ok});
    CHECK(!harness.registry.inspect(peer.session).has_value());
}

void testInFlightDuplicateWaitsForPinnedResponse() {
    Harness harness;
    EndpointObserver observer;
    const auto peer = registerPeer(harness, 841, 20, observer);
    harness.memory.store(kLineA, lineBytes(0xe5));
    harness.memory.blockReads();
    const auto gets = command(Opcode::Gets, peer.host, peer.session, 1, kLineA);

    auto first = std::async(std::launch::async, [&] { return harness.server.dispatch(peer.connection, gets); });
    harness.memory.waitForRead();
    auto duplicate = std::async(std::launch::async, [&] { return harness.server.dispatch(peer.connection, gets); });
    CHECK(duplicate.wait_for(std::chrono::milliseconds(5)) == std::future_status::timeout);
    harness.memory.releaseReads();

    const auto first_result = first.get();
    const auto duplicate_result = duplicate.get();
    checkResponse(first_result, gets, Status::Ok);
    checkResponse(duplicate_result, gets, Status::Ok);
    CHECK(encodeFrame(requireResponse(first_result, "in-flight first")) ==
          encodeFrame(requireResponse(duplicate_result, "in-flight duplicate")));
    CHECK(harness.directory.transitionCounters().gets == 1);
    CHECK(!observer.saw_nonterminal_response);
}

} // namespace

int main() {
    testRegisterIsRequiredAndNegotiatesModelSnoop();
    testDetachWinningConcurrentRegisterLeavesNoSessionOrSender();
    testRegisterSenderCopyFailureClearsRegisteringState();
    testThrowingActiveSnoopSenderCopyFailsClosed();
    testConnectionHostAndSessionAreExplicitlyBound();
    testDispatchesMesiWritebackAndAtomicsWithDuplexSnoops();
    testDirtyBackInvalidationIsTracedAfterServerCopyCompletion();
    testPutsResponseIsInvalidForReleaserWhileDirectoryKeepsOtherSharer();
    testMalformedAckCannotGrantOwnership();
    testPartialAckCommitsOnlyAckedInvalidationsAndNeverGrants();
    testPartialGetmReportsRequesterInvalidAtAdvancedEpoch();
    testPartialAtomicReportsRequesterInvalidAtAdvancedEpoch();
    testPartialReacquisitionCapsRequesterPermission();
    testThrowingPinnedResponseSenderBecomesDeliveryFailure();
    testResumedHostIsNotSnoopVisibleBeforeRegisterResponse();
    testResumedRegisterResponsePrecedesPinnedReplay();
    testV1AndBadVersionsFailClosed();
    testMalformedOrdinaryReturnsBadProtocolAndKeepsConnectionUsable();
    testDuplicateRequestReturnsByteIdenticalPinnedResponse();
    testCapacityFailureCannotEscapeDirectoryGrant();
    testDirtyPersistenceFailureRetainsDirectoryOwnerIndex();
    testHeartbeatAcknowledgesAndReclaimsReplayWindow();
    testHostFenceAckUsesContextualRoute();
    testInFlightDuplicateWaitsForPinnedResponse();

    if (const auto failed = failures.load(std::memory_order_relaxed); failed != 0) {
        std::cerr << failed << " coherence server v2 test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "coherence server v2 tests passed\n";
    return EXIT_SUCCESS;
}
