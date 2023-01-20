//
// Created by victoryang00 on 1/12/23.
//

#include "policy.h"
Policy::Policy() {

}
InterleavePolicy::InterleavePolicy() {

}
CXLEndPoint *InterleavePolicy::compute_once() {
    return Policy::compute_once();
}
