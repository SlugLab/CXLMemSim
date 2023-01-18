//
// Created by victoryang00 on 1/13/23.
//

#include "cxlendpoint.h"
CXLMemExpander::CXLMemExpander(int read_bw, int write_bw, int read_lat, int write_lat, int id) {
    this->bandwidth.read = read_bw;
    this->bandwidth.write = write_bw;
    this->latency.read = read_lat;
    this->latency.write = write_lat;
    this->id = id;
}
double CXLMemExpander::calculate_latency(double weight, struct Elem *elem) {

    return 0;
}
double CXLMemExpander::calculate_bandwidth(double weight, struct Elem *elem) { return 0; }
void CXLMemExpander::delete_entry(uint64_t addr) {
    for (auto it = occupation.begin(); it != occupation.end();) {
        if (it->first == addr)
            it = occupation.erase(it);
        else
            ++it;
    }
}
void CXLMemExpander::va_to_pa() {

}
void CXLMemExpander::add_lazy_add(uint64_t addr) {}
void CXLMemExpander::add_lazy_remove(uint64_t addr) {}

void CXLMemExpander::insert(uint64_t timestamp, uint64_t size) {
    this->occupation.insert(timestamp,size);
}
void CXLMemExpander::output() {
    std::cout<<"CXLMemEx
}
void CXLSwitch::output() {

}
