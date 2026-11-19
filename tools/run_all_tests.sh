#!/bin/bash
# run_all_tests.sh - CURVEBALL round 3, Task D.5: one command, one summary table.
#
#   tools/run_all_tests.sh
#
# Runs, in order: style check -> host unit/filtering/transfer tests -> host integration
# (real lora_mesh.c / wifi_mesh.c) -> backend pytest -> ml/tests (read-only, never changed here).
# Each stage that cannot run at all (a missing tool) is reported SKIPPED, not silently dropped and
# not counted as a pass; nothing here claims a result it did not actually produce.
#
# Needs, to run everything: a C11 compiler on PATH (cmake optional -- falls back to a manual gcc
# build of exactly the sources host/CMakeLists.txt lists if cmake is missing), bash, Python 3 with
# pytest installed (`pip install pytest`) and this repo's backend/requirements.txt
# (`pip install -r backend/requirements.txt`) for the backend suite, and Pillow
# (`pip install pillow`) only if ml/tests needs it (check ml/requirements.txt). The host
# integration stage additionally needs nothing beyond the C compiler -- it links the real
# third_party/lora_mesh and third_party/wifi_mesh sources directly.
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

declare -a STAGE_NAMES
declare -a STAGE_RESULTS # PASS, FAIL, or SKIPPED
record() { STAGE_NAMES+=("$1"); STAGE_RESULTS+=("$2"); }

hr() { printf '%s\n' "------------------------------------------------------------"; }

# ---- 1. style check ------------------------------------------------------------------------
hr
echo "1/5 style check (tools/check_style.sh)"
hr
if bash tools/check_style.sh; then
    record "style check" PASS
else
    record "style check" FAIL
fi

# ---- 2. host unit/filtering/transfer tests -------------------------------------------------
hr
echo "2/5 host tests (test_units, test_transfer, test_filtering)"
hr
HOST_BUILD="$(mktemp -d)"
build_and_run_host_tests() {
    CC="${CC:-cc}"
    command -v "$CC" >/dev/null 2>&1 || CC=gcc
    if ! command -v "$CC" >/dev/null 2>&1; then
        echo "SKIPPED: no C compiler (cc/gcc) on PATH"
        return 2
    fi
    if command -v cmake >/dev/null 2>&1; then
        echo "cmake found: configuring host/ the normal way"
        ( cmake -S host -B "$HOST_BUILD/cmake" >/tmp/run_all_tests_cmake.log 2>&1 &&
          cmake --build "$HOST_BUILD/cmake" -j >>/tmp/run_all_tests_cmake.log 2>&1 &&
          ctest --test-dir "$HOST_BUILD/cmake" --output-on-failure )
        return $?
    fi
    echo "cmake not found: compiling the exact sources host/CMakeLists.txt lists, by hand"
    cd "$HOST_BUILD"
    local WARN="-Wall -Wextra -Wshadow"
    "$CC" -std=c11 $WARN -O1 -g \
        -I "$ROOT/alfp/include" -I "$ROOT/mesh" -I "$ROOT/common" -I "$ROOT/host/sim" \
        -c "$ROOT"/alfp/src/*.c "$ROOT/common/uplink.c" || { echo "FAILED: alfp library"; return 1; }
    "$CC" -std=c11 $WARN -O1 -g \
        -I "$ROOT/alfp/include" -I "$ROOT/mesh" -I "$ROOT/common" -I "$ROOT/host/sim" \
        -c "$ROOT"/host/sim/*.c || { echo "FAILED: sim library (host/sim/storage_posix.c needs a real POSIX mkdir(path,mode) -- MinGW on native Windows does not have one; use WSL/Linux/Mac)"; return 1; }
    local OBJS=(*.o)
    "$CC" -std=c11 $WARN -O1 -g -I "$ROOT/alfp/include" -I "$ROOT/mesh" -I "$ROOT/common" \
        -o test_units "$ROOT/host/tests/test_units.c" "${OBJS[@]}" -lm || return 1
    "$CC" -std=c11 $WARN -O1 -g -I "$ROOT/alfp/include" -I "$ROOT/mesh" -I "$ROOT/common" \
        -I "$ROOT/host/sim" -I "$ROOT/host/tests" \
        -o test_transfer "$ROOT/host/tests/test_transfer.c" "${OBJS[@]}" -lm || return 1
    "$CC" -std=c11 $WARN -O1 -g -I "$ROOT/alfp/include" -I "$ROOT/mesh" -I "$ROOT/common" \
        -I "$ROOT/host/sim" -I "$ROOT/host/tests" \
        -o test_filtering "$ROOT/host/tests/test_filtering.c" "${OBJS[@]}" -lm || return 1
    ./test_units && ./test_transfer && ./test_filtering
    local rc=$?
    cd "$ROOT"
    return $rc
}
build_and_run_host_tests
host_rc=$?
cd "$ROOT" # the function may have cd'd into a build dir and returned early on a failure path
if [ "$host_rc" -eq 0 ]; then
    record "host tests" PASS
elif [ "$host_rc" -eq 2 ]; then
    record "host tests" SKIPPED
else
    record "host tests" FAIL
fi
rm -rf "$HOST_BUILD"

# ---- 3. host integration (real mesh code) --------------------------------------------------
hr
echo "3/5 host integration (host/integration/run_integration.sh, run_wifi_integration.sh)"
hr
if command -v gcc >/dev/null 2>&1 || command -v cc >/dev/null 2>&1; then
    if bash host/integration/run_integration.sh 0.2 && bash host/integration/run_wifi_integration.sh 0.2; then
        record "host integration" PASS
    else
        record "host integration" FAIL
    fi
    echo "(full size ladder not run here: host/integration/run_size_ladder.sh --quick or the full ladder"
    echo " takes several extra minutes; run it separately)"
else
    echo "SKIPPED: no C compiler on PATH"
    record "host integration" SKIPPED
fi

# ---- 4. backend pytest ----------------------------------------------------------------------
hr
echo "4/5 backend tests (backend/tests/, pytest)"
hr
if command -v python3 >/dev/null 2>&1 || command -v python >/dev/null 2>&1; then
    PY="$(command -v python3 || command -v python)"
    if "$PY" -m pytest --version >/dev/null 2>&1; then
        if "$PY" -m pytest backend/tests -q; then
            record "backend tests" PASS
        else
            record "backend tests" FAIL
        fi
    else
        echo "SKIPPED: pytest not installed (pip install pytest)"
        record "backend tests" SKIPPED
    fi
else
    echo "SKIPPED: no python on PATH"
    record "backend tests" SKIPPED
fi

# ---- 5. ml/tests (read-only) ------------------------------------------------------------------
hr
echo "5/5 ml/tests (read-only -- never modified by this script or this round's work)"
hr
if command -v python3 >/dev/null 2>&1 || command -v python >/dev/null 2>&1; then
    PY="$(command -v python3 || command -v python)"
    if "$PY" -m pytest --version >/dev/null 2>&1; then
        if "$PY" -m pytest ml/tests -q; then
            record "ml/tests" PASS
        else
            record "ml/tests" FAIL
        fi
    else
        echo "SKIPPED: pytest not installed"
        record "ml/tests" SKIPPED
    fi
else
    echo "SKIPPED: no python on PATH"
    record "ml/tests" SKIPPED
fi

# ---- summary ----------------------------------------------------------------------------------
hr
echo "SUMMARY"
hr
printf '%-20s %s\n' "STAGE" "RESULT"
n_fail=0
for i in "${!STAGE_NAMES[@]}"; do
    printf '%-20s %s\n' "${STAGE_NAMES[$i]}" "${STAGE_RESULTS[$i]}"
    [ "${STAGE_RESULTS[$i]}" = "FAIL" ] && n_fail=$((n_fail + 1))
done
hr
if [ "$n_fail" -gt 0 ]; then
    echo "$n_fail stage(s) FAILED"
    exit 1
fi
echo "No stage failed (SKIPPED stages were not run, see above -- not the same as PASS)"
exit 0
