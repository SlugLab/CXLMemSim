//
// Created by victoryang00 on 1/12/23.
//

#include "uncore.h"
Uncore::Uncore(const uint32_t unc_idx) {

}
Uncore::~Uncore() {}

int Uncore::read_cbo_elems(struct CBOElem *elem) {
    int r = this->perf.read_pmu(&elem->llc_wb);
    if (r < 0) {
        LOG(ERROR) << fmt::format("perf_read_pmu failed.\n");
    }

    LOG(DEBUG) << fmt::format("llc_wb:{}\n", elem->llc_wb);
    return r;
}
