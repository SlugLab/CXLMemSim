//
// Created by victoryang00 on 1/13/23.
//

#include "cxlendpoint.h"
CXLEndPoint::CXLEndPoint(int read_bw, int write_bw, int read_lat, int write_lat, int id) {
    this->bandwidth.read = read_bw;
    this->bandwidth.write = write_bw;
    this->latency.read = read_lat;
    this->latency.write = write_lat;
    this->id = id;
}
double CXLEndPoint::calculate_latency(double weight, struct Elem *elem) { return 0; }
double CXLEndPoint::calculate_bandwidth(double weight, struct Elem *elem) { return 0; }
void CXLEndPoint::delete_entry(uint64_t addr) {
    for (auto it = occupation.begin(); it != occupation.end();) {
        if (it->first == addr)
            it = occupation.erase(it);
        else
            ++it;
    }
}
