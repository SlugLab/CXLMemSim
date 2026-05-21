#include "dcd_gfam.h"

#include <cstdlib>
#include <iostream>
#include <limits>

#define REQUIRE(condition)                                                                                             \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            std::cerr << "Requirement failed: " #condition << "\n";                                                    \
            std::exit(1);                                                                                              \
        }                                                                                                              \
    } while (0)

int main() {
    DynamicCapacityDevice dcd(1024, 64);

    auto first = dcd.add_capacity(std::numeric_limits<uint64_t>::max(), 128, 42, 1);
    REQUIRE(first.status == DCDStatus::OK);
    REQUIRE(first.base == 0);
    REQUIRE(first.size == 128);
    REQUIRE(first.tag == 42);
    REQUIRE(dcd.is_allocated(64, 64));

    auto overlap = dcd.add_capacity(64, 64, 43, 2);
    REQUIRE(overlap.status == DCDStatus::OVERLAP);

    auto second = dcd.add_capacity(std::numeric_limits<uint64_t>::max(), 128, 44, 3);
    REQUIRE(second.status == DCDStatus::OK);
    REQUIRE(second.base == 128);

    auto release_status = dcd.release_capacity(0, 128, 42, 4);
    REQUIRE(release_status == DCDStatus::OK);
    REQUIRE(!dcd.is_allocated(0, 64));
    REQUIRE(dcd.is_allocated(128, 128));

    GFAMDevice gfam(&dcd, 80.0, 64.0);
    gfam.register_host(0, "host0");
    gfam.register_host(1, "host1");

    REQUIRE(gfam.grant_access(0, 128, 128, DCD_PERM_READ) == DCDStatus::OK);
    auto read = gfam.record_access(0, 128, 64, false, false, 5);
    REQUIRE(read.allowed);
    REQUIRE(read.latency_ns > 0.0);

    auto write_denied = gfam.record_access(0, 128, 64, true, false, 6);
    REQUIRE(!write_denied.allowed);

    REQUIRE(gfam.grant_access(0, 128, 128, DCD_PERM_ALL) == DCDStatus::OK);
    auto write = gfam.record_access(0, 128, 64, true, false, 7);
    REQUIRE(write.allowed);

    auto stats = gfam.stats();
    REQUIRE(stats.hosts == 2);
    REQUIRE(stats.denied_accesses == 1);
    REQUIRE(stats.read_ops == 1);
    REQUIRE(stats.write_ops == 1);

    return 0;
}
