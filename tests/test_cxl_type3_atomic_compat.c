#include "cxl_type3_atomic_compat.h"

#include <stdint.h>

int main(void) {
    _Atomic uint64_t value = 7;
    uint64_t old_value;

    old_value = atomic_fetch_add_explicit(&value, 5, memory_order_acq_rel);
    if (old_value != 7 || atomic_load_explicit(&value, memory_order_relaxed) != 12) {
        return 1;
    }
    return 0;
}
