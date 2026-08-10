#pragma once

#include "coherence_memory_backend.h"
#include "coherence_protocol_v2.h"
#include "coherence_transport.h"
#include "endpoint_session_registry.h"
#include "mesi_directory.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace cxlmemsim::mesi_v2 {

struct TransactionDependencies;

enum class AckDisposition : std::uint8_t { Accepted, Deferred, Duplicate, Stale, Invalid };

struct TransactionResult {
    protocol_v2::Status status{protocol_v2::Status::InvalidState};
    TransitionResult transition;
    bool granted{};
    std::array<std::byte, 64> data{};
    std::uint64_t old_value{};
};

struct TransactionRequest {
    using GrantInstaller = void (*)(void *, const TransactionResult &) noexcept;
    using CommitInstaller = void (*)(void *, const TransactionResult &) noexcept;

    std::uint16_t host_id{};
    std::uint64_t session_id{};
    std::uint64_t request_id{};
    std::optional<protocol_v2::LineState> local_state;
    std::uint64_t installed_epoch{};
    void *grant_context{};
    GrantInstaller grant_installer{};
    void *commit_context{};
    CommitInstaller commit_installer{};
};

enum class HostFailurePolicy : std::uint8_t { RequireFenceAck, AssertProcessStopped, ForceDataLoss };
enum class AdministrativeStatus : std::uint8_t {
    Ok,
    FenceAckRequired,
    DirtyDataPresent,
    DataLoss,
    StaleSession,
    InvalidHost,
    AuditFailure,
};

struct EvictionResult {
    AdministrativeStatus status{AdministrativeStatus::InvalidHost};
    std::size_t clean_removed{};
    std::size_t dirty_lost{};
};

enum class AuditEventKind : std::uint8_t {
    Timeout,
    PartialAck,
    ForcedCleanRemoval,
    ForcedDirtyLoss,
    StaleAck,
    InvalidOwnership,
};
enum class AuditSeverity : std::uint8_t { Info, Warning, High };
enum class AuditRecordPhase : std::uint8_t { Event, Intent, Completion };

struct CoherenceAuditCounters {
    std::uint64_t timeout{};
    std::uint64_t partial_ack{};
    std::uint64_t forced_clean_removal{};
    std::uint64_t forced_dirty_loss{};
    std::uint64_t stale_ack{};
    std::uint64_t invalid_ownership{};
    std::uint64_t accepted_snoop_ack{};
};

struct CoherenceAuditRecord {
    AuditEventKind kind{AuditEventKind::Timeout};
    AuditSeverity severity{AuditSeverity::Info};
    std::uint16_t host_id{};
    std::uint64_t session_id{};
    std::uint64_t line_address{};
    std::uint64_t epoch{};
    AuditRecordPhase phase{AuditRecordPhase::Event};
};

class MesiTransactionEngine {
public:
    using HostFailurePolicy = mesi_v2::HostFailurePolicy;
    enum class Operation : std::uint8_t { Gets, Getm, Upgrade, AtomicFaa, AtomicCas };
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using Duration = Clock::duration;

    // Forced dirty loss is permitted only after this synchronous sink durably accepts the exact High-severity Intent
    // record. Returning true promises durable acceptance. After the typed M -> I commit, the exact Completion is also
    // presented synchronously; its return value cannot undo the discard or replace DATA_LOSS. The callback may
    // synchronously reenter registry/engine APIs and runs without lifecycle, operation-wait, transport, audit, or
    // directory locks. The sink is caller-owned and must outlive the engine.
    class AuditSink {
    public:
        virtual ~AuditSink() = default;
        virtual bool accept(const CoherenceAuditRecord &record) = 0;
    };

    // Optional deterministic administrative boundary used by fault-injection and supervisory integrations. It runs
    // immediately before each candidate line lock, without lifecycle, registry, audit, or directory locks. Throwing
    // fails closed before the first dirty discard; after one or more committed discards, DATA_LOSS remains sticky.
    class AdministrativeFaultInjector {
    public:
        virtual ~AdministrativeFaultInjector() = default;
        virtual void beforeLine(std::size_t ordinal, std::uint64_t line_address) = 0;
    };

    // first_snoop_id seeds the never-reused monotonic allocator. Zero means
    // the identifier space is already exhausted and transactions fail closed.
    explicit MesiTransactionEngine(MesiDirectory &directory, Duration snoop_timeout = std::chrono::milliseconds(1000),
                                   std::uint64_t first_snoop_id = 1);
    MesiTransactionEngine(MesiDirectory &directory, CoherenceMemoryBackend &memory, CoherenceTransport &transport,
                          Duration snoop_timeout = std::chrono::milliseconds(1000), std::uint64_t first_snoop_id = 1,
                          AuditSink *audit_sink = nullptr, std::size_t audit_capacity = 256,
                          AdministrativeFaultInjector *administrative_fault_injector = nullptr);
    ~MesiTransactionEngine();

    MesiTransactionEngine(const MesiTransactionEngine &) = delete;
    MesiTransactionEngine &operator=(const MesiTransactionEngine &) = delete;

    // The configured backend and transport are caller-owned and must outlive
    // this engine and every transaction that may have captured them. A
    // transaction captures the complete configuration atomically, so a
    // concurrent configure() cannot mix dependency generations.
    void configure(CoherenceMemoryBackend &memory, CoherenceTransport &transport, Duration snoop_timeout);

    // Inserts a previously unbound session or confirms the identical generation. A different generation fails without
    // changing the binding; replacement is allowed only after notifyDisconnect() removes the matching old generation.
    bool bindSession(std::uint16_t host_id, std::uint64_t session_id);
    std::uint64_t sessionFor(std::uint16_t host_id) const;

    TransactionResult gets(std::uint64_t line_address, TransactionRequest request);
    TransactionResult getm(std::uint64_t line_address, TransactionRequest request);
    TransactionResult upgrade(std::uint64_t line_address, TransactionRequest request);
    // PUTS/PUTM acquire an exact-generation operation lease before the line transaction. PUTM admission is its
    // lifecycle linearization point: a disconnect that wins first performs no write; once admitted, PUTM completes its
    // synchronous data-before-typed-metadata sequence despite later fencing. No lifecycle lock crosses the callback.
    TransactionResult puts(std::uint64_t line_address, TransactionRequest request, std::uint64_t installed_epoch);
    TransactionResult putm(std::uint64_t line_address, TransactionRequest request, std::uint64_t installed_epoch,
                           std::span<const std::byte, 64> data);
    TransitionResult puts(std::uint64_t line_address, std::uint16_t requester);
    TransitionResult putm(std::uint64_t line_address, std::uint16_t requester);

    // Atomics operate on naturally aligned 64-bit scalars contained in one line. They acquire exclusive coherence under
    // the existing line transaction lock, persist the complete updated line, advance the directory epoch once, and only
    // then return the old scalar and M grant. Coherence or backend failure executes no atomic update.
    TransactionResult fetchAdd(std::uint64_t address, TransactionRequest request, std::uint64_t value);
    TransactionResult compareExchange(std::uint64_t address, TransactionRequest request, std::uint64_t expected,
                                      std::uint64_t desired);

    // FENCE and UNREGISTER wait on the target session's operation watermark without holding registry, engine-session,
    // or line locks. UNREGISTER snapshots reverse-index candidates, then revalidates and commits at most one directory
    // line at a time.
    protocol_v2::Status fence(EndpointSessionRegistry &registry, SessionId session_id, BindingId binding_id,
                              std::uint64_t request_id);
    protocol_v2::Status unregisterSession(EndpointSessionRegistry &registry, std::uint16_t host_id,
                                          SessionId session_id, BindingId binding_id,
                                          const protocol_v2::CoherenceFrame &unregister_request);

    // RequireFenceAck sends HOST_FENCE through the configured transport after blocking application admission. Only a
    // one-shot contextual ACK delivered through handleControlFrame() authorizes cleanup; timeout, send failure,
    // disconnect, malformed ACK, or generation mismatch fails closed. AssertProcessStopped is a separate caller
    // precondition that the process cannot access cache bytes. Neither policy may discard M. ForceDataLoss is the sole
    // dirty-discard path and requires durable AuditSink acceptance before each irreversible M -> I commit.
    EvictionResult evictHost(EndpointSessionRegistry &registry, std::uint16_t host_id, SessionId session_id,
                             BindingId binding_id, HostFailurePolicy policy);

    CoherenceAuditCounters auditCounters() const noexcept;
    std::vector<CoherenceAuditRecord> auditRecords() const;

    AckDisposition handleSnoopAck(const protocol_v2::CoherenceFrame &ack);
    // Contextual ACK route used by fenced transports. Registry validation pins the exact live binding generation; the
    // engine then validates host/session/snoop/opcode-derived state/strength and consumes the pending token once.
    AckDisposition handleControlFrame(EndpointSessionRegistry &registry, SessionId session_id, BindingId binding_id,
                                      const protocol_v2::CoherenceFrame &frame);
    std::size_t progress(TimePoint now = Clock::now());
    std::size_t notifyDisconnect(std::uint16_t host_id, std::uint64_t session_id);

private:
    struct EngineSession;
    struct OperationLease {
        MesiTransactionEngine *engine{};
        std::shared_ptr<EngineSession> session;
        bool admitted{};
        OperationLease() = default;
        OperationLease(MesiTransactionEngine *owner, std::shared_ptr<EngineSession> state)
            : engine(owner), session(std::move(state)), admitted(true) {}
        OperationLease(OperationLease &&other) noexcept
            : engine(other.engine), session(std::move(other.session)), admitted(other.admitted) {
            other.engine = nullptr;
            other.admitted = false;
        }
        OperationLease &operator=(OperationLease &&other) noexcept;
        OperationLease(const OperationLease &) = delete;
        OperationLease &operator=(const OperationLease &) = delete;
        ~OperationLease();
        explicit operator bool() const noexcept { return admitted; }
    };
    struct AtomicArguments {
        std::size_t offset{};
        std::uint64_t operand{};
        std::uint64_t expected{};
    };
    TransactionResult acquire(std::uint64_t line_address, TransactionRequest request, Operation operation,
                              std::optional<AtomicArguments> atomic = std::nullopt);
    TransactionResult direct(MesiDirectory::LockedLine &line, TransactionRequest request, Operation operation,
                             const DirectorySnapshot &current,
                             const std::shared_ptr<const TransactionDependencies> &dependencies,
                             std::optional<AtomicArguments> atomic,
                             const std::shared_ptr<EngineSession> &admitted_session);
    TransactionResult transact(MesiDirectory::LockedLine &line, TransactionRequest request, Operation operation,
                               const DirectorySnapshot &current,
                               const std::shared_ptr<const TransactionDependencies> &dependencies,
                               std::optional<AtomicArguments> atomic);
    TransactionResult reconcile(MesiDirectory::LockedLine &line, const std::shared_ptr<PendingTransaction> &pending);
    protocol_v2::Status validateRequest(const TransactionRequest &request,
                                        const std::shared_ptr<const TransactionDependencies> &dependencies) const;
    std::optional<std::uint64_t> allocateSnoopId() noexcept;
    void registerPending(const std::shared_ptr<PendingTransaction> &pending);
    void unregisterPending(const std::shared_ptr<PendingTransaction> &pending);
    bool pendingSessionsCurrent(const PendingTransaction &pending) const;
    std::size_t interruptHost(std::uint16_t host_id, std::uint64_t session_id, bool remove_binding);
    OperationLease admitOperation(const TransactionRequest &request, bool fenced_drain);
    void releaseOperation(const std::shared_ptr<EngineSession> &session) noexcept;
    bool fenceEngineSession(std::uint16_t host_id, std::uint64_t session_id);
    bool sealEngineSession(std::uint16_t host_id, std::uint64_t session_id);
    bool waitEngineQuiescent(std::uint16_t host_id, std::uint64_t session_id);
    std::shared_ptr<const TransactionDependencies> dependencySnapshot() const;
    static protocol_v2::Status statusFor(TransitionStatus status) noexcept;
    void recordAudit(AuditEventKind kind, AuditSeverity severity, std::uint16_t host_id, std::uint64_t session_id,
                     std::uint64_t line_address, std::uint64_t epoch,
                     AuditRecordPhase phase = AuditRecordPhase::Event) noexcept;
    bool acceptForcedLoss(const CoherenceAuditRecord &record) noexcept;

    MesiDirectory &directory_;
    mutable std::mutex dependencies_mutex_;
    std::shared_ptr<const TransactionDependencies> dependencies_;
    std::atomic<std::uint64_t> next_snoop_id_{1};

    mutable std::mutex sessions_mutex_;
    std::condition_variable sessions_changed_;
    std::unordered_map<std::uint16_t, std::shared_ptr<EngineSession>> sessions_;

    mutable std::mutex active_mutex_;
    std::unordered_map<std::uint64_t, std::weak_ptr<PendingTransaction>> active_by_snoop_;
    std::unordered_map<std::uint64_t, std::weak_ptr<PendingTransaction>> active_by_line_;
    struct PendingHostFence;
    std::unordered_map<std::uint64_t, std::shared_ptr<PendingHostFence>> active_host_fences_;

    std::atomic<std::uint64_t> timeout_events_{};
    std::atomic<std::uint64_t> partial_ack_events_{};
    std::atomic<std::uint64_t> forced_clean_removals_{};
    std::atomic<std::uint64_t> forced_dirty_losses_{};
    std::atomic<std::uint64_t> stale_acks_{};
    std::atomic<std::uint64_t> invalid_ownership_events_{};
    std::atomic<std::uint64_t> accepted_snoop_acks_{};
    mutable std::mutex audit_mutex_;
    std::vector<CoherenceAuditRecord> audit_records_;
    AuditSink *audit_sink_{};
    std::size_t audit_capacity_{256};
    AdministrativeFaultInjector *administrative_fault_injector_{};
};

} // namespace cxlmemsim::mesi_v2
