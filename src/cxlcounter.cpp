//
// Created by victoryang00 on 1/12/23.
//

#include "cxlcounter.h"

CXLCounter::CXLCounter()
    : cxl_mem_load_miss_retired_remote_dram({0}), cxl_mem_load_miss_retired_remote_fwd({0}),
      cxl_mem_miss_retired_local_dram(0) {}
