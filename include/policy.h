//
// Created by victoryang00 on 1/12/23.
//

#ifndef CXL_MEM_SIMULATOR_POLICY_H
#define CXL_MEM_SIMULATOR_POLICY_H
#include "cxlcontroller.h"
#include "cxlendpoint.h"
#include "helper.h"
#include <map>

// Saturate Local 90% and start interleave accrodingly the remote with topology
// Say 3 remote, 2 200ns, 1 400ns, will give 40% 40% 20%
class InterleavePolicy : public Policy {

public:
    InterleavePolicy();
    int last_remote = 0;
    int all_size = 0;
    std::vector<double> percentage;
    int compute_once(CXLController *) override;
};

#endif // CXL_MEM_SIMULATOR_POLICY_H
