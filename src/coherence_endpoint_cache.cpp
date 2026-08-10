#include "coherence_endpoint_cache.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace cxlmemsim {

namespace {

using mesi_v2::MesiState;
using mesi_v2::TransactionRequest;
using mesi_v2::TransactionResult;
using protocol_v2::CoherenceFrame;
using protocol_v2::LineState;
using protocol_v2::Opcode;
using protocol_v2::Status;

constexpr std::uint64_t kLineMask = protocol_v2::kLineSize - 1;

std::uint64_t lineAddress(std::uint64_t address) noexcept { return address & ~kLineMask; }

bool validAccess(std::uint64_t address, std::size_t size) noexcept {
    return size <= protocol_v2::kLineSize - static_cast<std::size_t>(address & kLineMask);
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

Status grantedStatus(const TransactionResult &result) noexcept {
    return result.status == Status::Ok && !result.granted ? Status::InvalidState : result.status;
}

bool isDowngrade(Opcode opcode) noexcept {
    return opcode == Opcode::SnpDowngrade || opcode == Opcode::SnpDataDowngrade;
}

bool returnsData(Opcode opcode) noexcept { return opcode == Opcode::SnpDataInv || opcode == Opcode::SnpDataDowngrade; }

LineState snoopPostState(Opcode opcode) noexcept { return isDowngrade(opcode) ? LineState::S : LineState::I; }

bool compatibleReplacement(Opcode completed, Opcode replacement) noexcept {
    if (completed == replacement)
        return true;
    return (completed == Opcode::SnpDowngrade && replacement == Opcode::SnpInv) ||
           (completed == Opcode::SnpDataDowngrade && replacement == Opcode::SnpDataInv);
}

} // namespace

struct CoherenceEndpointCache::Impl {
    struct CacheLine {
        std::array<std::byte, protocol_v2::kLineSize> data{};
        LineState state{LineState::I};
        std::uint64_t epoch{};
        std::uint64_t lru{};
        std::uint64_t snoop_token{};
    };

    struct SnoopCompletion {
        Opcode opcode{Opcode::SnpInv};
        std::uint64_t line_address{};
        std::uint64_t snoop_id{};
        std::uint64_t epoch{};
        CoherenceFrame ack{};
    };

    struct LineOperation {
        std::uint64_t token{};
    };

    struct ReportedPermission {
        LineState state{LineState::I};
        std::uint64_t epoch{};

        bool operator==(const ReportedPermission &) const = default;
    };

    struct GrantInstallContext {
        Impl *impl{};
        std::uint64_t line_address{};
        std::size_t offset{};
        std::span<std::byte> destination;
        std::span<const std::byte> source;
        std::array<std::byte, protocol_v2::kLineSize> base_data{};
        bool use_base_data{};
        bool installed{};
    };

    class OperationGuard {
    public:
        OperationGuard() = default;
        OperationGuard(Impl &impl, std::uint64_t line_address, std::uint64_t token) noexcept
            : impl_(&impl), line_address_(line_address), token_(token) {}
        OperationGuard(const OperationGuard &) = delete;
        OperationGuard &operator=(const OperationGuard &) = delete;
        OperationGuard(OperationGuard &&other) noexcept
            : impl_(std::exchange(other.impl_, nullptr)), line_address_(other.line_address_), token_(other.token_) {}
        OperationGuard &operator=(OperationGuard &&) = delete;
        ~OperationGuard() { release(); }

        explicit operator bool() const noexcept { return impl_ != nullptr; }

    private:
        void release() noexcept {
            if (impl_ == nullptr)
                return;
            {
                std::lock_guard lock(impl_->mutex);
                const auto found = impl_->operations.find(line_address_);
                if (found != impl_->operations.end() && found->second.token == token_)
                    impl_->operations.erase(found);
            }
            impl_->operation_changed.notify_all();
            impl_ = nullptr;
        }

        Impl *impl_{};
        std::uint64_t line_address_{};
        std::uint64_t token_{};
    };

    Impl(mesi_v2::MesiTransactionEngine &engine_value, CoherenceEndpointConfig config_value)
        : engine(engine_value), config(config_value) {}

    TransactionRequest request(LineState local_state, std::uint64_t installed_epoch, void *grant_context = nullptr,
                               TransactionRequest::GrantInstaller grant_installer = nullptr) noexcept {
        const auto id = next_request_id.fetch_add(1, std::memory_order_relaxed);
        TransactionRequest request{config.endpoint_id, config.session_id,
                                   id == 0 ? std::numeric_limits<std::uint64_t>::max() : id};
        request.local_state = local_state;
        request.installed_epoch = installed_epoch;
        request.grant_context = grant_context;
        request.grant_installer = grant_installer;
        return request;
    }

    static void installGrant(void *opaque, const TransactionResult &result) noexcept {
        auto &context = *static_cast<GrantInstallContext *>(opaque);
        try {
            std::lock_guard lock(context.impl->mutex);
            auto &line = context.impl->lines[context.line_address];
            context.impl->forgetCompletions(context.line_address);
            line.data = context.use_base_data ? context.base_data : result.data;
            if (!context.source.empty())
                std::copy(context.source.begin(), context.source.end(), line.data.begin() + context.offset);
            line.state = lineState(result.transition.snapshot.state);
            line.epoch = result.transition.snapshot.epoch;
            line.snoop_token = 0;
            context.impl->touch(line);
            if (!context.destination.empty())
                std::copy_n(line.data.begin() + context.offset, context.destination.size(),
                            context.destination.begin());
            context.installed = true;
        } catch (...) {
            context.installed = false;
        }
    }

    OperationGuard beginOperation(std::uint64_t line_address) {
        std::unique_lock lock(mutex);
        operation_changed.wait(lock, [&] {
            const auto line = lines.find(line_address);
            return fenced ||
                   (!operations.contains(line_address) && (line == lines.end() || line->second.snoop_token == 0));
        });
        if (fenced)
            return {};
        auto token = next_operation_token++;
        if (token == 0)
            token = next_operation_token++;
        operations.emplace(line_address, LineOperation{token});
        return OperationGuard(*this, line_address, token);
    }

    void touch(CacheLine &line) noexcept { line.lru = ++lru_clock; }

    void forgetCompletions(std::uint64_t line_address) {
        std::erase_if(completions,
                      [line_address](const auto &item) { return item.second.line_address == line_address; });
    }

    bool hasCompletion(std::uint64_t line_address) const {
        return std::any_of(completions.begin(), completions.end(),
                           [line_address](const auto &item) { return item.second.line_address == line_address; });
    }

    ReportedPermission reportedPermission(std::uint64_t line_address) const {
        const auto found = lines.find(line_address);
        if (found == lines.end() || hasCompletion(line_address))
            return {};
        return {found->second.state, found->second.epoch};
    }

    bool permissionChanged(std::uint64_t line_address, ReportedPermission expected) const {
        std::lock_guard lock(mutex);
        return reportedPermission(line_address) != expected;
    }

    bool canRememberCompletion(std::uint64_t line_address, std::uint64_t snoop_id) {
        if (completions.contains(snoop_id) ||
            std::any_of(completions.begin(), completions.end(),
                        [line_address](const auto &item) { return item.second.line_address == line_address; }))
            return true;
        if (completions.size() < completion_capacity)
            return true;
        fenced = true;
        operation_changed.notify_all();
        return false;
    }

    void rememberCompletion(Opcode opcode, const CoherenceFrame &ack) {
        const auto id = protocol_v2::snoopId(ack);
        const auto line_address = protocol_v2::address(ack);
        std::erase_if(completions,
                      [line_address](const auto &item) { return item.second.line_address == line_address; });
        completions[id] = {opcode, line_address, id, protocol_v2::epoch(ack), ack};
    }

    mesi_v2::MesiTransactionEngine &engine;
    const CoherenceEndpointConfig config;
    mutable std::mutex mutex;
    std::condition_variable operation_changed;
    std::unordered_map<std::uint64_t, CacheLine> lines;
    std::unordered_map<std::uint64_t, LineOperation> operations;
    std::unordered_map<std::uint64_t, SnoopCompletion> completions;
    CoherenceEndpointCounters counters;
    std::atomic<std::uint64_t> next_request_id{1};
    std::uint64_t next_snoop_token{1};
    std::uint64_t next_operation_token{1};
    std::uint64_t lru_clock{};
    bool fenced{};
    const std::size_t completion_capacity{std::max<std::size_t>(16, config.capacity_lines * 4)};
};

PreparedSnoopAck::PreparedSnoopAck(CoherenceEndpointCache &endpoint, std::uint64_t line_address, std::uint64_t token,
                                   CoherenceFrame ack) noexcept
    : endpoint_(&endpoint), line_address_(line_address), token_(token), ack_(ack) {}

PreparedSnoopAck::PreparedSnoopAck(PreparedSnoopAck &&other) noexcept
    : endpoint_(std::exchange(other.endpoint_, nullptr)), line_address_(other.line_address_), token_(other.token_),
      ack_(other.ack_), committed_(other.committed_) {}

PreparedSnoopAck &PreparedSnoopAck::operator=(PreparedSnoopAck &&other) noexcept {
    if (this == &other)
        return *this;
    cancel();
    endpoint_ = std::exchange(other.endpoint_, nullptr);
    line_address_ = other.line_address_;
    token_ = other.token_;
    ack_ = other.ack_;
    committed_ = other.committed_;
    return *this;
}

PreparedSnoopAck::~PreparedSnoopAck() { cancel(); }

PreparedSnoopAck::operator bool() const noexcept { return endpoint_ != nullptr; }

bool PreparedSnoopAck::has_value() const noexcept { return endpoint_ != nullptr; }

const CoherenceFrame &PreparedSnoopAck::operator*() const {
    if (endpoint_ != nullptr && !committed_) {
        if (!endpoint_->commitPreparedSnoop(line_address_, token_, ack_)) {
            protocol_v2::setStatus(ack_, Status::InvalidState);
            protocol_v2::setPayloadLength(ack_, 0);
            ack_.data.fill(0);
        }
        committed_ = true;
    }
    return ack_;
}

const CoherenceFrame *PreparedSnoopAck::operator->() const { return &operator*(); }

void PreparedSnoopAck::cancel() noexcept {
    if (endpoint_ != nullptr && !committed_)
        endpoint_->cancelPreparedSnoop(line_address_, token_);
    endpoint_ = nullptr;
}

CoherenceEndpointCache::CoherenceEndpointCache(mesi_v2::MesiTransactionEngine &engine, CoherenceEndpointConfig config)
    : impl_(std::make_unique<Impl>(engine, config)) {
    if (config.endpoint_id >= protocol_v2::kMaximumHosts)
        throw std::invalid_argument("coherence endpoint id is out of range");
    if (config.session_id == 0)
        throw std::invalid_argument("coherence endpoint session id must be nonzero");
    if (config.capacity_lines == 0)
        throw std::invalid_argument("coherence endpoint cache must contain at least one line");
}

CoherenceEndpointCache::~CoherenceEndpointCache() = default;

Status CoherenceEndpointCache::ensureCapacity(std::uint64_t incoming_line) {
    for (;;) {
        std::uint64_t victim_address{};
        {
            std::lock_guard lock(impl_->mutex);
            if (impl_->lines.contains(incoming_line) || impl_->lines.size() < impl_->config.capacity_lines)
                return Status::Ok;

            const auto found =
                std::min_element(impl_->lines.begin(), impl_->lines.end(), [](const auto &left, const auto &right) {
                    return left.second.lru < right.second.lru;
                });
            if (found == impl_->lines.end())
                return Status::InvalidState;
            victim_address = found->first;
        }

        auto victim_operation = impl_->beginOperation(victim_address);
        if (!victim_operation)
            return Status::HostFenced;
        Impl::CacheLine victim;
        {
            std::lock_guard lock(impl_->mutex);
            if (impl_->lines.contains(incoming_line) || impl_->lines.size() < impl_->config.capacity_lines)
                return Status::Ok;
            const auto found = impl_->lines.find(victim_address);
            if (found == impl_->lines.end())
                continue;
            victim = found->second;
        }

        TransactionResult result;
        if (victim.state == LineState::M) {
            {
                std::lock_guard lock(impl_->mutex);
                ++impl_->counters.putm;
            }
            result = impl_->engine.putm(victim_address, impl_->request(victim.state, victim.epoch), victim.epoch,
                                        victim.data);
        } else {
            {
                std::lock_guard lock(impl_->mutex);
                ++impl_->counters.puts;
            }
            result = impl_->engine.puts(victim_address, impl_->request(victim.state, victim.epoch), victim.epoch);
        }
        if (result.status != Status::Ok)
            return result.status;

        std::lock_guard lock(impl_->mutex);
        const auto current = impl_->lines.find(victim_address);
        if (current != impl_->lines.end() && current->second.epoch == victim.epoch) {
            impl_->lines.erase(current);
            ++impl_->counters.evictions;
            if (victim.state == LineState::M) {
                ++impl_->counters.dirty_evictions;
                ++impl_->counters.writebacks;
            } else {
                ++impl_->counters.clean_evictions;
            }
        }
    }
}

Status CoherenceEndpointCache::load(std::uint64_t address, std::span<std::byte> destination) {
    if (!validAccess(address, destination.size()))
        return Status::InvalidState;
    if (destination.empty())
        return Status::Ok;

    const auto line_address = lineAddress(address);
    const auto offset = static_cast<std::size_t>(address - line_address);
    auto operation = impl_->beginOperation(line_address);
    if (!operation)
        return Status::HostFenced;
    {
        std::lock_guard lock(impl_->mutex);
        ++impl_->counters.loads;
        const auto found = impl_->lines.find(line_address);
        if (found != impl_->lines.end()) {
            ++impl_->counters.hits;
            impl_->touch(found->second);
            std::copy_n(found->second.data.begin() + offset, destination.size(), destination.begin());
            return Status::Ok;
        }
        ++impl_->counters.misses;
    }

    if (const auto status = ensureCapacity(line_address); status != Status::Ok)
        return status;
    {
        std::lock_guard lock(impl_->mutex);
        ++impl_->counters.gets;
    }
    Impl::GrantInstallContext install{impl_.get(), line_address, offset, destination};
    const auto result =
        impl_->engine.gets(line_address, impl_->request(LineState::I, 0, &install, &Impl::installGrant));
    if (const auto status = grantedStatus(result); status != Status::Ok)
        return status;
    return install.installed ? Status::Ok : Status::IoError;
}

Status CoherenceEndpointCache::acquireLine(std::uint64_t line_address, RangeIntent intent) {
    if ((line_address & kLineMask) != 0)
        return Status::InvalidState;

    auto operation = impl_->beginOperation(line_address);
    if (!operation)
        return Status::HostFenced;

    Impl::CacheLine cached;
    bool found_line = false;
    {
        std::lock_guard lock(impl_->mutex);
        const auto found = impl_->lines.find(line_address);
        if (found != impl_->lines.end() && !impl_->hasCompletion(line_address)) {
            ++impl_->counters.hits;
            impl_->touch(found->second);
            if (intent == RangeIntent::Read || found->second.state == LineState::M)
                return Status::Ok;
            cached = found->second;
            found_line = true;
        } else {
            ++impl_->counters.misses;
        }
    }

    if (found_line) {
        {
            std::lock_guard lock(impl_->mutex);
            ++impl_->counters.upgrades;
        }
        Impl::GrantInstallContext install{impl_.get(), line_address, 0, {}, {}, cached.data, true};
        const auto result = impl_->engine.upgrade(
            line_address, impl_->request(cached.state, cached.epoch, &install, &Impl::installGrant));
        if (const auto status = grantedStatus(result); status != Status::Ok)
            return status;
        return install.installed ? Status::Ok : Status::IoError;
    }

    if (const auto status = ensureCapacity(line_address); status != Status::Ok)
        return status;

    Impl::GrantInstallContext install{impl_.get(), line_address};
    TransactionResult result;
    if (intent == RangeIntent::Read) {
        {
            std::lock_guard lock(impl_->mutex);
            ++impl_->counters.gets;
        }
        result = impl_->engine.gets(line_address, impl_->request(LineState::I, 0, &install, &Impl::installGrant));
    } else {
        {
            std::lock_guard lock(impl_->mutex);
            ++impl_->counters.getm;
        }
        result = impl_->engine.getm(line_address, impl_->request(LineState::I, 0, &install, &Impl::installGrant));
    }
    if (const auto status = grantedStatus(result); status != Status::Ok)
        return status;
    return install.installed ? Status::Ok : Status::IoError;
}

RangeAcquireResult CoherenceEndpointCache::acquireRange(std::uint64_t address, std::size_t size, RangeIntent intent) {
    if (size == 0)
        return {.status = Status::Ok, .complete = true};
    if (size - 1 > std::numeric_limits<std::uint64_t>::max() - address)
        return {};

    const auto first_line = lineAddress(address);
    const auto last_line = lineAddress(address + size - 1);
    const auto line_count = static_cast<std::size_t>((last_line - first_line) / protocol_v2::kLineSize + 1);
    RangeAcquireResult result{.status = Status::Ok, .lines_requested = line_count};

    for (std::size_t index = 0; index < line_count; ++index) {
        const auto status = acquireLine(first_line + index * protocol_v2::kLineSize, intent);
        if (status != Status::Ok) {
            result.status = status;
            return result;
        }
        ++result.lines_granted;
    }
    result.complete = true;
    return result;
}

Status CoherenceEndpointCache::writeThrough(std::uint64_t line_address) {
    Impl::CacheLine line;
    {
        std::lock_guard lock(impl_->mutex);
        const auto found = impl_->lines.find(line_address);
        if (found == impl_->lines.end() || found->second.state != LineState::M)
            return Status::InvalidState;
        line = found->second;
        ++impl_->counters.putm;
    }

    const auto result =
        impl_->engine.putm(line_address, impl_->request(LineState::M, line.epoch), line.epoch, line.data);
    if (result.status != Status::Ok)
        return result.status;

    std::unique_lock lock(impl_->mutex);
    const auto found = impl_->lines.find(line_address);
    if (found != impl_->lines.end() && found->second.epoch == line.epoch)
        impl_->lines.erase(found);
    ++impl_->counters.writebacks;
    return Status::Ok;
}

Status CoherenceEndpointCache::store(std::uint64_t address, std::span<const std::byte> source) {
    if (!validAccess(address, source.size()))
        return Status::InvalidState;
    if (source.empty())
        return Status::Ok;

    const auto line_address = lineAddress(address);
    const auto offset = static_cast<std::size_t>(address - line_address);
    auto operation = impl_->beginOperation(line_address);
    if (!operation)
        return Status::HostFenced;
    Impl::CacheLine cached;
    bool found_line = false;
    {
        std::lock_guard lock(impl_->mutex);
        ++impl_->counters.stores;
        const auto found = impl_->lines.find(line_address);
        if (found != impl_->lines.end() && !impl_->hasCompletion(line_address)) {
            ++impl_->counters.hits;
            cached = found->second;
            found_line = true;
            if (found->second.state == LineState::M) {
                std::copy(source.begin(), source.end(), found->second.data.begin() + offset);
                impl_->touch(found->second);
                if (impl_->config.write_policy == EndpointWritePolicy::WriteBack)
                    return Status::Ok;
                cached = found->second;
            }
        } else {
            ++impl_->counters.misses;
        }
    }

    if (found_line && cached.state == LineState::M)
        return writeThrough(line_address);

    TransactionResult result;
    if (found_line) {
        {
            std::lock_guard lock(impl_->mutex);
            ++impl_->counters.upgrades;
        }
        Impl::GrantInstallContext install{impl_.get(), line_address, offset, {}, source, cached.data, true};
        result = impl_->engine.upgrade(line_address,
                                       impl_->request(cached.state, cached.epoch, &install, &Impl::installGrant));
        if (const auto status = grantedStatus(result); status != Status::Ok)
            return status;
        if (!install.installed)
            return Status::IoError;
    } else {
        if (const auto status = ensureCapacity(line_address); status != Status::Ok)
            return status;
        {
            std::lock_guard lock(impl_->mutex);
            ++impl_->counters.getm;
        }
        Impl::GrantInstallContext install{impl_.get(), line_address, offset, {}, source};
        result = impl_->engine.getm(line_address, impl_->request(LineState::I, 0, &install, &Impl::installGrant));
        if (const auto status = grantedStatus(result); status != Status::Ok)
            return status;
        if (!install.installed)
            return Status::IoError;
    }
    return impl_->config.write_policy == EndpointWritePolicy::WriteThrough ? writeThrough(line_address) : Status::Ok;
}

TransactionResult CoherenceEndpointCache::fetchAdd(std::uint64_t address, std::uint64_t value) {
    const auto line_address = lineAddress(address);
    auto operation = impl_->beginOperation(line_address);
    if (!operation)
        return {.status = Status::HostFenced};
    if (const auto status = ensureCapacity(line_address); status != Status::Ok)
        return {.status = status};
    {
        std::lock_guard lock(impl_->mutex);
        ++impl_->counters.atomics;
        ++impl_->counters.fetch_adds;
    }
    for (;;) {
        Impl::ReportedPermission permission;
        {
            std::lock_guard lock(impl_->mutex);
            permission = impl_->reportedPermission(line_address);
        }
        Impl::GrantInstallContext install{impl_.get(), line_address};
        auto result = impl_->engine.fetchAdd(
            address, impl_->request(permission.state, permission.epoch, &install, &Impl::installGrant), value);
        if (grantedStatus(result) == Status::Ok && !install.installed)
            result.status = Status::IoError;
        if ((result.status == Status::InvalidState || result.status == Status::StaleEpoch) &&
            impl_->permissionChanged(line_address, permission))
            continue;
        if (result.status == Status::Ok && impl_->config.write_policy == EndpointWritePolicy::WriteThrough)
            result.status = writeThrough(line_address);
        return result;
    }
}

TransactionResult CoherenceEndpointCache::compareExchange(std::uint64_t address, std::uint64_t expected,
                                                          std::uint64_t desired) {
    const auto line_address = lineAddress(address);
    auto operation = impl_->beginOperation(line_address);
    if (!operation)
        return {.status = Status::HostFenced};
    if (const auto status = ensureCapacity(line_address); status != Status::Ok)
        return {.status = status};
    {
        std::lock_guard lock(impl_->mutex);
        ++impl_->counters.atomics;
        ++impl_->counters.compare_exchanges;
    }
    for (;;) {
        Impl::ReportedPermission permission;
        {
            std::lock_guard lock(impl_->mutex);
            permission = impl_->reportedPermission(line_address);
        }
        Impl::GrantInstallContext install{impl_.get(), line_address};
        auto result = impl_->engine.compareExchange(
            address, impl_->request(permission.state, permission.epoch, &install, &Impl::installGrant), expected,
            desired);
        if (grantedStatus(result) == Status::Ok && !install.installed)
            result.status = Status::IoError;
        if ((result.status == Status::InvalidState || result.status == Status::StaleEpoch) &&
            impl_->permissionChanged(line_address, permission))
            continue;
        if (result.status == Status::Ok && impl_->config.write_policy == EndpointWritePolicy::WriteThrough)
            result.status = writeThrough(line_address);
        return result;
    }
}

PreparedSnoopAck CoherenceEndpointCache::processSnoop(const CoherenceFrame &snoop) {
    const auto op = protocol_v2::opcode(snoop);
    const bool supported = op == Opcode::SnpInv || op == Opcode::SnpDowngrade || op == Opcode::SnpDataInv ||
                           op == Opcode::SnpDataDowngrade;
    if (!supported || !protocol_v2::validateFrame(snoop) || protocol_v2::dstHost(snoop) != impl_->config.endpoint_id ||
        protocol_v2::sessionId(snoop) != impl_->config.session_id) {
        std::lock_guard lock(impl_->mutex);
        ++impl_->counters.rejected_snoops;
        return {};
    }

    const auto address = protocol_v2::address(snoop);
    std::unique_lock lock(impl_->mutex);
    const auto exact = impl_->completions.find(protocol_v2::snoopId(snoop));
    if (exact != impl_->completions.end()) {
        const auto &completion = exact->second;
        if (completion.opcode != op || completion.line_address != address ||
            completion.epoch != protocol_v2::epoch(snoop)) {
            ++impl_->counters.rejected_snoops;
            return {};
        }
        ++impl_->counters.snoop_acks;
        PreparedSnoopAck replay(*this, address, 0, completion.ack);
        replay.committed_ = true;
        return replay;
    }

    const auto semantic = std::find_if(impl_->completions.begin(), impl_->completions.end(), [&](const auto &item) {
        return item.second.line_address == address && compatibleReplacement(item.second.opcode, op) &&
               item.second.epoch <= protocol_v2::epoch(snoop) &&
               (!returnsData(op) || protocol_v2::payloadLength(item.second.ack) == protocol_v2::kLineSize);
    });
    if (semantic != impl_->completions.end()) {
        auto current = impl_->lines.find(address);
        const auto completed_post_state = protocol_v2::lineState(semantic->second.ack);
        const bool completion_current =
            (completed_post_state == LineState::I && current == impl_->lines.end()) ||
            (completed_post_state == LineState::S && current != impl_->lines.end() &&
             current->second.state == LineState::S && current->second.epoch == semantic->second.epoch);
        if (completion_current) {
            auto ack = semantic->second.ack;
            protocol_v2::setSnoopId(ack, protocol_v2::snoopId(snoop));
            protocol_v2::setEpoch(ack, protocol_v2::epoch(snoop));
            protocol_v2::setLineState(ack, snoopPostState(op));
            if (returnsData(op)) {
                protocol_v2::setPayloadLength(ack, protocol_v2::kLineSize);
            } else {
                protocol_v2::setPayloadLength(ack, 0);
                ack.data.fill(0);
            }

            if (snoopPostState(op) == LineState::I) {
                if (current != impl_->lines.end())
                    impl_->lines.erase(current);
            } else if (current != impl_->lines.end()) {
                current->second.epoch = protocol_v2::epoch(snoop);
                impl_->touch(current->second);
            }
            impl_->rememberCompletion(op, ack);
            ++impl_->counters.snoop_acks;
            PreparedSnoopAck replay(*this, address, 0, ack);
            replay.committed_ = true;
            return replay;
        }
    }

    if (impl_->fenced) {
        ++impl_->counters.rejected_snoops;
        return {};
    }

    const auto found = impl_->lines.find(address);
    if (found == impl_->lines.end() || found->second.snoop_token != 0) {
        ++impl_->counters.rejected_snoops;
        return {};
    }

    const auto state = found->second.state;
    const bool valid_state = (op == Opcode::SnpInv && (state == LineState::S || state == LineState::E)) ||
                             (op == Opcode::SnpDowngrade && state == LineState::E) ||
                             ((op == Opcode::SnpDataInv || op == Opcode::SnpDataDowngrade) && state == LineState::M);
    const bool valid_epoch = state == LineState::S ? found->second.epoch < protocol_v2::epoch(snoop)
                                                   : found->second.epoch != std::numeric_limits<std::uint64_t>::max() &&
                                                         found->second.epoch + 1 == protocol_v2::epoch(snoop);
    if (!valid_state || !valid_epoch) {
        ++impl_->counters.rejected_snoops;
        return {};
    }
    if (!impl_->canRememberCompletion(address, protocol_v2::snoopId(snoop))) {
        ++impl_->counters.rejected_snoops;
        return {};
    }

    auto ack = protocol_v2::initializeFrame(Opcode::SnoopAck);
    protocol_v2::setSrcHost(ack, impl_->config.endpoint_id);
    protocol_v2::setDstHost(ack, protocol_v2::kServerHost);
    protocol_v2::setSessionId(ack, impl_->config.session_id);
    protocol_v2::setSnoopId(ack, protocol_v2::snoopId(snoop));
    protocol_v2::setAddress(ack, address);
    protocol_v2::setEpoch(ack, protocol_v2::epoch(snoop));
    protocol_v2::setStatus(ack, Status::Ok);
    protocol_v2::setAckStrength(ack, protocol_v2::AckStrength::MODEL);
    const bool downgrade = op == Opcode::SnpDowngrade || op == Opcode::SnpDataDowngrade;
    protocol_v2::setLineState(ack, downgrade ? LineState::S : LineState::I);
    if (op == Opcode::SnpDataInv || op == Opcode::SnpDataDowngrade) {
        protocol_v2::setPayloadLength(ack, protocol_v2::kLineSize);
        std::transform(found->second.data.begin(), found->second.data.end(), ack.data.begin(),
                       [](std::byte byte) { return std::to_integer<std::uint8_t>(byte); });
    }

    auto token = impl_->next_snoop_token++;
    if (token == 0)
        token = impl_->next_snoop_token++;
    found->second.snoop_token = token;
    PreparedSnoopAck completion(*this, address, token, ack);
    lock.unlock();
    if (!commitPreparedSnoop(address, token, ack))
        return {};
    completion.committed_ = true;
    return completion;
}

bool CoherenceEndpointCache::commitPreparedSnoop(std::uint64_t line_address, std::uint64_t token,
                                                 const CoherenceFrame &ack) noexcept {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->lines.find(line_address);
    if (found == impl_->lines.end() || found->second.snoop_token != token)
        return false;

    const auto op_state = protocol_v2::lineState(ack);
    if (op_state == LineState::I) {
        impl_->lines.erase(found);
    } else {
        found->second.state = LineState::S;
        found->second.epoch = protocol_v2::epoch(ack);
        found->second.snoop_token = 0;
        impl_->touch(found->second);
    }

    const bool data = protocol_v2::payloadLength(ack) == protocol_v2::kLineSize;
    const auto completed_opcode = op_state == LineState::I ? (data ? Opcode::SnpDataInv : Opcode::SnpInv)
                                                           : (data ? Opcode::SnpDataDowngrade : Opcode::SnpDowngrade);
    impl_->rememberCompletion(completed_opcode, ack);
    if (op_state == LineState::I)
        data ? ++impl_->counters.snoop_data_inv : ++impl_->counters.snoop_inv;
    else
        data ? ++impl_->counters.snoop_data_downgrade : ++impl_->counters.snoop_downgrade;
    ++impl_->counters.snoop_acks;
    impl_->operation_changed.notify_all();
    return true;
}

void CoherenceEndpointCache::cancelPreparedSnoop(std::uint64_t line_address, std::uint64_t token) noexcept {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->lines.find(line_address);
    if (found != impl_->lines.end() && found->second.snoop_token == token)
        found->second.snoop_token = 0;
    impl_->operation_changed.notify_all();
}

std::uint16_t CoherenceEndpointCache::endpointId() const noexcept { return impl_->config.endpoint_id; }

std::uint64_t CoherenceEndpointCache::sessionId() const noexcept { return impl_->config.session_id; }

bool CoherenceEndpointCache::contains(std::uint64_t address) const {
    std::lock_guard lock(impl_->mutex);
    return impl_->lines.contains(lineAddress(address));
}

bool CoherenceEndpointCache::fenced() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->fenced;
}

std::size_t CoherenceEndpointCache::size() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->lines.size();
}

CoherenceEndpointCounters CoherenceEndpointCache::counters() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->counters;
}

struct InProcessCoherenceTransport::Impl {
    mutable std::mutex mutex;
    mesi_v2::MesiTransactionEngine *engine{};
    std::unordered_map<std::uint16_t, CoherenceEndpointCache *> endpoints;
    InProcessCoherenceTransportCounters counters;
};

InProcessCoherenceTransport::InProcessCoherenceTransport() : impl_(std::make_unique<Impl>()) {}

InProcessCoherenceTransport::~InProcessCoherenceTransport() = default;

void InProcessCoherenceTransport::bindEngine(mesi_v2::MesiTransactionEngine &engine) {
    std::lock_guard lock(impl_->mutex);
    impl_->engine = &engine;
}

bool InProcessCoherenceTransport::registerEndpoint(CoherenceEndpointCache &endpoint) {
    std::lock_guard lock(impl_->mutex);
    const auto [found, inserted] = impl_->endpoints.emplace(endpoint.endpointId(), &endpoint);
    return inserted || found->second == &endpoint;
}

bool InProcessCoherenceTransport::sendToHost(std::uint16_t host_id, const CoherenceFrame &frame) {
    CoherenceEndpointCache *endpoint{};
    mesi_v2::MesiTransactionEngine *engine{};
    {
        std::lock_guard lock(impl_->mutex);
        ++impl_->counters.snoops;
        const auto found = impl_->endpoints.find(host_id);
        if (found != impl_->endpoints.end())
            endpoint = found->second;
        engine = impl_->engine;
    }
    if (endpoint == nullptr || engine == nullptr) {
        std::lock_guard lock(impl_->mutex);
        ++impl_->counters.send_failures;
        return false;
    }

    auto ack = endpoint->processSnoop(frame);
    if (!ack) {
        std::lock_guard lock(impl_->mutex);
        ++impl_->counters.send_failures;
        return false;
    }
    const auto disposition = engine->handleSnoopAck(*ack);
    const bool delivered =
        disposition == mesi_v2::AckDisposition::Accepted || disposition == mesi_v2::AckDisposition::Deferred;
    std::lock_guard lock(impl_->mutex);
    if (delivered)
        ++impl_->counters.snoop_acks;
    else
        ++impl_->counters.send_failures;
    return delivered;
}

InProcessCoherenceTransportCounters InProcessCoherenceTransport::counters() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->counters;
}

} // namespace cxlmemsim
