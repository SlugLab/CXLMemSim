//
// Created by victoryang00 on 1/12/23.
//

#ifndef CXLMEMSIM_CXLCOUNTER_H
#define CXLMEMSIM_CXLCOUNTER_H

#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

/** TODO: Whether to using the pebs to record the state. add back invalidation migrate huge/ page and prefetch*/
class CXLSwitchEvent {
public:
    uint64_t load = 0;
    uint64_t store = 0;
    uint64_t conflict = 0;

    CXLSwitchEvent() = default;
    CXLSwitchEvent(const CXLSwitchEvent &other) = default;
    void inc_load();
    void inc_store();
    void inc_conflict();
};
class CXLMemExpanderEvent {
public:
    uint64_t load = 0;
    uint64_t store = 0;
    uint64_t migrate = 0;
    uint64_t hit_old = 0;

    CXLMemExpanderEvent() = default;
    CXLMemExpanderEvent(const CXLMemExpanderEvent &other) = default;
    void inc_load();
    void inc_store();
    void inc_migrate();
    void inc_hit_old();
};
class CXLCounter {
public:
    uint64_t local = 0;
    uint64_t remote = 0;
    uint64_t hitm = 0;

    CXLCounter() = default;
    CXLCounter(const CXLCounter &other) = default;
    void inc_local();
    void inc_remote();
    void inc_hitm();
};

#endif // CXLMEMSIM_CXLCOUNTER_H
