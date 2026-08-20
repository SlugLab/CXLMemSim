#include "pgas_poll_policy.h"

#include <cassert>
#include <cstdint>

static void testRecentActivityKeepsWorkerSpinning() {
    PgasPollPolicy policy(50'000, 2);

    assert(policy.next(true, 1'000'000) == PgasPollAction::Spin);
    assert(policy.next(false, 1'049'999) == PgasPollAction::Spin);
}

static void testExpiredActiveWindowYieldsThenSleeps() {
    PgasPollPolicy policy(50'000, 2);

    assert(policy.next(true, 1'000'000) == PgasPollAction::Spin);
    assert(policy.next(false, 1'050'000) == PgasPollAction::Yield);
    assert(policy.next(false, 1'050'001) == PgasPollAction::Yield);
    assert(policy.next(false, 1'050'002) == PgasPollAction::Sleep);
}

static void testNewActivityResetsIdleBackoff() {
    PgasPollPolicy policy(50'000, 1);

    assert(policy.next(false, 1'000'000) == PgasPollAction::Sleep);
    assert(policy.next(true, 2'000'000) == PgasPollAction::Spin);
    assert(policy.next(false, 2'050'000) == PgasPollAction::Yield);
    assert(policy.next(true, 3'000'000) == PgasPollAction::Spin);
    assert(policy.next(false, 3'050'000) == PgasPollAction::Yield);
}

static void testWorkerCountValidationProtectsSlotOwnership() {
    PgasPollConfig config;

    config.workers = 0;
    assert(!isValidPgasPollConfig(config, 256));

    config.workers = 257;
    assert(!isValidPgasPollConfig(config, 256));

    config.workers = 1;
    assert(isValidPgasPollConfig(config, 256));

    config.workers = 256;
    assert(isValidPgasPollConfig(config, 256));
}

int main() {
    testRecentActivityKeepsWorkerSpinning();
    testExpiredActiveWindowYieldsThenSleeps();
    testNewActivityResetsIdleBackoff();
    testWorkerCountValidationProtectsSlotOwnership();
    return 0;
}
