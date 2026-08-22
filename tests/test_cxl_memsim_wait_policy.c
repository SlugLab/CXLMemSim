#include "hw/cxl/cxl_memsim_wait.h"

#include <stdio.h>

static int failures;

static void expect_action(CXLMemSimWaitAction actual, CXLMemSimWaitAction expected, const char *message) {
    if (actual != expected) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

static void testElapsedTimeInsideHotWindowSpins(void) {
    expect_action(cxl_memsim_wait_action(0, 50000), CXL_MEMSIM_WAIT_SPIN, "start of hot window must spin");
    expect_action(cxl_memsim_wait_action(49999, 50000), CXL_MEMSIM_WAIT_SPIN,
                  "last nanosecond of hot window must spin");
}

static void testElapsedTimeAtBoundarySleeps(void) {
    expect_action(cxl_memsim_wait_action(50000, 50000), CXL_MEMSIM_WAIT_SLEEP, "hot-window boundary must sleep");
    expect_action(cxl_memsim_wait_action(50001, 50000), CXL_MEMSIM_WAIT_SLEEP, "elapsed cold path must sleep");
    expect_action(cxl_memsim_wait_action(0, 0), CXL_MEMSIM_WAIT_SLEEP, "zero spin interval must sleep immediately");
}

int main(void) {
    testElapsedTimeInsideHotWindowSpins();
    testElapsedTimeAtBoundarySleeps();
    return failures == 0 ? 0 : 1;
}
