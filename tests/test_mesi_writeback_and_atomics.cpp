#include "endpoint_session_registry.h"
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
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
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

constexpr std::uint64_t kLineA = 0x1000;
constexpr std::uint64_t kLineB = 0x2000;
constexpr std::uint64_t kLineC = 0x3000;
constexpr auto kWait = std::chrono::seconds(5);

std::uint64_t holder(std::uint16_t host) { return std::uint64_t{1} << host; }

std::array<std::byte, 64> bytes(std::uint8_t value) {
    std::array<std::byte, 64> result{};
    result.fill(static_cast<std::byte>(value));
    return result;
}

void storeScalar(std::array<std::byte, 64> &line, std::size_t offset, std::uint64_t value) {
    std::memcpy(line.data() + offset, &value, sizeof(value));
}

std::uint64_t loadScalar(const std::array<std::byte, 64> &line, std::size_t offset) {
    std::uint64_t value{};
    std::memcpy(&value, line.data() + offset, sizeof(value));
    return value;
}

class TestMemory final : public CoherenceMemoryBackend {
public:
    std::array<std::byte, 64> readLine(std::uint64_t address) override {
        std::unique_lock lock(mutex_);
        ++reads_;
        read_entered_ = true;
        changed_.notify_all();
        changed_.wait(lock, [&] { return !block_reads_; });
        return lines_[address];
    }

    void writeLine(std::uint64_t address, std::span<const std::byte, 64> data) override {
        std::unique_lock lock(mutex_);
        ++writes_;
        write_entered_ = true;
        changed_.notify_all();
        changed_.wait(lock, [&] { return !block_writes_; });
        if (throw_writes_)
            throw std::runtime_error("injected write failure");
        std::copy(data.begin(), data.end(), lines_[address].begin());
        events_.push_back("write:" + std::to_string(address));
    }

    void seed(std::uint64_t address, const std::array<std::byte, 64> &line) {
        std::lock_guard lock(mutex_);
        lines_[address] = line;
    }

    std::array<std::byte, 64> line(std::uint64_t address) const {
        std::lock_guard lock(mutex_);
        const auto found = lines_.find(address);
        return found == lines_.end() ? std::array<std::byte, 64>{} : found->second;
    }

    void blockWrites() {
        std::lock_guard lock(mutex_);
        block_writes_ = true;
        write_entered_ = false;
    }

    void blockReads() {
        std::lock_guard lock(mutex_);
        block_reads_ = true;
        read_entered_ = false;
    }

    void waitForRead() {
        std::unique_lock lock(mutex_);
        if (!changed_.wait_for(lock, kWait, [&] { return read_entered_; })) {
            std::cerr << "backend read wait timed out\n";
            std::_Exit(EXIT_FAILURE);
        }
    }

    void releaseReads() {
        std::lock_guard lock(mutex_);
        block_reads_ = false;
        changed_.notify_all();
    }

    void waitForWrite() {
        std::unique_lock lock(mutex_);
        if (!changed_.wait_for(lock, kWait, [&] { return write_entered_; })) {
            std::cerr << "backend write wait timed out\n";
            std::_Exit(EXIT_FAILURE);
        }
    }

    void releaseWrites() {
        std::lock_guard lock(mutex_);
        block_writes_ = false;
        changed_.notify_all();
    }

    void throwWrites(bool enabled = true) {
        std::lock_guard lock(mutex_);
        throw_writes_ = enabled;
    }

    std::size_t reads() const {
        std::lock_guard lock(mutex_);
        return reads_;
    }

    std::size_t writes() const {
        std::lock_guard lock(mutex_);
        return writes_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::map<std::uint64_t, std::array<std::byte, 64>> lines_;
    std::vector<std::string> events_;
    std::size_t reads_{};
    std::size_t writes_{};
    bool block_writes_{};
    bool throw_writes_{};
    bool write_entered_{};
    bool block_reads_{};
    bool read_entered_{};
};

class TestTransport final : public CoherenceTransport {
public:
    bool sendToHost(std::uint16_t host, const CoherenceFrame &frame) override {
        std::function<void(std::uint16_t, const CoherenceFrame &)> callback;
        {
            std::lock_guard lock(mutex_);
            sent_.emplace_back(host, frame);
            callback = callback_;
            changed_.notify_all();
        }
        if (callback)
            callback(host, frame);
        if (throw_on_send_)
            throw std::runtime_error("injected transport failure");
        return send_result_;
    }

    void onSend(std::function<void(std::uint16_t, const CoherenceFrame &)> callback) {
        std::lock_guard lock(mutex_);
        callback_ = std::move(callback);
    }

    void failSends(bool throws = false) {
        std::lock_guard lock(mutex_);
        send_result_ = false;
        throw_on_send_ = throws;
    }

    std::vector<std::pair<std::uint16_t, CoherenceFrame>> waitFor(std::size_t count) const {
        std::unique_lock lock(mutex_);
        if (!changed_.wait_for(lock, kWait, [&] { return sent_.size() >= count; })) {
            std::cerr << "transport send wait timed out\n";
            std::_Exit(EXIT_FAILURE);
        }
        return sent_;
    }

    std::vector<std::pair<std::uint16_t, CoherenceFrame>> sent() const {
        std::lock_guard lock(mutex_);
        return sent_;
    }

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    std::vector<std::pair<std::uint16_t, CoherenceFrame>> sent_;
    std::function<void(std::uint16_t, const CoherenceFrame &)> callback_;
    bool send_result_{true};
    bool throw_on_send_{};
};

class AcceptingAuditSink final : public MesiTransactionEngine::AuditSink {
public:
    bool accept(const CoherenceAuditRecord &record) override {
        records.push_back(record);
        return true;
    }
    std::vector<CoherenceAuditRecord> records;
};

CoherenceFrame ackFor(const CoherenceFrame &snoop, const std::array<std::byte, 64> &dirty = {}) {
    auto ack = initializeFrame(Opcode::SnoopAck);
    setSrcHost(ack, dstHost(snoop));
    setDstHost(ack, srcHost(snoop));
    setSessionId(ack, sessionId(snoop));
    setSnoopId(ack, snoopId(snoop));
    setAddress(ack, address(snoop));
    setEpoch(ack, epoch(snoop));
    setStatus(ack, Status::Ok);
    setAckStrength(ack, AckStrength::MODEL);
    const auto snoop_opcode = opcode(snoop);
    const bool downgrade = snoop_opcode == Opcode::SnpDowngrade || snoop_opcode == Opcode::SnpDataDowngrade;
    setLineState(ack, downgrade ? LineState::S : LineState::I);
    if (snoop_opcode == Opcode::SnpDataInv || snoop_opcode == Opcode::SnpDataDowngrade) {
        setPayloadLength(ack, kLineSize);
        std::transform(dirty.begin(), dirty.end(), ack.data.begin(),
                       [](std::byte value) { return static_cast<std::uint8_t>(value); });
    }
    return ack;
}

CoherenceFrame requestFrame(Opcode opcode_value, std::uint64_t request_id, SessionId session_id,
                            std::uint16_t host_id) {
    auto frame = initializeFrame(opcode_value);
    setRequestId(frame, request_id);
    setSessionId(frame, session_id);
    setSrcHost(frame, host_id);
    setDstHost(frame, kServerHost);
    return frame;
}

CoherenceFrame responseFrame(const CoherenceFrame &request) {
    auto response = initializeFrame(Opcode::Response);
    setRequestId(response, requestId(request));
    setSessionId(response, sessionId(request));
    setSrcHost(response, kServerHost);
    setDstHost(response, srcHost(request));
    setStatus(response, Status::Ok);
    setAddress(response, address(request));
    const auto request_opcode = opcode(request);
    if (request_opcode == Opcode::Getm || request_opcode == Opcode::AtomicFaa || request_opcode == Opcode::AtomicCas) {
        setLineState(response, LineState::M);
        setEpoch(response, epoch(request) + 1);
        setPayloadLength(response, kLineSize);
    } else if (request_opcode == Opcode::Putm) {
        setLineState(response, LineState::I);
        setEpoch(response, epoch(request) + 1);
    }
    return response;
}

RegistrationRequest registration(std::uint16_t host) {
    return {host, 0, static_cast<std::uint64_t>(Capability::MODEL_SNOOP), 256 * 1024, 4, "test", {}};
}

template <typename Future> auto ready(Future &future, const char *context) {
    if (future.wait_for(kWait) != std::future_status::ready) {
        std::cerr << context << " timed out\n";
        std::_Exit(EXIT_FAILURE);
    }
    return future.get();
}

void checkState(const MesiDirectory &directory, std::uint64_t address, MesiState state,
                std::optional<std::uint16_t> owner, std::uint64_t sharers, std::uint64_t epoch, bool server_current) {
    const auto snapshot = directory.inspect(address);
    CHECK(snapshot.has_value());
    if (!snapshot)
        return;
    CHECK(snapshot->state == state);
    CHECK(snapshot->owner == owner);
    CHECK(snapshot->sharers == sharers);
    CHECK(snapshot->epoch == epoch);
    CHECK(snapshot->server_copy_current == server_current);
    CHECK(isValidSnapshot(*snapshot));
}

struct TransactionObservation {
    Status status{Status::InvalidState};
    TransitionResult transition;
    bool granted{};
    std::array<std::byte, 64> data{};
    std::uint64_t old_value{};
};

template <typename Result> TransactionObservation observeTransaction(const Result &result) {
    TransactionObservation observed{result.status, result.transition, result.granted, result.data, 0};
    if constexpr (requires { result.old_value; })
        observed.old_value = result.old_value;
    return observed;
}

template <typename Engine>
TransactionObservation task5Puts(Engine &engine, std::uint64_t address, TransactionRequest request,
                                 std::uint64_t epoch_value) {
    if constexpr (requires { engine.puts(address, request, epoch_value); })
        return observeTransaction(engine.puts(address, request, epoch_value));
    return {};
}

template <typename Engine>
constexpr bool hasTask5Putm = requires(Engine &engine, std::array<std::byte, 64> data) {
    engine.putm(kLineA, TransactionRequest{}, std::uint64_t{}, data);
};

template <typename Engine>
constexpr bool hasTask5Atomics = requires(Engine &engine) {
    engine.fetchAdd(kLineA, TransactionRequest{}, std::uint64_t{});
    engine.compareExchange(kLineA, TransactionRequest{}, std::uint64_t{}, std::uint64_t{});
};

template <typename Engine>
constexpr bool hasTask5Fence = requires(Engine &engine, EndpointSessionRegistry &registry, BindingId binding) {
    engine.fence(registry, SessionId{}, binding, std::uint64_t{});
};

template <typename Engine>
TransactionObservation task5Putm(Engine &engine, std::uint64_t address, TransactionRequest request,
                                 std::uint64_t epoch_value, const std::array<std::byte, 64> &data) {
    if constexpr (requires { engine.putm(address, request, epoch_value, data); })
        return observeTransaction(engine.putm(address, request, epoch_value, data));
    return {};
}

template <typename Engine>
TransactionObservation task5FetchAdd(Engine &engine, std::uint64_t address, TransactionRequest request,
                                     std::uint64_t value) {
    if constexpr (requires { engine.fetchAdd(address, request, value); })
        return observeTransaction(engine.fetchAdd(address, request, value));
    return {};
}

template <typename Engine>
TransactionObservation task5CompareExchange(Engine &engine, std::uint64_t address, TransactionRequest request,
                                            std::uint64_t expected, std::uint64_t desired) {
    if constexpr (requires { engine.compareExchange(address, request, expected, desired); })
        return observeTransaction(engine.compareExchange(address, request, expected, desired));
    return {};
}

template <typename Engine>
Status task5Fence(Engine &engine, EndpointSessionRegistry &registry, SessionId session, BindingId binding,
                  std::uint64_t request_id) {
    if constexpr (requires { engine.fence(registry, session, binding, request_id); })
        return engine.fence(registry, session, binding, request_id);
    return Status::InvalidState;
}

template <typename Engine>
Status task5Unregister(Engine &engine, EndpointSessionRegistry &registry, std::uint16_t host, SessionId session,
                       BindingId binding, const CoherenceFrame &request) {
    if constexpr (requires { engine.unregisterSession(registry, host, session, binding, request); })
        return engine.unregisterSession(registry, host, session, binding, request);
    return Status::InvalidState;
}

template <typename Registry>
bool task5Complete(Registry &registry, SessionId session, BindingId binding, std::uint64_t request_id) {
    if constexpr (requires { registry.completeOperation(session, binding, request_id); })
        return registry.completeOperation(session, binding, request_id);
    return false;
}

enum class TestFailurePolicy { RequireFenceAck, AssertProcessStopped, ForceDataLoss };
enum class TestAdministrativeStatus { Ok, FenceAckRequired, DirtyDataPresent, DataLoss, StaleSession, InvalidHost };

struct EvictionObservation {
    TestAdministrativeStatus status{TestAdministrativeStatus::InvalidHost};
    std::size_t clean_removed{};
    std::size_t dirty_lost{};
};

template <typename Engine>
EvictionObservation task5Evict(Engine &engine, EndpointSessionRegistry &registry, std::uint16_t host, SessionId session,
                               BindingId binding, TestFailurePolicy policy) {
    if constexpr (requires { typename Engine::HostFailurePolicy; }) {
        using Policy = typename Engine::HostFailurePolicy;
        Policy selected = Policy::RequireFenceAck;
        if (policy == TestFailurePolicy::AssertProcessStopped)
            selected = Policy::AssertProcessStopped;
        else if (policy == TestFailurePolicy::ForceDataLoss)
            selected = Policy::ForceDataLoss;
        const auto result = engine.evictHost(registry, host, session, binding, selected);
        EvictionObservation observed;
        using StatusType = std::remove_cvref_t<decltype(result.status)>;
        if (result.status == StatusType::Ok)
            observed.status = TestAdministrativeStatus::Ok;
        else if (result.status == StatusType::FenceAckRequired)
            observed.status = TestAdministrativeStatus::FenceAckRequired;
        else if (result.status == StatusType::DirtyDataPresent)
            observed.status = TestAdministrativeStatus::DirtyDataPresent;
        else if (result.status == StatusType::DataLoss)
            observed.status = TestAdministrativeStatus::DataLoss;
        else if (result.status == StatusType::StaleSession)
            observed.status = TestAdministrativeStatus::StaleSession;
        else
            observed.status = TestAdministrativeStatus::InvalidHost;
        observed.clean_removed = result.clean_removed;
        observed.dirty_lost = result.dirty_lost;
        return observed;
    }
    return {};
}

template <typename Engine>
AckDisposition task5ControlFrame(Engine &engine, EndpointSessionRegistry &registry, SessionId session,
                                 BindingId binding, const CoherenceFrame &frame) {
    if constexpr (requires { engine.handleControlFrame(registry, session, binding, frame); })
        return engine.handleControlFrame(registry, session, binding, frame);
    return AckDisposition::Invalid;
}

template <typename Registry>
bool task5AddClean(Registry &registry, SessionId session, BindingId binding, std::uint64_t address) {
    if constexpr (requires { registry.addCleanHolder(session, binding, address); })
        return registry.addCleanHolder(session, binding, address);
    else
        return registry.addCleanHolder(session, address);
}

template <typename Registry>
bool task5RemoveClean(Registry &registry, SessionId session, BindingId binding, std::uint64_t address) {
    if constexpr (requires { registry.removeCleanHolder(session, binding, address); })
        return registry.removeCleanHolder(session, binding, address);
    else
        return registry.removeCleanHolder(session, address);
}

template <typename Registry>
bool task5AddModified(Registry &registry, SessionId session, BindingId binding, std::uint64_t address) {
    if constexpr (requires { registry.addModifiedHolder(session, binding, address); })
        return registry.addModifiedHolder(session, binding, address);
    else
        return registry.addModifiedHolder(session, address);
}

template <typename Registry>
bool task5RemoveModified(Registry &registry, SessionId session, BindingId binding, std::uint64_t address) {
    if constexpr (requires { registry.removeModifiedHolder(session, binding, address); })
        return registry.removeModifiedHolder(session, binding, address);
    else
        return registry.removeModifiedHolder(session, address);
}

template <typename Registry>
std::vector<std::uint64_t> task5CleanHolders(Registry &registry, SessionId session, BindingId binding) {
    if constexpr (requires { registry.cleanHolders(session, binding); })
        return registry.cleanHolders(session, binding);
    else
        return registry.cleanHolders(session);
}

template <typename Registry, typename Token>
bool task5IdentityCanRemoveClean(Registry &registry, const Token &token, std::uint64_t address) {
    if constexpr (requires { registry.removeCleanHolder(token, address); })
        return registry.removeCleanHolder(token, address);
    return false;
}

struct AuditObservation {
    std::uint64_t timeout{};
    std::uint64_t partial_ack{};
    std::uint64_t forced_clean_removal{};
    std::uint64_t forced_dirty_loss{};
    std::uint64_t stale_ack{};
    std::uint64_t invalid_ownership{};
    std::size_t records{};
    bool has_high_severity_data_loss{};
};

template <typename Engine> AuditObservation task5Audit(const Engine &engine) {
    if constexpr (requires {
                      engine.auditCounters();
                      engine.auditRecords();
                  }) {
        const auto counters = engine.auditCounters();
        const auto records = engine.auditRecords();
        AuditObservation observed{counters.timeout,
                                  counters.partial_ack,
                                  counters.forced_clean_removal,
                                  counters.forced_dirty_loss,
                                  counters.stale_ack,
                                  counters.invalid_ownership,
                                  records.size(),
                                  false};
        for (const auto &record : records) {
            using Kind = std::remove_cvref_t<decltype(record.kind)>;
            using Severity = std::remove_cvref_t<decltype(record.severity)>;
            if (record.kind == Kind::ForcedDirtyLoss && record.severity == Severity::High)
                observed.has_high_severity_data_loss = true;
        }
        return observed;
    }
    return {};
}

void testCleanPutsFromSharedAndExclusive() {
    MesiDirectory directory;
    CHECK(directory.gets(kLineA, 1).committed());
    CHECK(directory.gets(kLineA, 2).committed());
    CHECK(directory.gets(kLineB, 3).committed());
    TestMemory memory;
    TestTransport transport;
    MesiTransactionEngine engine(directory, memory, transport, kWait);
    CHECK(engine.bindSession(1, 101));
    CHECK(engine.bindSession(2, 102));
    CHECK(engine.bindSession(3, 103));

    const auto remove_shared = task5Puts(engine, kLineA, {1, 101, 1}, 2);
    CHECK(remove_shared.status == Status::Ok);
    CHECK(remove_shared.transition.committed());
    checkState(directory, kLineA, MesiState::S, std::nullopt, holder(2), 3, true);

    const auto remove_last = task5Puts(engine, kLineA, {2, 102, 1}, 2);
    CHECK(remove_last.status == Status::Ok);
    checkState(directory, kLineA, MesiState::I, std::nullopt, 0, 4, true);

    const auto remove_exclusive = task5Puts(engine, kLineB, {3, 103, 1}, 1);
    CHECK(remove_exclusive.status == Status::Ok);
    checkState(directory, kLineB, MesiState::I, std::nullopt, 0, 2, true);

    const auto non_holder = task5Puts(engine, kLineB, {1, 101, 2}, 2);
    CHECK(non_holder.status == Status::InvalidState);
}

void testPutmPersistsBeforeOwnerRemovalAndRejectsInvalidOwnership() {
    if constexpr (!hasTask5Putm<MesiTransactionEngine>) {
        CHECK(hasTask5Putm<MesiTransactionEngine>);
        return;
    }
    MesiDirectory directory;
    CHECK(directory.getm(kLineA, 1).committed());
    TestMemory memory;
    TestTransport transport;
    MesiTransactionEngine engine(directory, memory, transport, kWait);
    CHECK(engine.bindSession(1, 101));
    CHECK(engine.bindSession(2, 102));
    const auto dirty = bytes(0xa5);
    memory.blockWrites();

    auto writeback = std::async(std::launch::async, [&] { return task5Putm(engine, kLineA, {1, 101, 1}, 1, dirty); });
    memory.waitForWrite();
    auto observer = std::async(std::launch::async, [&] { return directory.inspect(kLineA); });
    CHECK(observer.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout);
    CHECK(writeback.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout);
    memory.releaseWrites();
    const auto result = ready(writeback, "PUTM persistence");
    CHECK(ready(observer, "PUTM metadata observer").has_value());
    CHECK(result.status == Status::Ok);
    CHECK(memory.line(kLineA) == dirty);
    checkState(directory, kLineA, MesiState::I, std::nullopt, 0, 2, true);

    CHECK(directory.getm(kLineB, 1).committed());
    const auto writes_before = memory.writes();
    const auto non_owner = task5Putm(engine, kLineB, {2, 102, 1}, 1, bytes(0xbb));
    const auto wrong_epoch = task5Putm(engine, kLineB, {1, 101, 2}, 99, bytes(0xcc));
    CHECK(non_owner.status == Status::InvalidState);
    CHECK(wrong_epoch.status == Status::StaleEpoch);
    CHECK(memory.writes() == writes_before);
    checkState(directory, kLineB, MesiState::M, 1, 0, 1, false);
    CHECK(task5Audit(engine).invalid_ownership >= 1);
}

void testCasAndFetchAddSerializeAfterExclusiveOwnershipAndReturnUpdatedLine() {
    if constexpr (!hasTask5Atomics<MesiTransactionEngine>) {
        CHECK(hasTask5Atomics<MesiTransactionEngine>);
        return;
    }
    MesiDirectory directory;
    TestMemory memory;
    auto initial = bytes(0x11);
    storeScalar(initial, 8, 10);
    memory.seed(kLineA, initial);
    TestTransport transport;
    MesiTransactionEngine engine(directory, memory, transport, kWait);
    CHECK(engine.bindSession(1, 101));
    CHECK(engine.bindSession(2, 102));

    const auto cas = task5CompareExchange(engine, kLineA + 8, {1, 101, 1}, 10, 42);
    CHECK(cas.status == Status::Ok);
    CHECK(cas.granted);
    CHECK(cas.old_value == 10);
    CHECK(loadScalar(cas.data, 8) == 42);
    CHECK(memory.line(kLineA) == cas.data);
    checkState(directory, kLineA, MesiState::M, 1, 0, 1, false);

    transport.onSend([&](std::uint16_t, const CoherenceFrame &snoop) {
        CHECK(opcode(snoop) == Opcode::SnpDataInv);
        CHECK(engine.handleSnoopAck(ackFor(snoop, cas.data)) == AckDisposition::Deferred);
    });
    const auto faa = task5FetchAdd(engine, kLineA + 8, {2, 102, 1}, 5);
    CHECK(faa.status == Status::Ok);
    CHECK(faa.granted);
    CHECK(faa.old_value == 42);
    CHECK(loadScalar(faa.data, 8) == 47);
    CHECK(memory.line(kLineA) == faa.data);
    checkState(directory, kLineA, MesiState::M, 2, 0, 2, false);
    CHECK(transport.sent().size() == 1);

    transport.onSend([&](std::uint16_t, const CoherenceFrame &snoop) {
        CHECK(opcode(snoop) == Opcode::SnpDataInv);
        CHECK(engine.handleSnoopAck(ackFor(snoop, faa.data)) == AckDisposition::Deferred);
    });
    const auto owner_cas = task5CompareExchange(engine, kLineA + 8, {2, 102, 2}, 47, 50);
    CHECK(owner_cas.status == Status::Ok);
    CHECK(owner_cas.old_value == 47);
    CHECK(loadScalar(owner_cas.data, 8) == 50);
    CHECK(memory.line(kLineA) == owner_cas.data);
    checkState(directory, kLineA, MesiState::M, 2, 0, 3, false);

    const auto counters = directory.transitionCounters();
    CHECK(counters.atomic == 3);
    CHECK(counters.getm == 0);

    const auto unaligned = task5FetchAdd(engine, kLineA + 4, {1, 101, 2}, 1);
    CHECK(unaligned.status == Status::InvalidState);
    CHECK(loadScalar(memory.line(kLineA), 8) == 50);
}

void testAtomicWaitsForConflictingExclusiveAckBeforeUpdatingMemory() {
    if constexpr (!hasTask5Atomics<MesiTransactionEngine>) {
        CHECK(hasTask5Atomics<MesiTransactionEngine>);
        return;
    }
    MesiDirectory directory;
    CHECK(directory.gets(kLineA, 1).committed());
    TestMemory memory;
    auto initial = bytes(0x22);
    storeScalar(initial, 16, 7);
    memory.seed(kLineA, initial);
    TestTransport transport;
    MesiTransactionEngine engine(directory, memory, transport, kWait);
    CHECK(engine.bindSession(1, 101));
    CHECK(engine.bindSession(2, 102));

    auto atomic = std::async(std::launch::async, [&] { return task5FetchAdd(engine, kLineA + 16, {2, 102, 1}, 9); });
    const auto sent = transport.waitFor(1);
    CHECK(opcode(sent[0].second) == Opcode::SnpInv);
    CHECK(memory.writes() == 0);
    CHECK(atomic.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout);
    CHECK(engine.handleSnoopAck(ackFor(sent[0].second)) == AckDisposition::Accepted);
    const auto result = ready(atomic, "atomic exclusive acquisition");
    CHECK(result.status == Status::Ok);
    CHECK(result.old_value == 7);
    CHECK(loadScalar(result.data, 16) == 16);
    checkState(directory, kLineA, MesiState::M, 2, 0, 2, false);
}

void testFenceWaitsOnlyForEarlierSameSessionWorkAndModifiedDrain() {
    if constexpr (!hasTask5Fence<MesiTransactionEngine>) {
        CHECK(hasTask5Fence<MesiTransactionEngine>);
        return;
    }
    MesiDirectory directory;
    TestMemory memory;
    TestTransport transport;
    MesiTransactionEngine engine(directory, memory, transport, kWait);
    EndpointSessionRegistry registry;
    const auto first = registry.registerEndpoint(registration(6));
    const auto other = registry.registerEndpoint(registration(7));
    CHECK(first.status == Status::Ok);
    CHECK(other.status == Status::Ok);
    const auto earlier = requestFrame(Opcode::Heartbeat, 1, first.session_id, 6);
    const auto fence = requestFrame(Opcode::Fence, 2, first.session_id, 6);
    const auto other_fence = requestFrame(Opcode::Fence, 1, other.session_id, 7);
    CHECK(registry.admitRequest(first.session_id, first.binding_id, earlier) == RequestAdmissionResult::Accepted);
    CHECK(registry.admitRequest(first.session_id, first.binding_id, fence) == RequestAdmissionResult::Accepted);
    CHECK(registry.admitRequest(other.session_id, other.binding_id, other_fence) == RequestAdmissionResult::Accepted);
    CHECK(task5AddModified(registry, first.session_id, first.binding_id, kLineA));

    std::promise<void> started;
    auto started_future = started.get_future();
    auto blocked = std::async(std::launch::async, [&] {
        started.set_value();
        return task5Fence(engine, registry, first.session_id, first.binding_id, 2);
    });
    ready(started_future, "fence start");
    CHECK(blocked.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout);

    CHECK(task5Fence(engine, registry, other.session_id, other.binding_id, 1) == Status::Ok);
    CHECK(task5Complete(registry, first.session_id, first.binding_id, 1));
    CHECK(blocked.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout);
    CHECK(task5RemoveModified(registry, first.session_id, first.binding_id, kLineA));
    CHECK(ready(blocked, "same-session fence drain") == Status::Ok);
    CHECK(task5Fence(engine, registry, first.session_id, first.binding_id, 1) == Status::InvalidState);

    const auto cancelled = registry.registerEndpoint(registration(18));
    CHECK(cancelled.status == Status::Ok);
    const auto cancelled_earlier = requestFrame(Opcode::Heartbeat, 1, cancelled.session_id, 18);
    const auto cancelled_fence = requestFrame(Opcode::Fence, 2, cancelled.session_id, 18);
    CHECK(registry.admitRequest(cancelled.session_id, cancelled.binding_id, cancelled_earlier) ==
          RequestAdmissionResult::Accepted);
    CHECK(registry.admitRequest(cancelled.session_id, cancelled.binding_id, cancelled_fence) ==
          RequestAdmissionResult::Accepted);
    auto cancelled_wait = std::async(std::launch::async, [&] {
        return task5Fence(engine, registry, cancelled.session_id, cancelled.binding_id, 2);
    });
    CHECK(cancelled_wait.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout);
    CHECK(registry.disconnectAbruptly(18, cancelled.session_id, cancelled.binding_id));
    const auto cancelled_promptly =
        cancelled_wait.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready;
    CHECK(cancelled_promptly);
    if (!cancelled_promptly) {
        auto resume = registration(18);
        resume.requested_session_id = cancelled.session_id;
        const auto resumed = registry.registerEndpoint(resume);
        CHECK(resumed.status == Status::Ok);
        CHECK(task5Complete(registry, resumed.session_id, resumed.binding_id, 1));
    }
    CHECK(ready(cancelled_wait, "retired-binding fence") == Status::StaleSession);
}

void testGracefulUnregisterRevalidatesCleanCandidatesAndRejectsModifiedOwner() {
    MesiDirectory directory;
    CHECK(directory.gets(kLineA, 8).committed());
    CHECK(directory.gets(kLineA, 9).committed());
    CHECK(directory.gets(kLineB, 8).committed());
    TestMemory memory;
    TestTransport transport;
    MesiTransactionEngine engine(directory, memory, transport, kWait);
    EndpointSessionRegistry registry;
    const auto clean = registry.registerEndpoint(registration(8));
    CHECK(clean.status == Status::Ok);
    CHECK(task5AddClean(registry, clean.session_id, clean.binding_id, kLineA));
    CHECK(task5AddClean(registry, clean.session_id, clean.binding_id, kLineB));
    const auto unregister = requestFrame(Opcode::Unregister, 1, clean.session_id, 8);
    CHECK(registry.admitRequest(clean.session_id, clean.binding_id, unregister) == RequestAdmissionResult::Accepted);
    CHECK(task5Unregister(engine, registry, 8, clean.session_id, clean.binding_id, unregister) == Status::Ok);
    checkState(directory, kLineA, MesiState::S, std::nullopt, holder(9), 3, true);
    checkState(directory, kLineB, MesiState::I, std::nullopt, 0, 2, true);
    CHECK(task5CleanHolders(registry, clean.session_id, clean.binding_id).empty());
    CHECK(registry.inspect(clean.session_id)->state == SessionState::Closed);

    CHECK(directory.getm(kLineC, 10).committed());
    const auto dirty = registry.registerEndpoint(registration(10));
    CHECK(dirty.status == Status::Ok);
    CHECK(task5AddModified(registry, dirty.session_id, dirty.binding_id, kLineC));
    const auto dirty_unregister = requestFrame(Opcode::Unregister, 1, dirty.session_id, 10);
    CHECK(registry.admitRequest(dirty.session_id, dirty.binding_id, dirty_unregister) ==
          RequestAdmissionResult::Accepted);
    CHECK(task5Unregister(engine, registry, 10, dirty.session_id, dirty.binding_id, dirty_unregister) ==
          Status::InvalidState);
    checkState(directory, kLineC, MesiState::M, 10, 0, 1, false);
    CHECK(registry.inspect(dirty.session_id)->state == SessionState::Active);

    CHECK(directory.gets(kLineB, 19).committed());
    EndpointSessionRegistry forged_registry;
    const auto forged_session = forged_registry.registerEndpoint(registration(19));
    CHECK(forged_session.status == Status::Ok);
    CHECK(task5AddClean(forged_registry, forged_session.session_id, forged_session.binding_id, kLineB));
    const auto admitted_unregister = requestFrame(Opcode::Unregister, 1, forged_session.session_id, 19);
    CHECK(forged_registry.admitRequest(forged_session.session_id, forged_session.binding_id, admitted_unregister) ==
          RequestAdmissionResult::Accepted);
    auto forged_unregister = admitted_unregister;
    setSrcHost(forged_unregister, 20);
    CHECK(task5Unregister(engine, forged_registry, 20, forged_session.session_id, forged_session.binding_id,
                          forged_unregister) == Status::InvalidState);
    CHECK(task5CleanHolders(forged_registry, forged_session.session_id, forged_session.binding_id) ==
          std::vector<std::uint64_t>{kLineB});
    checkState(directory, kLineB, MesiState::E, 19, 0, 3, true);
}

void testHostFenceAckAndStoppedAssertionGateCleanRemoval() {
    MesiDirectory directory;
    CHECK(directory.gets(kLineA, 11).committed());
    CHECK(directory.gets(kLineB, 12).committed());
    TestMemory memory;
    TestTransport transport;
    MesiTransactionEngine engine(directory, memory, transport, kWait);
    EndpointSessionRegistry registry;
    const auto fenced = registry.registerEndpoint(registration(11));
    const auto stopped = registry.registerEndpoint(registration(12));
    CHECK(task5AddClean(registry, fenced.session_id, fenced.binding_id, kLineA));
    CHECK(task5AddClean(registry, stopped.session_id, stopped.binding_id, kLineB));

    const auto missing_ack =
        task5Evict(engine, registry, 11, fenced.session_id, fenced.binding_id, TestFailurePolicy::RequireFenceAck);
    CHECK(missing_ack.status == TestAdministrativeStatus::FenceAckRequired);
    checkState(directory, kLineA, MesiState::E, 11, 0, 1, true);
    CHECK(registry.inspect(fenced.session_id)->state == SessionState::Fenced);

    transport.onSend([&](std::uint16_t, const CoherenceFrame &snoop) {
        CHECK(opcode(snoop) == Opcode::HostFence);
        CHECK(task5ControlFrame(engine, registry, fenced.session_id, fenced.binding_id, ackFor(snoop)) ==
              AckDisposition::Accepted);
    });
    const auto acknowledged =
        task5Evict(engine, registry, 11, fenced.session_id, fenced.binding_id, TestFailurePolicy::RequireFenceAck);
    CHECK(acknowledged.status == TestAdministrativeStatus::Ok);
    CHECK(acknowledged.clean_removed == 1);
    checkState(directory, kLineA, MesiState::I, std::nullopt, 0, 2, true);

    const auto asserted = task5Evict(engine, registry, 12, stopped.session_id, stopped.binding_id,
                                     TestFailurePolicy::AssertProcessStopped);
    CHECK(asserted.status == TestAdministrativeStatus::Ok);
    CHECK(asserted.clean_removed == 1);
    checkState(directory, kLineB, MesiState::I, std::nullopt, 0, 2, true);
    CHECK(task5Audit(engine).forced_clean_removal == 2);
}

void testForcedDirtyLossIsExplicitRecordedAndNeverCoherentSuccess() {
    MesiDirectory directory;
    CHECK(directory.getm(kLineA, 13).committed());
    CHECK(directory.getm(kLineB, 14).committed());
    TestMemory memory;
    memory.seed(kLineA, bytes(0x31));
    memory.seed(kLineB, bytes(0x42));
    TestTransport transport;
    AcceptingAuditSink audit_sink;
    MesiTransactionEngine engine(directory, memory, transport, kWait, 1, &audit_sink);
    EndpointSessionRegistry registry;
    const auto normal = registry.registerEndpoint(registration(13));
    const auto forced = registry.registerEndpoint(registration(14));
    CHECK(task5AddModified(registry, normal.session_id, normal.binding_id, kLineA));
    CHECK(task5AddModified(registry, forced.session_id, forced.binding_id, kLineB));

    const auto rejected =
        task5Evict(engine, registry, 13, normal.session_id, normal.binding_id, TestFailurePolicy::AssertProcessStopped);
    CHECK(rejected.status == TestAdministrativeStatus::DirtyDataPresent);
    checkState(directory, kLineA, MesiState::M, 13, 0, 1, false);

    CHECK(registry.disconnectAbruptly(14, forced.session_id, forced.binding_id));
    const auto lost =
        task5Evict(engine, registry, 14, forced.session_id, BindingId{}, TestFailurePolicy::ForceDataLoss);
    CHECK(lost.status == TestAdministrativeStatus::DataLoss);
    CHECK(lost.status != TestAdministrativeStatus::Ok);
    CHECK(lost.dirty_lost == 1);
    CHECK(memory.line(kLineB) == bytes(0x42));
    checkState(directory, kLineB, MesiState::I, std::nullopt, 0, 2, true);
    const auto audit = task5Audit(engine);
    CHECK(audit.forced_dirty_loss == 1);
    CHECK(audit.has_high_severity_data_loss);

    const auto replacement = registry.registerEndpoint(registration(14));
    CHECK(replacement.status == Status::Ok);
    CHECK(replacement.session_id != forced.session_id);
}

void testHostFenceWakesTransactionsWaitingOnThatHost() {
    MesiDirectory directory;
    CHECK(directory.gets(kLineA, 20).committed());
    CHECK(directory.gets(kLineA, 21).committed());
    TestMemory memory;
    TestTransport transport;
    MesiTransactionEngine engine(directory, memory, transport, kWait);
    EndpointSessionRegistry registry;
    const auto failed = registry.registerEndpoint(registration(20));
    CHECK(failed.status == Status::Ok);
    CHECK(engine.bindSession(20, failed.session_id));
    CHECK(engine.bindSession(21, 121));
    CHECK(engine.bindSession(22, 122));
    CHECK(task5AddClean(registry, failed.session_id, failed.binding_id, kLineA));

    auto transaction = std::async(std::launch::async, [&] { return engine.getm(kLineA, {22, 122, 1}); });
    CHECK(transport.waitFor(2).size() == 2);
    const auto eviction =
        task5Evict(engine, registry, 20, failed.session_id, failed.binding_id, TestFailurePolicy::RequireFenceAck);
    CHECK(eviction.status == TestAdministrativeStatus::FenceAckRequired);
    const auto promptly_woken = transaction.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready;
    CHECK(promptly_woken);
    if (!promptly_woken)
        CHECK(engine.progress(MesiTransactionEngine::TimePoint::max()) == 1);
    const auto result = ready(transaction, "host-fence transaction wake");
    CHECK(result.status == Status::HostFenced);
}

void testTimeoutPartialAckStaleAckAndInvalidOwnershipAreAudited() {
    MesiDirectory directory;
    CHECK(directory.gets(kLineA, 1).committed());
    CHECK(directory.gets(kLineA, 2).committed());
    TestMemory memory;
    memory.seed(kLineA, bytes(0x55));
    TestTransport transport;
    MesiTransactionEngine engine(directory, memory, transport, kWait);
    CHECK(engine.bindSession(1, 101));
    CHECK(engine.bindSession(2, 102));
    CHECK(engine.bindSession(3, 103));

    auto transaction = std::async(std::launch::async, [&] { return engine.getm(kLineA, {3, 103, 1}); });
    const auto sent = transport.waitFor(2);
    CHECK(engine.handleSnoopAck(ackFor(sent[0].second)) == AckDisposition::Accepted);
    CHECK(engine.progress(MesiTransactionEngine::TimePoint::max()) == 1);
    const auto result = ready(transaction, "partial timeout audit");
    CHECK(result.status == Status::CoherenceTimeout);
    CHECK(engine.handleSnoopAck(ackFor(sent[1].second)) == AckDisposition::Stale);
    const auto audit = task5Audit(engine);
    CHECK(audit.timeout == 1);
    CHECK(audit.partial_ack == 1);
    CHECK(audit.stale_ack >= 1);
    CHECK(audit.records >= 3);
}

void testAcceptedSnoopAckAuditCounterExcludesRejectedAcks() {
    MesiDirectory directory;
    CHECK(directory.gets(kLineA, 1).committed());
    CHECK(directory.gets(kLineA, 2).committed());
    TestMemory memory;
    memory.seed(kLineA, bytes(0x55));
    TestTransport transport;
    MesiTransactionEngine engine(directory, memory, transport, kWait);
    CHECK(engine.bindSession(1, 101));
    CHECK(engine.bindSession(2, 102));
    CHECK(engine.bindSession(3, 103));

    auto transaction = std::async(std::launch::async, [&] { return engine.getm(kLineA, {3, 103, 1}); });
    const auto sent = transport.waitFor(2);
    CHECK(engine.auditCounters().accepted_snoop_ack == 0);

    auto malformed = ackFor(sent[0].second);
    setAckStrength(malformed, AckStrength::NONE);
    CHECK(engine.handleSnoopAck(malformed) == AckDisposition::Invalid);
    CHECK(engine.auditCounters().accepted_snoop_ack == 0);

    auto wrong_host = ackFor(sent[0].second);
    setSrcHost(wrong_host, srcHost(wrong_host) + 1);
    CHECK(engine.handleSnoopAck(wrong_host) == AckDisposition::Stale);
    CHECK(engine.auditCounters().accepted_snoop_ack == 0);

    auto stale_epoch = ackFor(sent[0].second);
    setEpoch(stale_epoch, epoch(stale_epoch) - 1);
    CHECK(engine.handleSnoopAck(stale_epoch) == AckDisposition::Stale);
    CHECK(engine.auditCounters().accepted_snoop_ack == 0);

    const auto first = ackFor(sent[0].second);
    CHECK(engine.handleSnoopAck(first) == AckDisposition::Accepted);
    CHECK(engine.auditCounters().accepted_snoop_ack == 1);
    CHECK(engine.handleSnoopAck(first) == AckDisposition::Duplicate);
    CHECK(engine.auditCounters().accepted_snoop_ack == 1);

    CHECK(engine.handleSnoopAck(ackFor(sent[1].second)) == AckDisposition::Accepted);
    CHECK(engine.auditCounters().accepted_snoop_ack == 2);
    CHECK(ready(transaction, "accepted snoop ACK audit").status == Status::Ok);

    MesiDirectory dirty_directory;
    CHECK(dirty_directory.getm(kLineB, 4).committed());
    TestMemory dirty_memory;
    dirty_memory.seed(kLineB, bytes(0x66));
    dirty_memory.blockWrites();
    TestTransport dirty_transport;
    MesiTransactionEngine dirty_engine(dirty_directory, dirty_memory, dirty_transport, kWait);
    CHECK(dirty_engine.bindSession(4, 104));
    CHECK(dirty_engine.bindSession(5, 105));

    auto dirty_transaction = std::async(std::launch::async, [&] { return dirty_engine.getm(kLineB, {5, 105, 1}); });
    const auto dirty_snoop = dirty_transport.waitFor(1).front().second;
    CHECK(opcode(dirty_snoop) == Opcode::SnpDataInv);
    const auto dirty_ack = ackFor(dirty_snoop, bytes(0xa5));
    CHECK(dirty_engine.handleSnoopAck(dirty_ack) == AckDisposition::Deferred);
    CHECK(dirty_engine.auditCounters().accepted_snoop_ack == 1);
    dirty_memory.waitForWrite();
    CHECK(dirty_engine.handleSnoopAck(dirty_ack) == AckDisposition::Deferred);
    CHECK(dirty_engine.auditCounters().accepted_snoop_ack == 1);
    dirty_memory.releaseWrites();
    CHECK(ready(dirty_transaction, "accepted dirty snoop ACK audit").status == Status::Ok);
}

void testHostFenceAuthorizationIsContextualOneShotAndFailClosed() {
    const auto run = [](auto mutate_ack, bool send_failure = false) {
        MesiDirectory directory;
        CHECK(directory.gets(kLineA, 23).committed());
        TestMemory memory;
        TestTransport transport;
        MesiTransactionEngine engine(directory, memory, transport, std::chrono::milliseconds(20));
        EndpointSessionRegistry registry;
        const auto registered = registry.registerEndpoint(registration(23));
        CHECK(registered.status == Status::Ok);
        CHECK(engine.bindSession(23, registered.session_id));
        CHECK(task5AddClean(registry, registered.session_id, registered.binding_id, kLineA));
        if (send_failure)
            transport.failSends();
        transport.onSend([&](std::uint16_t host, const CoherenceFrame &fence) {
            CHECK(host == 23);
            CHECK(opcode(fence) == Opcode::HostFence);
            CHECK(snoopId(fence) != 0);
            auto ack = ackFor(fence);
            mutate_ack(ack);
            (void)task5ControlFrame(engine, registry, registered.session_id, registered.binding_id, ack);
        });
        const auto result = task5Evict(engine, registry, 23, registered.session_id, registered.binding_id,
                                       TestFailurePolicy::RequireFenceAck);
        if (result.status != TestAdministrativeStatus::Ok) {
            checkState(directory, kLineA, MesiState::E, 23, 0, 1, true);
            CHECK(task5CleanHolders(registry, registered.session_id, registered.binding_id) ==
                  std::vector<std::uint64_t>{kLineA});
        }
        return std::pair{result, transport.sent()};
    };

    const auto missing = run([](CoherenceFrame &ack) { setSnoopId(ack, snoopId(ack) + 1); });
    CHECK(missing.first.status != TestAdministrativeStatus::Ok);
    CHECK(missing.second.size() == 1);
    CHECK(run([](CoherenceFrame &ack) { setSrcHost(ack, 24); }).first.status != TestAdministrativeStatus::Ok);
    CHECK(run([](CoherenceFrame &ack) { setSessionId(ack, sessionId(ack) + 1); }).first.status !=
          TestAdministrativeStatus::Ok);
    CHECK(run([](CoherenceFrame &ack) { setAckStrength(ack, AckStrength::NONE); }).first.status !=
          TestAdministrativeStatus::Ok);
    CHECK(run([](CoherenceFrame &) {}, true).first.status != TestAdministrativeStatus::Ok);

    {
        MesiDirectory disconnected_directory;
        CHECK(disconnected_directory.gets(kLineA, 23).committed());
        TestMemory disconnected_memory;
        TestTransport disconnected_transport;
        MesiTransactionEngine disconnected_engine(disconnected_directory, disconnected_memory, disconnected_transport,
                                                  std::chrono::milliseconds(20));
        EndpointSessionRegistry disconnected_registry;
        const auto disconnected = disconnected_registry.registerEndpoint(registration(23));
        CHECK(disconnected_engine.bindSession(23, disconnected.session_id));
        CHECK(task5AddClean(disconnected_registry, disconnected.session_id, disconnected.binding_id, kLineA));
        disconnected_transport.onSend([&](std::uint16_t, const CoherenceFrame &fence) {
            CHECK(opcode(fence) == Opcode::HostFence);
            CHECK(disconnected_registry.disconnectAbruptly(23, disconnected.session_id, disconnected.binding_id));
            CHECK(task5ControlFrame(disconnected_engine, disconnected_registry, disconnected.session_id,
                                    disconnected.binding_id, ackFor(fence)) != AckDisposition::Accepted);
        });
        CHECK(task5Evict(disconnected_engine, disconnected_registry, 23, disconnected.session_id,
                         disconnected.binding_id, TestFailurePolicy::RequireFenceAck)
                  .status != TestAdministrativeStatus::Ok);
        checkState(disconnected_directory, kLineA, MesiState::E, 23, 0, 1, true);
    }

    MesiDirectory directory;
    CHECK(directory.gets(kLineB, 24).committed());
    TestMemory memory;
    TestTransport transport;
    MesiTransactionEngine engine(directory, memory, transport, kWait);
    EndpointSessionRegistry registry;
    const auto live = registry.registerEndpoint(registration(24));
    CHECK(engine.bindSession(24, live.session_id));
    CHECK(task5AddClean(registry, live.session_id, live.binding_id, kLineB));
    CoherenceFrame accepted_ack{};
    transport.onSend([&](std::uint16_t, const CoherenceFrame &fence) {
        const auto snoop_ack_before = engine.auditCounters().accepted_snoop_ack;
        accepted_ack = ackFor(fence);
        CHECK(task5ControlFrame(engine, registry, live.session_id, live.binding_id, accepted_ack) ==
              AckDisposition::Accepted);
        CHECK(engine.auditCounters().accepted_snoop_ack == snoop_ack_before);
        const auto stale_before = engine.auditCounters().stale_ack;
        CHECK(task5ControlFrame(engine, registry, live.session_id, live.binding_id, accepted_ack) ==
              AckDisposition::Stale);
        CHECK(engine.auditCounters().stale_ack == stale_before + 1);
    });
    CHECK(
        task5Evict(engine, registry, 24, live.session_id, live.binding_id, TestFailurePolicy::RequireFenceAck).status ==
        TestAdministrativeStatus::Ok);
    CHECK(task5ControlFrame(engine, registry, live.session_id, live.binding_id, accepted_ack) !=
          AckDisposition::Accepted);
}

void testFencedOrdinaryAdmissionIsBoundedAndControlUsesContext() {
    EndpointSessionRegistry registry;
    const auto live = registry.registerEndpoint(registration(25));
    CHECK(live.status == Status::Ok);
    CHECK(registry.fenceSession(25, live.session_id, live.binding_id) == Status::Ok);

    auto putm = requestFrame(Opcode::Putm, 1, live.session_id, 25);
    setAddress(putm, kLineA);
    setEpoch(putm, 1);
    setLineState(putm, LineState::M);
    setPayloadLength(putm, kLineSize);
    putm.data.fill(0x5a);
    CHECK(registry.admitOperation(live.session_id, live.binding_id, putm).result == RequestAdmissionResult::Accepted);
    CHECK(
        registry.admitRequest(live.session_id, live.binding_id, requestFrame(Opcode::Fence, 2, live.session_id, 25)) ==
        RequestAdmissionResult::Accepted);
    CHECK(registry.admitRequest(live.session_id, live.binding_id,
                                requestFrame(Opcode::Heartbeat, 3, live.session_id, 25)) ==
          RequestAdmissionResult::Accepted);
    for (const auto opcode_value : {Opcode::Gets, Opcode::Getm, Opcode::Upgrade, Opcode::AtomicFaa, Opcode::AtomicCas,
                                    Opcode::Puts, Opcode::Unregister}) {
        auto rejected = requestFrame(opcode_value, 4, live.session_id, 25);
        if (opcode_value == Opcode::Gets || opcode_value == Opcode::Getm || opcode_value == Opcode::Upgrade ||
            opcode_value == Opcode::Puts)
            setAddress(rejected, kLineA);
        if (opcode_value == Opcode::AtomicFaa || opcode_value == Opcode::AtomicCas) {
            setAddress(rejected, kLineA);
            setSize(rejected, 8);
            setValue(rejected, 1);
        }
        CHECK(registry.admitRequest(live.session_id, live.binding_id, rejected) ==
              RequestAdmissionResult::InvalidRequest);
    }
    auto ordinary_ack = initializeFrame(Opcode::SnoopAck);
    setSrcHost(ordinary_ack, 25);
    setDstHost(ordinary_ack, kServerHost);
    setSessionId(ordinary_ack, live.session_id);
    setSnoopId(ordinary_ack, 7);
    setAckStrength(ordinary_ack, AckStrength::MODEL);
    CHECK(registry.admitRequest(live.session_id, live.binding_id, ordinary_ack) ==
          RequestAdmissionResult::InvalidRequest);
    CHECK(registry.sealFencedSession(25, live.session_id, live.binding_id).has_value());
    CHECK(registry.fenceSession(25, live.session_id, live.binding_id) == Status::Ok);
    CHECK(registry.admitRequest(live.session_id, live.binding_id,
                                requestFrame(Opcode::Heartbeat, 4, live.session_id, 25)) ==
          RequestAdmissionResult::InvalidRequest);

    EndpointSessionRegistry prebarrier_registry;
    const auto prebarrier = prebarrier_registry.registerEndpoint(registration(37));
    CHECK(task5AddClean(prebarrier_registry, prebarrier.session_id, prebarrier.binding_id, kLineB));
    CHECK(prebarrier_registry.fenceSession(37, prebarrier.session_id, prebarrier.binding_id) == Status::Ok);
    const auto prebarrier_generation =
        prebarrier_registry.captureGeneration(37, prebarrier.session_id, prebarrier.binding_id);
    CHECK(prebarrier_generation.has_value());
    CHECK(prebarrier_registry.sealFencedSession(*prebarrier_generation).has_value());
    CHECK(prebarrier_registry.disconnectAbruptly(37, prebarrier.session_id, prebarrier.binding_id));
    CHECK(!task5IdentityCanRemoveClean(prebarrier_registry, *prebarrier_generation, kLineB));

    MesiDirectory directory;
    CHECK(directory.gets(kLineA, 33).committed());
    TestMemory memory;
    memory.seed(kLineA, bytes(0x6a));
    TestTransport transport;
    MesiTransactionEngine engine(directory, memory, transport, kWait);
    EndpointSessionRegistry control_registry;
    const auto target = control_registry.registerEndpoint(registration(33));
    CHECK(engine.bindSession(33, target.session_id));
    CHECK(engine.bindSession(34, 134));
    auto transaction = std::async(std::launch::async, [&] { return engine.getm(kLineA, {34, 134, 1}); });
    const auto snoop = transport.waitFor(1).front().second;
    CHECK(control_registry.fenceSession(33, target.session_id, target.binding_id) == Status::Ok);
    CHECK(task5ControlFrame(engine, control_registry, target.session_id, target.binding_id, ackFor(snoop)) ==
          AckDisposition::Accepted);
    CHECK(ready(transaction, "fenced contextual line ACK").status == Status::Ok);
}

void testEvictionWaitsForAdmittedAtomicHolderPublicationAndFencesNewWork() {
    MesiDirectory directory;
    TestMemory memory;
    auto initial = bytes(0x10);
    storeScalar(initial, 0, 4);
    memory.seed(kLineA, initial);
    memory.blockWrites();
    TestTransport transport;
    MesiTransactionEngine engine(directory, memory, transport, kWait);
    EndpointSessionRegistry registry;
    const auto live = registry.registerEndpoint(registration(26));
    CHECK(engine.bindSession(26, live.session_id));
    auto admitted = requestFrame(Opcode::AtomicFaa, 1, live.session_id, 26);
    setAddress(admitted, kLineA);
    setSize(admitted, 8);
    setValue(admitted, 3);
    auto registry_admission = registry.admitOperation(live.session_id, live.binding_id, admitted);
    CHECK(registry_admission.result == RequestAdmissionResult::Accepted);
    auto authority = std::move(registry_admission.authority);

    auto atomic = std::async(std::launch::async,
                             [&] { return engine.fetchAdd(kLineA, {26, live.session_id, 1}, std::uint64_t{3}); });
    memory.waitForWrite();
    auto eviction = std::async(std::launch::async, [&] {
        return task5Evict(engine, registry, 26, live.session_id, live.binding_id,
                          TestFailurePolicy::AssertProcessStopped);
    });
    for (int i = 0; i != 100 && registry.inspect(live.session_id) &&
                    registry.inspect(live.session_id)->state != SessionState::Fenced;
         ++i)
        std::this_thread::yield();
    CHECK(eviction.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout);
    const auto writes_before = memory.writes();
    const auto fenced = engine.fetchAdd(kLineB, {26, live.session_id, 2}, 1);
    CHECK(fenced.status == Status::HostFenced);
    CHECK(memory.writes() == writes_before);

    memory.releaseWrites();
    const auto completed = ready(atomic, "admitted atomic");
    CHECK(completed.status == Status::Ok);
    CHECK(registry.addModifiedHolder(authority, kLineA));
    CHECK(registry.completeOperation(authority));
    const auto result = ready(eviction, "eviction lifecycle barrier");
    CHECK(result.status == TestAdministrativeStatus::DirtyDataPresent);
    checkState(directory, kLineA, MesiState::M, 26, 0, 1, false);
    CHECK(registry.inspect(live.session_id).has_value());
}

void testPutmAdmissionSurvivesLaterDisconnectAndFailedWritesPreserveM() {
    MesiDirectory directory;
    CHECK(directory.getm(kLineA, 27).committed());
    TestMemory memory;
    const auto before = bytes(0x11);
    const auto after = bytes(0x22);
    memory.seed(kLineA, before);
    memory.blockWrites();
    TestTransport transport;
    MesiTransactionEngine engine(directory, memory, transport, kWait);
    CHECK(engine.bindSession(27, 127));
    auto writeback = std::async(std::launch::async, [&] { return engine.putm(kLineA, {27, 127, 1}, 1, after); });
    memory.waitForWrite();
    CHECK(engine.notifyDisconnect(27, 127) == 0);
    memory.releaseWrites();
    const auto committed = ready(writeback, "admitted PUTM after disconnect");
    CHECK(committed.status == Status::Ok);
    CHECK(memory.line(kLineA) == after);
    checkState(directory, kLineA, MesiState::I, std::nullopt, 0, 2, true);

    CHECK(directory.getm(kLineB, 28).committed());
    CHECK(engine.bindSession(28, 128));
    engine.notifyDisconnect(28, 128);
    const auto writes_before = memory.writes();
    CHECK(engine.putm(kLineB, {28, 128, 1}, 1, bytes(0x33)).status == Status::StaleSession);
    CHECK(memory.writes() == writes_before);
    checkState(directory, kLineB, MesiState::M, 28, 0, 1, false);

    CHECK(directory.getm(kLineC, 29).committed());
    CHECK(engine.bindSession(29, 129));
    const auto original = bytes(0x44);
    memory.seed(kLineC, original);
    memory.throwWrites();
    CHECK(engine.putm(kLineC, {29, 129, 1}, 1, bytes(0x55)).status == Status::IoError);
    CHECK(memory.line(kLineC) == original);
    checkState(directory, kLineC, MesiState::M, 29, 0, 1, false);
}

void testHolderIndexRejectsRetiredBindingAfterResume() {
    EndpointSessionRegistry registry;
    const auto first = registry.registerEndpoint(registration(30));
    CHECK(task5AddClean(registry, first.session_id, first.binding_id, kLineA));
    CHECK(registry.disconnectAbruptly(30, first.session_id, first.binding_id));
    const auto retired_generation = registry.captureGeneration(30, first.session_id, BindingId{});
    CHECK(retired_generation.has_value());
    auto resume = registration(30);
    resume.requested_session_id = first.session_id;
    const auto second = registry.registerEndpoint(resume);
    CHECK(second.status == Status::Ok);
    CHECK(!task5AddClean(registry, first.session_id, first.binding_id, kLineB));
    CHECK(!task5RemoveClean(registry, first.session_id, first.binding_id, kLineA));
    CHECK(!task5IdentityCanRemoveClean(registry, *retired_generation, kLineA));
    CHECK(task5AddClean(registry, second.session_id, second.binding_id, kLineB));
    CHECK(task5CleanHolders(registry, second.session_id, second.binding_id) ==
          (std::vector<std::uint64_t>{kLineA, kLineB}));
}

void testUnregisterPreflightsAllCandidatesBeforeFirstRemoval() {
    MesiDirectory directory;
    CHECK(directory.gets(kLineA, 31).committed());
    CHECK(directory.getm(kLineB, 31).committed());
    TestMemory memory;
    TestTransport transport;
    MesiTransactionEngine engine(directory, memory, transport, kWait);
    EndpointSessionRegistry registry;
    const auto live = registry.registerEndpoint(registration(31));
    CHECK(task5AddClean(registry, live.session_id, live.binding_id, kLineA));
    CHECK(task5AddClean(registry, live.session_id, live.binding_id, kLineB));
    const auto unregister = requestFrame(Opcode::Unregister, 1, live.session_id, 31);
    CHECK(registry.admitRequest(live.session_id, live.binding_id, unregister) == RequestAdmissionResult::Accepted);
    CHECK(engine.unregisterSession(registry, 31, live.session_id, live.binding_id, unregister) == Status::InvalidState);
    checkState(directory, kLineA, MesiState::E, 31, 0, 1, true);
    checkState(directory, kLineB, MesiState::M, 31, 0, 1, false);
    CHECK(task5CleanHolders(registry, live.session_id, live.binding_id) ==
          (std::vector<std::uint64_t>{kLineA, kLineB}));

    MesiDirectory race_directory;
    CHECK(race_directory.gets(kLineC, 35).committed());
    MesiTransactionEngine race_engine(race_directory, memory, transport, kWait);
    EndpointSessionRegistry race_registry;
    const auto race = race_registry.registerEndpoint(registration(35));
    CHECK(task5AddClean(race_registry, race.session_id, race.binding_id, kLineC));
    const auto earlier = requestFrame(Opcode::Heartbeat, 1, race.session_id, 35);
    const auto close = requestFrame(Opcode::Unregister, 2, race.session_id, 35);
    CHECK(race_registry.admitRequest(race.session_id, race.binding_id, earlier) == RequestAdmissionResult::Accepted);
    CHECK(race_registry.admitRequest(race.session_id, race.binding_id, close) == RequestAdmissionResult::Accepted);
    auto closing = std::async(std::launch::async, [&] {
        return race_engine.unregisterSession(race_registry, 35, race.session_id, race.binding_id, close);
    });
    CHECK(closing.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout);
    CHECK(race_registry.disconnectAbruptly(35, race.session_id, race.binding_id));
    CHECK(ready(closing, "UNREGISTER binding loss") != Status::Ok);
    checkState(race_directory, kLineC, MesiState::E, 35, 0, 1, true);
    auto resume = registration(35);
    resume.requested_session_id = race.session_id;
    const auto resumed = race_registry.registerEndpoint(resume);
    CHECK(resumed.status == Status::Ok);
    CHECK(task5CleanHolders(race_registry, resumed.session_id, resumed.binding_id) ==
          std::vector<std::uint64_t>{kLineC});
}

template <typename Engine> bool exerciseDurableBoundedAuditContract() {
    if constexpr (requires { typename Engine::AuditSink; }) {
        class RejectingSink final : public Engine::AuditSink {
        public:
            bool accept(const CoherenceAuditRecord &) noexcept override { return false; }
        } rejecting;
        MesiDirectory rejected_directory;
        CHECK(rejected_directory.getm(kLineA, 32).committed());
        TestMemory rejected_memory;
        TestTransport rejected_transport;
        Engine rejected_engine(rejected_directory, rejected_memory, rejected_transport, kWait, 1, &rejecting, 2);
        EndpointSessionRegistry rejected_registry;
        const auto rejected = rejected_registry.registerEndpoint(registration(32));
        CHECK(task5AddModified(rejected_registry, rejected.session_id, rejected.binding_id, kLineA));
        CHECK(rejected_registry.disconnectAbruptly(32, rejected.session_id, rejected.binding_id));
        const auto failed = task5Evict(rejected_engine, rejected_registry, 32, rejected.session_id, BindingId{},
                                       TestFailurePolicy::ForceDataLoss);
        CHECK(failed.status != TestAdministrativeStatus::DataLoss);
        CHECK(failed.dirty_lost == 0);
        checkState(rejected_directory, kLineA, MesiState::M, 32, 0, 1, false);
        CHECK(rejected_engine.auditCounters().forced_dirty_loss == 0);

        class AcceptingSink final : public Engine::AuditSink {
        public:
            bool accept(const CoherenceAuditRecord &record) noexcept override {
                records.push_back(record);
                return true;
            }
            std::vector<CoherenceAuditRecord> records;
        } accepting;
        MesiDirectory bounded_directory;
        TestMemory bounded_memory;
        TestTransport bounded_transport;
        Engine bounded_engine(bounded_directory, bounded_memory, bounded_transport, kWait, 1, &accepting, 2);
        for (std::uint64_t snoop = 1; snoop != 5; ++snoop) {
            auto stale = initializeFrame(Opcode::SnoopAck);
            setSrcHost(stale, 1);
            setDstHost(stale, kServerHost);
            setSessionId(stale, 1);
            setSnoopId(stale, snoop);
            setAckStrength(stale, AckStrength::MODEL);
            CHECK(bounded_engine.handleSnoopAck(stale) == AckDisposition::Stale);
        }
        CHECK(bounded_engine.auditRecords().size() == 2);
        return true;
    }
    return false;
}

void testForcedLossRequiresDurableAuditAndDiagnosticStorageIsBounded() {
    CHECK(exerciseDurableBoundedAuditContract<MesiTransactionEngine>());
}

void testForcedLossKeepsSealedGenerationAcrossReentrantDisconnect() {
    class DisconnectingSink final : public MesiTransactionEngine::AuditSink {
    public:
        bool accept(const CoherenceAuditRecord &) override {
            disconnected = registry->disconnectAbruptly(host_id, session_id, binding_id) || disconnected;
            return true;
        }

        EndpointSessionRegistry *registry{};
        std::uint16_t host_id{};
        SessionId session_id{};
        BindingId binding_id{};
        bool disconnected{};
    } sink;

    MesiDirectory directory;
    CHECK(directory.getm(kLineA, 36).committed());
    TestMemory memory;
    TestTransport transport;
    MesiTransactionEngine engine(directory, memory, transport, kWait, 1, &sink, 2);
    EndpointSessionRegistry registry;
    const auto live = registry.registerEndpoint(registration(36));
    CHECK(task5AddModified(registry, live.session_id, live.binding_id, kLineA));
    sink.registry = &registry;
    sink.host_id = 36;
    sink.session_id = live.session_id;
    sink.binding_id = live.binding_id;

    const auto result =
        task5Evict(engine, registry, 36, live.session_id, live.binding_id, TestFailurePolicy::ForceDataLoss);
    CHECK(sink.disconnected);
    CHECK(result.status == TestAdministrativeStatus::DataLoss);
    CHECK(result.dirty_lost == 1);
    CHECK(engine.auditCounters().forced_dirty_loss == 1);
    checkState(directory, kLineA, MesiState::I, std::nullopt, 0, 2, true);
    CHECK(!registry.inspect(live.session_id).has_value());
}

template <typename Registry>
constexpr bool hasRound2OperationAuthority =
    requires(Registry &registry) { registry.admitOperation(SessionId{}, BindingId{}, CoherenceFrame{}); };

template <typename Engine>
constexpr bool hasRound2EvictionFaultInjection = requires { typename Engine::AdministrativeFaultInjector; };

void testUnregisterRejectsMissingEarlierPinnedResponseBeforeMutation() {
    MesiDirectory directory;
    CHECK(directory.gets(kLineA, 38).committed());
    TestMemory memory;
    TestTransport transport;
    MesiTransactionEngine engine(directory, memory, transport, kWait);
    EndpointSessionRegistry registry;
    const auto live = registry.registerEndpoint(registration(38));
    CHECK(task5AddClean(registry, live.session_id, live.binding_id, kLineA));
    const auto heartbeat = requestFrame(Opcode::Heartbeat, 1, live.session_id, 38);
    const auto unregister = requestFrame(Opcode::Unregister, 2, live.session_id, 38);
    CHECK(registry.admitRequest(live.session_id, live.binding_id, heartbeat) == RequestAdmissionResult::Accepted);
    CHECK(registry.admitRequest(live.session_id, live.binding_id, unregister) == RequestAdmissionResult::Accepted);
    CHECK(task5Complete(registry, live.session_id, live.binding_id, 1));
    const auto before = directory.lockLine(kLineA)->snapshot();

    CHECK(engine.unregisterSession(registry, 38, live.session_id, live.binding_id, unregister) == Status::InvalidState);
    const auto after = directory.lockLine(kLineA)->snapshot();
    CHECK(after.state == before.state);
    CHECK(after.epoch == before.epoch);
    CHECK(task5CleanHolders(registry, live.session_id, live.binding_id) == std::vector<std::uint64_t>{kLineA});
}

void testUnregisterRejectsInFlightResponseRetirementBeforeMutation() {
    std::mutex sender_mutex;
    std::condition_variable sender_changed;
    bool sender_entered = false;
    bool release_sender = false;

    auto registration_request = registration(49);
    registration_request.sender = [&](const CoherenceFrame &) {
        std::unique_lock lock(sender_mutex);
        sender_entered = true;
        sender_changed.notify_all();
        sender_changed.wait(lock, [&] { return release_sender; });
        return false;
    };

    MesiDirectory directory;
    CHECK(directory.gets(kLineA, 49).committed());
    TestMemory memory;
    TestTransport transport;
    MesiTransactionEngine engine(directory, memory, transport, kWait);
    EndpointSessionRegistry registry;
    const auto live = registry.registerEndpoint(registration_request);
    CHECK(registry.addCleanHolder(live.session_id, live.binding_id, kLineA));
    const auto heartbeat = requestFrame(Opcode::Heartbeat, 1, live.session_id, 49);
    const auto unregister = requestFrame(Opcode::Unregister, 2, live.session_id, 49);
    CHECK(registry.admitRequest(live.session_id, live.binding_id, heartbeat) == RequestAdmissionResult::Accepted);
    CHECK(registry.admitRequest(live.session_id, live.binding_id, unregister) == RequestAdmissionResult::Accepted);

    auto publishing = std::async(
        std::launch::async, [&] { return registry.pinResponse(live.session_id, heartbeat, responseFrame(heartbeat)); });
    {
        std::unique_lock lock(sender_mutex);
        sender_changed.wait(lock, [&] { return sender_entered; });
    }
    const auto before = directory.lockLine(kLineA)->snapshot();
    CHECK(engine.unregisterSession(registry, 49, live.session_id, live.binding_id, unregister) == Status::InvalidState);
    const auto after = directory.lockLine(kLineA)->snapshot();
    CHECK(after.state == before.state);
    CHECK(after.epoch == before.epoch);
    CHECK(registry.cleanHolders(live.session_id, live.binding_id) == std::vector<std::uint64_t>{kLineA});

    {
        std::lock_guard lock(sender_mutex);
        release_sender = true;
    }
    sender_changed.notify_all();
    CHECK(ready(publishing, "in-flight response retirement") == PinResponseResult::DeliveryFailed);
}

void testRound2AuthorityAndStickyLossSurfacesArePresent() {
    CHECK(hasRound2OperationAuthority<EndpointSessionRegistry>);
    CHECK(hasRound2EvictionFaultInjection<MesiTransactionEngine>);
}

void testAdmittedAtomicPublishesAfterDisconnectBeforeOfflineCleanup() {
    MesiDirectory directory;
    TestMemory memory;
    auto initial = bytes(0x20);
    storeScalar(initial, 0, 9);
    memory.seed(kLineA, initial);
    memory.blockWrites();
    TestTransport transport;
    MesiTransactionEngine engine(directory, memory, transport, kWait);
    EndpointSessionRegistry registry;
    const auto live = registry.registerEndpoint(registration(43));
    CHECK(engine.bindSession(43, live.session_id));
    auto request = requestFrame(Opcode::AtomicFaa, 1, live.session_id, 43);
    setAddress(request, kLineA);
    setSize(request, sizeof(std::uint64_t));
    setValue(request, 4);
    auto admission = registry.admitOperation(live.session_id, live.binding_id, request);
    CHECK(admission.result == RequestAdmissionResult::Accepted);
    auto authority = std::move(admission.authority);
    CHECK(static_cast<bool>(authority));

    auto operation =
        std::async(std::launch::async, [&] { return engine.fetchAdd(kLineA, {43, live.session_id, 1}, 4); });
    memory.waitForWrite();
    CHECK(registry.disconnectAbruptly(43, live.session_id, live.binding_id));
    (void)engine.notifyDisconnect(43, live.session_id);
    auto resume = registration(43);
    resume.requested_session_id = live.session_id;
    CHECK(registry.registerEndpoint(resume).status == Status::StaleSession);
    auto eviction = std::async(std::launch::async, [&] {
        return engine.evictHost(registry, 43, live.session_id, BindingId{}, HostFailurePolicy::AssertProcessStopped);
    });
    CHECK(eviction.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout);

    memory.releaseWrites();
    CHECK(ready(operation, "post-disconnect admitted atomic").status == Status::Ok);
    CHECK(registry.addModifiedHolder(authority, kLineA));
    CHECK(registry.completeOperation(authority));
    CHECK(!registry.addModifiedHolder(authority, kLineB));
    CHECK(!registry.completeOperation(authority));
    const auto result = ready(eviction, "offline atomic cleanup barrier");
    CHECK(result.status == AdministrativeStatus::DirtyDataPresent);
    checkState(directory, kLineA, MesiState::M, 43, 0, 1, false);

    const auto generation = registry.captureGeneration(43, live.session_id, BindingId{});
    CHECK(generation.has_value());
    auto cleanup = registry.freezeFencedGenerationForCleanup(*generation);
    CHECK(cleanup.has_value());
    CHECK(registry.holderSnapshot(*cleanup).modified == std::vector<std::uint64_t>{kLineA});
    registry.abortFencedCleanup(*cleanup);
}

void testAdmittedGetmPublishesTerminalFailureAfterDisconnectBeforeOfflineCleanup() {
    MesiDirectory directory;
    TestMemory memory;
    memory.seed(kLineA, bytes(0x31));
    memory.blockReads();
    TestTransport transport;
    MesiTransactionEngine engine(directory, memory, transport, kWait);
    EndpointSessionRegistry registry;
    const auto live = registry.registerEndpoint(registration(44));
    CHECK(engine.bindSession(44, live.session_id));
    auto request = requestFrame(Opcode::Getm, 1, live.session_id, 44);
    setAddress(request, kLineA);
    auto admission = registry.admitOperation(live.session_id, live.binding_id, request);
    CHECK(admission.result == RequestAdmissionResult::Accepted);
    auto authority = std::move(admission.authority);
    CHECK(static_cast<bool>(authority));

    auto operation = std::async(std::launch::async, [&] { return engine.getm(kLineA, {44, live.session_id, 1}); });
    memory.waitForRead();
    CHECK(registry.disconnectAbruptly(44, live.session_id, live.binding_id));
    (void)engine.notifyDisconnect(44, live.session_id);
    auto eviction = std::async(std::launch::async, [&] {
        return engine.evictHost(registry, 44, live.session_id, BindingId{}, HostFailurePolicy::AssertProcessStopped);
    });
    CHECK(eviction.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout);

    memory.releaseReads();
    CHECK(ready(operation, "post-disconnect admitted GETM").status == Status::HostFenced);
    auto failed_response = responseFrame(request);
    setStatus(failed_response, Status::HostFenced);
    setLineState(failed_response, lineState(request));
    setEpoch(failed_response, epoch(request));
    setPayloadLength(failed_response, 0);
    CHECK(registry.pinResponse(authority, request, failed_response) == PinResponseResult::Pinned);
    CHECK(registry.completeOperation(authority));
    CHECK(registry.pinResponse(authority, request, failed_response) == PinResponseResult::SessionUnavailable);
    const auto result = ready(eviction, "offline GETM cleanup barrier");
    CHECK(result.status == AdministrativeStatus::Ok);
    checkState(directory, kLineA, MesiState::I, std::nullopt, 0, 0, true);
    CHECK(!registry.inspect(live.session_id).has_value());
}

void testAdmittedPutmPublishesRemovalAfterDisconnectAndAllowsRetirement() {
    MesiDirectory directory;
    CHECK(directory.getm(kLineA, 45).committed());
    TestMemory memory;
    memory.seed(kLineA, bytes(0x41));
    memory.blockWrites();
    TestTransport transport;
    MesiTransactionEngine engine(directory, memory, transport, kWait);
    EndpointSessionRegistry registry;
    const auto live = registry.registerEndpoint(registration(45));
    CHECK(registry.addModifiedHolder(live.session_id, live.binding_id, kLineA));
    CHECK(engine.bindSession(45, live.session_id));
    auto request = requestFrame(Opcode::Putm, 1, live.session_id, 45);
    setAddress(request, kLineA);
    setEpoch(request, 1);
    setLineState(request, LineState::M);
    setPayloadLength(request, kLineSize);
    auto admission = registry.admitOperation(live.session_id, live.binding_id, request);
    CHECK(admission.result == RequestAdmissionResult::Accepted);
    auto authority = std::move(admission.authority);
    CHECK(static_cast<bool>(authority));
    const auto updated = bytes(0x52);

    auto operation =
        std::async(std::launch::async, [&] { return engine.putm(kLineA, {45, live.session_id, 1}, 1, updated); });
    memory.waitForWrite();
    CHECK(registry.disconnectAbruptly(45, live.session_id, live.binding_id));
    (void)engine.notifyDisconnect(45, live.session_id);
    auto eviction = std::async(std::launch::async, [&] {
        return engine.evictHost(registry, 45, live.session_id, BindingId{}, HostFailurePolicy::AssertProcessStopped);
    });
    CHECK(eviction.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout);

    memory.releaseWrites();
    CHECK(ready(operation, "post-disconnect admitted PUTM").status == Status::Ok);
    CHECK(registry.removeModifiedHolder(authority, kLineA));
    CHECK(registry.pinResponse(authority, request, responseFrame(request)) == PinResponseResult::Pinned);
    CHECK(registry.completeOperation(authority));
    const auto result = ready(eviction, "offline PUTM cleanup barrier");
    CHECK(result.status == AdministrativeStatus::Ok);
    CHECK(memory.line(kLineA) == updated);
    checkState(directory, kLineA, MesiState::I, std::nullopt, 0, 2, true);
    CHECK(!registry.inspect(live.session_id).has_value());
}

void testDuplicateUnregisterExecutionCannotReleaseOwnerFreeze() {
    MesiDirectory directory;
    CHECK(directory.gets(kLineA, 46).committed());
    TestMemory memory;
    TestTransport transport;
    MesiTransactionEngine engine(directory, memory, transport, kWait);
    EndpointSessionRegistry registry;
    const auto live = registry.registerEndpoint(registration(46));
    CHECK(registry.addCleanHolder(live.session_id, live.binding_id, kLineA));
    const auto request = requestFrame(Opcode::Unregister, 1, live.session_id, 46);
    CHECK(registry.admitRequest(live.session_id, live.binding_id, request) == RequestAdmissionResult::Accepted);
    auto held_line = directory.lockLine(kLineA);
    CHECK(held_line.has_value());
    auto owner = std::async(std::launch::async, [&] {
        return engine.unregisterSession(registry, 46, live.session_id, live.binding_id, request);
    });
    const auto deadline = std::chrono::steady_clock::now() + kWait;
    while (!registry.inspect(live.session_id)->unregister_in_progress) {
        if (std::chrono::steady_clock::now() >= deadline)
            std::_Exit(EXIT_FAILURE);
        std::this_thread::yield();
    }
    auto duplicate = std::async(std::launch::async, [&] {
        return engine.unregisterSession(registry, 46, live.session_id, live.binding_id, request);
    });
    CHECK(ready(duplicate, "duplicate UNREGISTER execution") == Status::StaleSession);
    CHECK(!registry.disconnectAbruptly(46, live.session_id, live.binding_id));
    held_line.reset();
    CHECK(ready(owner, "owning UNREGISTER execution") == Status::Ok);
    checkState(directory, kLineA, MesiState::I, std::nullopt, 0, 2, true);
}

void testDirtyUnregisterLeavesSessionAbleToDrainPutmAndRetry() {
    MesiDirectory directory;
    CHECK(directory.getm(kLineA, 50).committed());
    TestMemory memory;
    memory.seed(kLineA, bytes(0x61));
    TestTransport transport;
    MesiTransactionEngine engine(directory, memory, transport, kWait);
    EndpointSessionRegistry registry;
    const auto live = registry.registerEndpoint(registration(50));
    CHECK(engine.bindSession(50, live.session_id));
    CHECK(registry.addModifiedHolder(live.session_id, live.binding_id, kLineA));
    const auto first_unregister = requestFrame(Opcode::Unregister, 1, live.session_id, 50);
    CHECK(registry.admitRequest(live.session_id, live.binding_id, first_unregister) ==
          RequestAdmissionResult::Accepted);
    CHECK(engine.unregisterSession(registry, 50, live.session_id, live.binding_id, first_unregister) ==
          Status::InvalidState);
    auto failed_unregister_response = responseFrame(first_unregister);
    setStatus(failed_unregister_response, Status::InvalidState);
    const auto failed_pin = registry.pinResponse(live.session_id, first_unregister, failed_unregister_response);
    CHECK(failed_pin == PinResponseResult::Pinned);
    if (failed_pin != PinResponseResult::Pinned)
        return;

    auto putm_request = requestFrame(Opcode::Putm, 2, live.session_id, 50);
    setAddress(putm_request, kLineA);
    setEpoch(putm_request, 1);
    setLineState(putm_request, LineState::M);
    setPayloadLength(putm_request, kLineSize);
    auto admission = registry.admitOperation(live.session_id, live.binding_id, putm_request);
    CHECK(admission.result == RequestAdmissionResult::Accepted);
    auto authority = std::move(admission.authority);
    const auto updated = bytes(0x72);
    CHECK(engine.putm(kLineA, {50, live.session_id, 2}, 1, updated).status == Status::Ok);
    CHECK(registry.removeModifiedHolder(authority, kLineA));
    CHECK(registry.pinResponse(authority, putm_request, responseFrame(putm_request)) == PinResponseResult::Pinned);
    CHECK(registry.completeOperation(authority));

    const auto retry = requestFrame(Opcode::Unregister, 3, live.session_id, 50);
    CHECK(registry.admitRequest(live.session_id, live.binding_id, retry) == RequestAdmissionResult::Accepted);
    CHECK(engine.unregisterSession(registry, 50, live.session_id, live.binding_id, retry) == Status::Ok);
    CHECK(memory.line(kLineA) == updated);
    checkState(directory, kLineA, MesiState::I, std::nullopt, 0, 2, true);
}

void testForcedLossStaysStickyWhenSecondCandidateThrows() {
    class ThrowOnOrdinal final : public MesiTransactionEngine::AdministrativeFaultInjector {
    public:
        explicit ThrowOnOrdinal(std::size_t ordinal) : ordinal_(ordinal) {}
        void beforeLine(std::size_t ordinal, std::uint64_t) override {
            if (ordinal == ordinal_)
                throw std::runtime_error("injected administrative line failure");
        }

    private:
        std::size_t ordinal_;
    } injector(1);

    MesiDirectory directory;
    CHECK(directory.getm(kLineA, 47).committed());
    CHECK(directory.getm(kLineB, 47).committed());
    TestMemory memory;
    TestTransport transport;
    AcceptingAuditSink sink;
    MesiTransactionEngine engine(directory, memory, transport, kWait, 1, &sink, 8, &injector);
    EndpointSessionRegistry registry;
    const auto live = registry.registerEndpoint(registration(47));
    CHECK(registry.addModifiedHolder(live.session_id, live.binding_id, kLineA));
    CHECK(registry.addModifiedHolder(live.session_id, live.binding_id, kLineB));
    CHECK(registry.disconnectAbruptly(47, live.session_id, live.binding_id));

    const auto result = engine.evictHost(registry, 47, live.session_id, BindingId{}, HostFailurePolicy::ForceDataLoss);
    CHECK(result.status == AdministrativeStatus::DataLoss);
    CHECK(result.dirty_lost == 1);
    checkState(directory, kLineA, MesiState::I, std::nullopt, 0, 2, true);
    checkState(directory, kLineB, MesiState::M, 47, 0, 1, false);
    CHECK(engine.auditCounters().forced_dirty_loss == 1);
    CHECK(sink.records.size() == 3);
    CHECK(std::count_if(sink.records.begin(), sink.records.end(),
                        [](const auto &record) { return record.phase == AuditRecordPhase::Intent; }) == 2);
    CHECK(std::count_if(sink.records.begin(), sink.records.end(), [](const auto &record) {
              return record.phase == AuditRecordPhase::Completion && record.line_address == kLineA;
          }) == 1);
    const auto completed = engine.auditRecords();
    CHECK(completed.size() == 1);
    CHECK(completed.front().line_address == kLineA);
    CHECK(completed.front().phase == AuditRecordPhase::Completion);

    const auto generation = registry.captureGeneration(47, live.session_id, BindingId{});
    CHECK(generation.has_value());
    auto cleanup = registry.freezeFencedGenerationForCleanup(*generation);
    CHECK(cleanup.has_value());
    CHECK(registry.holderSnapshot(*cleanup).modified == std::vector<std::uint64_t>{kLineB});
    registry.abortFencedCleanup(*cleanup);

    ThrowOnOrdinal first_injector(0);
    MesiDirectory before_directory;
    CHECK(before_directory.getm(kLineC, 48).committed());
    TestMemory before_memory;
    TestTransport before_transport;
    AcceptingAuditSink before_sink;
    MesiTransactionEngine before_engine(before_directory, before_memory, before_transport, kWait, 1, &before_sink, 8,
                                        &first_injector);
    EndpointSessionRegistry before_registry;
    const auto before_live = before_registry.registerEndpoint(registration(48));
    CHECK(before_registry.addModifiedHolder(before_live.session_id, before_live.binding_id, kLineC));
    CHECK(before_registry.disconnectAbruptly(48, before_live.session_id, before_live.binding_id));
    const auto before_result = before_engine.evictHost(before_registry, 48, before_live.session_id, BindingId{},
                                                       HostFailurePolicy::ForceDataLoss);
    CHECK(before_result.status == AdministrativeStatus::InvalidHost);
    CHECK(before_result.dirty_lost == 0);
    checkState(before_directory, kLineC, MesiState::M, 48, 0, 1, false);
    CHECK(before_engine.auditCounters().forced_dirty_loss == 0);
    CHECK(before_engine.auditRecords().empty());
    CHECK(before_sink.records.size() == 1);
    CHECK(before_sink.records.front().phase == AuditRecordPhase::Intent);
}

} // namespace

int main() {
    testCleanPutsFromSharedAndExclusive();
    testPutmPersistsBeforeOwnerRemovalAndRejectsInvalidOwnership();
    testCasAndFetchAddSerializeAfterExclusiveOwnershipAndReturnUpdatedLine();
    testAtomicWaitsForConflictingExclusiveAckBeforeUpdatingMemory();
    testFenceWaitsOnlyForEarlierSameSessionWorkAndModifiedDrain();
    testGracefulUnregisterRevalidatesCleanCandidatesAndRejectsModifiedOwner();
    testHostFenceAckAndStoppedAssertionGateCleanRemoval();
    testForcedDirtyLossIsExplicitRecordedAndNeverCoherentSuccess();
    testHostFenceWakesTransactionsWaitingOnThatHost();
    testTimeoutPartialAckStaleAckAndInvalidOwnershipAreAudited();
    testAcceptedSnoopAckAuditCounterExcludesRejectedAcks();
    testHostFenceAuthorizationIsContextualOneShotAndFailClosed();
    testFencedOrdinaryAdmissionIsBoundedAndControlUsesContext();
    testEvictionWaitsForAdmittedAtomicHolderPublicationAndFencesNewWork();
    testPutmAdmissionSurvivesLaterDisconnectAndFailedWritesPreserveM();
    testHolderIndexRejectsRetiredBindingAfterResume();
    testUnregisterPreflightsAllCandidatesBeforeFirstRemoval();
    testForcedLossRequiresDurableAuditAndDiagnosticStorageIsBounded();
    testForcedLossKeepsSealedGenerationAcrossReentrantDisconnect();
    testUnregisterRejectsMissingEarlierPinnedResponseBeforeMutation();
    testUnregisterRejectsInFlightResponseRetirementBeforeMutation();
    testRound2AuthorityAndStickyLossSurfacesArePresent();
    testAdmittedAtomicPublishesAfterDisconnectBeforeOfflineCleanup();
    testAdmittedGetmPublishesTerminalFailureAfterDisconnectBeforeOfflineCleanup();
    testAdmittedPutmPublishesRemovalAfterDisconnectAndAllowsRetirement();
    testDuplicateUnregisterExecutionCannotReleaseOwnerFreeze();
    testDirtyUnregisterLeavesSessionAbleToDrainPutmAndRetry();
    testForcedLossStaysStickyWhenSecondCandidateThrows();

    const auto count = failures.load(std::memory_order_relaxed);
    if (count != 0) {
        std::cerr << count << " checks failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "MESI writeback and atomic tests passed\n";
    return EXIT_SUCCESS;
}
