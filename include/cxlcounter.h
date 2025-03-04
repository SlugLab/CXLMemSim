/*
 * CXLMemSim counter
 *
 *  By: Andrew Quinn
 *      Yiwei Yang
 *      Brian Zhao
 *  SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
 *  Copyright 2025 Regents of the University of California
 *  UC Santa Cruz Sluglab.
 */
#ifndef CXLMEMSIM_CXLCOUNTER_H
#define CXLMEMSIM_CXLCOUNTER_H

#include <cstdint>
#include <atomic>
#include <concepts>
#include <source_location>
#include <expected>
#include <string>
#include <format>
/** TODO: Whether to using the pebs to record the state. add back invalidation migrate huge/ page and prefetch*/
const char loadName[] = "load";
const char storeName[] = "store";
const char conflictName[] = "conflict";
const char migrateInName[] = "migrate_in";
const char migrateOutName[] = "migrate_out";
const char hitOldName[] = "hit_old";
const char localName[] = "local";
const char remoteName[] = "remote";
const char hitmName[] = "hitm";
const char backinvName[] = "backinv";


// 基础计数器模板类
template<const char* Name>
class AtomicCounter {
public:
    std::atomic<uint64_t> value = 0;
    constexpr AtomicCounter() noexcept = default;
    constexpr AtomicCounter(const AtomicCounter& other) noexcept : value(other.value.load()) { // 复制构造函数

    };
    AtomicCounter& operator=(const AtomicCounter&other) {
        if (this != &other) {
            value.store(other.value.load());
        }
        return *this;
    };

    // C++26允许constexpr修饰原子操作
    constexpr void increment() noexcept {
        value.fetch_add(1, std::memory_order_relaxed);
    }

    constexpr uint64_t get() const noexcept {
        return value.load(std::memory_order_relaxed);
    }

    constexpr operator uint64_t() const noexcept {
        return get();
    }

    // 用于可能需要记录的事件日志
    void log_increment(std::source_location loc = std::source_location::current()) const {
        // 未来可以实现日志记录
    }
};
// 确保AtomicCounter满足Counter概念
template<const char* Name>
inline constexpr bool implements_counter_concept = requires(AtomicCounter<Name> t) {
    { t.value } -> std::convertible_to<uint64_t>;
    { t.increment() } -> std::same_as<void>;
};


template <const char* Name>
struct std::formatter<AtomicCounter<Name>> {
    constexpr auto parse(std::format_parse_context& ctx) -> decltype(ctx.begin()) {
        return ctx.end();
    }

    template <typename FormatContext>
    auto format(const AtomicCounter<Name>& counter, FormatContext& ctx) const -> decltype(ctx.out()) {
        // 简化formatter实现，避免嵌套std::format调用
        return format_to(ctx.out(), "{}", counter.get());
    }
};

// 使用C++20的enum class和强类型枚举
enum class EventType {
    Load,
    Store,
    Conflict,
    MigrateIn,
    MigrateOut,
    HitOld,
    Local,
    Remote,
    Hitm
};

// 开关事件计数器
class CXLSwitchEvent {
public:
    AtomicCounter<loadName> load;
    AtomicCounter<storeName> store;
    AtomicCounter<conflictName> conflict;

    constexpr CXLSwitchEvent() noexcept = default;

    // 使用C++20的模板元编程和编译期字符串实现更灵活的接口
    template<EventType Type>
    constexpr void increment() noexcept {
        if constexpr (Type == EventType::Load) {
            load.increment();
        } else if constexpr (Type == EventType::Store) {
            store.increment();
        } else if constexpr (Type == EventType::Conflict) {
            conflict.increment();
        }
    }

    constexpr void inc_load() noexcept { load.increment(); }
    constexpr void inc_store() noexcept { store.increment(); }
    constexpr void inc_conflict() noexcept { conflict.increment(); }

    // 使用C++23的auto模板参数实现更灵活的统计功能
    template<auto Field>
    constexpr uint64_t get() const noexcept {
        if constexpr (Field == &CXLSwitchEvent::load) {
            return load;
        } else if constexpr (Field == &CXLSwitchEvent::store) {
            return store;
        } else if constexpr (Field == &CXLSwitchEvent::conflict) {
            return conflict;
        }
    }
};

// 内存扩展器事件计数器
class CXLMemExpanderEvent {
public:
    AtomicCounter<loadName> load;
    AtomicCounter<storeName> store;
    AtomicCounter<migrateInName> migrate_in;
    AtomicCounter<migrateOutName> migrate_out;
    AtomicCounter<hitOldName> hit_old;

    constexpr CXLMemExpanderEvent() noexcept = default;

    template<EventType Type>
    constexpr void increment() noexcept {
        if constexpr (Type == EventType::Load) {
            load.increment();
        } else if constexpr (Type == EventType::Store) {
            store.increment();
        } else if constexpr (Type == EventType::MigrateIn) {
            migrate_in.increment();
        } else if constexpr (Type == EventType::MigrateOut) {
            migrate_out.increment();
        } else if constexpr (Type == EventType::HitOld) {
            hit_old.increment();
        }
    }

    constexpr void inc_load() noexcept { load.increment(); }
    constexpr void inc_store() noexcept { store.increment(); }
    constexpr void inc_migrate_in() noexcept { migrate_in.increment(); }
    constexpr void inc_migrate_out() noexcept { migrate_out.increment(); }
    constexpr void inc_hit_old() noexcept { hit_old.increment(); }

    // 统计方法
    constexpr uint64_t total_operations() const noexcept {
        return load + store + migrate_in + migrate_out + hit_old;
    }

    // 使用C++23的expected获取操作结果
    std::expected<uint64_t, std::string> safe_get(EventType type) const noexcept {
        switch (type) {
            case EventType::Load: return load;
            case EventType::Store: return store;
            case EventType::MigrateIn: return migrate_in;
            case EventType::MigrateOut: return migrate_out;
            case EventType::HitOld: return hit_old;
            default: return std::unexpected(std::string("Invalid event type for CXLMemExpanderEvent"));
        }
    }
};

// 通用计数器
class CXLCounter {
public:
    AtomicCounter<localName> local;
    AtomicCounter<remoteName> remote;
    AtomicCounter<hitmName> hitm;
    AtomicCounter<backinvName> backinv;

    constexpr CXLCounter() noexcept = default;

    template<EventType Type>
    constexpr void increment() noexcept {
        if constexpr (Type == EventType::Local) {
            local.increment();
        } else if constexpr (Type == EventType::Remote) {
            remote.increment();
        } else if constexpr (Type == EventType::Hitm) {
            hitm.increment();
        }
    }

    constexpr void inc_local() noexcept { local.increment(); }
    constexpr void inc_remote() noexcept { remote.increment(); }
    constexpr void inc_hitm() noexcept { hitm.increment(); }
    constexpr void inc_backinv() noexcept { backinv.increment(); }

    // 便捷方法:计算本地命中率
    constexpr double local_hit_ratio() const noexcept {
        uint64_t total = local + remote;
        return total > 0 ? static_cast<double>(local) / total : 0.0;
    }
};

// 实现部分现在可以以内联方式编写，不需要单独的.cpp文件
// 但为了兼容性，我们仍保留.cpp文件的实现

#endif // CXLMEMSIM_CXLCOUNTER_H