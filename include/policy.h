//
// Created by victoryang00 on 1/12/23.
//

#ifndef CXL_MEM_SIMULATOR_POLICY_H
#define CXL_MEM_SIMULATOR_POLICY_H
#include "helper.h"
#include <map>
class Policy {
public:

    Policy() {
    }
    void construct_topo(std::string);
};
// Saturate Local 90% and start interleave accrodingly the remote with topology
// Say 3 remote, 2 200ns, 1 400ns, will give 40% 40% 20%
class InterleavePolicy : public Policy {
    InterleavePolicy()
        : Policy() {}

};



#endif // CXL_MEM_SIMULATOR_POLICY_H
