#include "hw/cxl/cxl_memsim_wait.h"

#include <assert.h>

static void testElapsedTimeInsideHotWindowSpins(void) {
    assert(cxl_memsim_wait_action(0, 50000) == CXL_MEMSIM_WAIT_SPIN);
    assert(cxl_memsim_wait_action(49999, 50000) == CXL_MEMSIM_WAIT_SPIN);
}

static void testElapsedTimeAtBoundarySleeps(void) {
    assert(cxl_memsim_wait_action(50000, 50000) == CXL_MEMSIM_WAIT_SLEEP);
    assert(cxl_memsim_wait_action(50001, 50000) == CXL_MEMSIM_WAIT_SLEEP);
    assert(cxl_memsim_wait_action(0, 0) == CXL_MEMSIM_WAIT_SLEEP);
}

int main(void) {
    testElapsedTimeInsideHotWindowSpins();
    testElapsedTimeAtBoundarySleeps();
    return 0;
}
