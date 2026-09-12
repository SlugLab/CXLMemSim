#!/usr/bin/env bash
# Full C2 Sight Type-2 calibration test suite.
#
# Runs every check end to end against the currently built QEMU and simulator
# and prints a pass/fail summary:
#
#   1. functional regression   qtest_type2_paper.py (23 checks, fresh process)
#   2. timing-model smoke      qtest_notify_smoke.py (19 exact checks)
#   3. notify batch bench      qtest_notify_bench.py  (--repeats 1)
#   4. sharing motivation      qtest_sharing_motivation.py
#   5. DCOH backpressure       qtest_dcoh_bench.py
#   6. device-count scaling    qtest_scale_bench.py (--counts 1 4 32)
#   7. real-HW spot check      cxl_notify_bench (optional, --with-hw)
#
# Usage: ./run_full_suite.sh [--with-hw] [--skip-1st]
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
QEMU_INTEG="$(cd "$HERE/../.." && pwd)/qemu_integration"
RESULTS="$HERE/notify_bench_results"
mkdir -p "$RESULTS"

WITH_HW=0
for arg in "$@"; do
    case "$arg" in
        --with-hw) WITH_HW=1 ;;
    esac
done

declare -a NAMES=() STATS=()
record() { NAMES+=("$1"); STATS+=("$2"); }

run_step() {
    local name="$1"; shift
    echo ""
    echo "=== $name ==="
    local log
    log=$(mktemp /tmp/suite.XXXXXX)
    if "$@" >"$log" 2>&1; then
        record "$name" "PASS"
        tail -1 "$log"
    else
        record "$name" "FAIL"
        tail -15 "$log"
    fi
    rm -f "$log"
}

run_step "1. functional regression (23 checks)" \
    python3 "$QEMU_INTEG/qtest_type2_paper.py" --out "/tmp/suite-func-$$" \
        --repeats 1 --quick

run_step "2. timing-model smoke (19 exact checks)" \
    python3 "$HERE/qtest_notify_smoke.py"

run_step "3. notify batch bench" \
    python3 "$HERE/qtest_notify_bench.py" --repeats 1

run_step "4. sharing motivation" \
    python3 "$HERE/qtest_sharing_motivation.py"

run_step "5. DCOH allocation backpressure" \
    python3 "$HERE/qtest_dcoh_bench.py"

run_step "6. device-count scaling (1/4/32)" \
    python3 "$HERE/qtest_scale_bench.py" --counts 1 4 32

if [[ "$WITH_HW" == 1 ]]; then
    BENCH="/root/Splash/bench/cxl_notify_bench/cxl_notify_bench"
    if [[ -x "$BENCH" ]]; then
        echo ""
        echo "=== 7. real-HW spot check (CXL node, roundtrip) ==="
        if "$BENCH" 1 roundtrip 65536 1 >/dev/null 2>&1; then
            record "7. real-HW spot check" "PASS"
        else
            record "7. real-HW spot check" "FAIL"
        fi
    else
        record "7. real-HW spot check" "SKIP (bench not built)"
    fi
fi

echo ""
echo "==================== suite summary ===================="
fail=0
for i in "${!NAMES[@]}"; do
    printf "%-42s %s\n" "${NAMES[$i]}" "${STATS[$i]}"
    [[ "${STATS[$i]}" == PASS ]] || fail=1
done
echo "======================================================="
[[ $fail == 0 ]] && echo "SUITE: ALL PASS" || echo "SUITE: FAILURES PRESENT"
exit $fail
