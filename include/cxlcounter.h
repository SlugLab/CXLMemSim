//
// Created by victoryang00 on 1/12/23.
//

#ifndef CXL_MEM_SIMULATOR_CXLCOUNTER_H
#define CXL_MEM_SIMULATOR_CXLCOUNTER_H

#include <cstdint>
#include <vector>

/** TODO: Whether to using the pebs to record the state. */
class CXLSwitchEvent {
    uint64_t load=0;
    uint64_t store=0;
    uint64_t conflict=0;

    void inc_load();
    void inc_store();
    void inc_conflict();
};
class CXLMemExpanderEvent {
    uint64_t load=0;
    uint64_t store=0;
    uint64_t migrate=0;
    uint64_t hit_old=0;

    void inc_load();
    void inc_store();
    void inc_migrate();
    void inc_hit_old();
};
class CXLCounter{
    uint64_t local=0;
    uint64_t remote=0;
    uint64_t hitm=0;

    void inc_load();
    void inc_store();
    void inc_migrate();
    void inc_hit_old();
};

#endif // CXL_MEM_SIMULATOR_CXLCOUNTER_H
