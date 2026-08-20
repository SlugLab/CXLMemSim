#include "pgas_poll_policy.h"

#include <cstdint>
#include <iostream>

static int failures;

static void expectAction(PgasPollAction actual, PgasPollAction expected, const char *message) {
    if (actual != expected) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

static void expectCondition(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

static void testRecentActivityKeepsWorkerSpinning() {
    PgasPollPolicy policy(50'000, 2);

    expectAction(policy.next(true, 1'000'000), PgasPollAction::Spin, "activity must select spin");
    expectAction(policy.next(false, 1'049'999), PgasPollAction::Spin, "worker must spin inside the active window");
}

static void testExpiredActiveWindowYieldsThenSleeps() {
    PgasPollPolicy policy(50'000, 2);

    expectAction(policy.next(true, 1'000'000), PgasPollAction::Spin, "activity must select spin");
    expectAction(policy.next(false, 1'050'000), PgasPollAction::Yield, "active-window boundary must start yielding");
    expectAction(policy.next(false, 1'050'001), PgasPollAction::Yield, "configured second yield must be honored");
    expectAction(policy.next(false, 1'050'002), PgasPollAction::Sleep, "worker must sleep after configured yields");
}

static void testNewActivityResetsIdleBackoff() {
    PgasPollPolicy policy(50'000, 1);

    expectAction(policy.next(false, 1'000'000), PgasPollAction::Sleep, "never-active worker must remain asleep");
    expectAction(policy.next(true, 2'000'000), PgasPollAction::Spin, "new activity must select spin");
    expectAction(policy.next(false, 2'050'000), PgasPollAction::Yield, "first idle transition must yield");
    expectAction(policy.next(true, 3'000'000), PgasPollAction::Spin, "later activity must reset idle backoff");
    expectAction(policy.next(false, 3'050'000), PgasPollAction::Yield,
                 "reset backoff must yield again before sleeping");
}

static void testWorkerCountValidationProtectsSlotOwnership() {
    PgasPollConfig config;

    config.workers = 0;
    expectCondition(!isValidPgasPollConfig(config, 256), "zero workers must be rejected");

    config.workers = 257;
    expectCondition(!isValidPgasPollConfig(config, 256), "workers beyond slot count must be rejected");

    config.workers = 1;
    expectCondition(isValidPgasPollConfig(config, 256), "one worker must be accepted");

    config.workers = 256;
    expectCondition(isValidPgasPollConfig(config, 256), "one worker per slot must be accepted");
}

int main() {
    testRecentActivityKeepsWorkerSpinning();
    testExpiredActiveWindowYieldsThenSleeps();
    testNewActivityResetsIdleBackoff();
    testWorkerCountValidationProtectsSlotOwnership();
    return failures == 0 ? 0 : 1;
}
