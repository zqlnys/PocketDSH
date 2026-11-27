#!/bin/sh
# Reports interpreter throughput on a fixed workload and fails if it regresses
# badly. A per-instruction getenv used to hide in the step loop and cost more than
# the interpreter itself: 21 calls per instruction, twice the runtime.
set -eu
BIN=${1:-./tools/rune}
ELF=${2:-tests/guest/loop.elf}
if [ ! -x "$BIN" ]; then echo "perf-check: $BIN missing" >&2; exit 1; fi
if [ ! -f "$ELF" ]; then echo "perf-check: $ELF missing (guest loop image)" >&2; exit 0; fi
start=$(date +%s%N)
out=$("$BIN" "$ELF" 2>&1 | grep -o 'instructions=[0-9]*' || true)
end=$(date +%s%N)
n=${out#instructions=}
if [ -z "$n" ]; then echo "perf-check: no instruction count" >&2; exit 1; fi
ms=$(( (end - start) / 1000000 ))
# 1000005 instructions per run.
per=$(echo "$ms $n" | awk '{ printf "%.0f", ($1 * 1000000) / $2 }')
echo "perf-check: $n instructions in ${ms}ms = ${per} ns/instruction"
# About 90 ns/instruction on this host. Anything past 200 means per-instruction work
# has crept back into the harness.
if [ "$per" -gt 200 ]; then
    echo "perf-check: slower than 200 ns/instruction" >&2
    exit 1
fi

# The synthetic loop above measures dispatch, and dispatch stopped being the cost several rounds
# ago: a front-door test now catches the common opcodes in about two comparisons per instruction.
# This second measurement is the one that matters - a real workload, whose cost is dominated by
# memory rather than by branching. It reports nanoseconds per instruction, which is the number to
# watch if this ever regresses.
if [ -x "${E2_PERF_PYTHON:-/tmp/ftest/usr/bin/python3.12}" ]; then
    py=${E2_PERF_PYTHON:-/tmp/ftest/usr/bin/python3.12}
    root=${E2_PERF_ROOT:-/tmp/ftest}
    # Five runs, best one reported. The instruction count is deterministic, so the spread between
    # runs is the machine's, not the code's - and the minimum is the estimator that shrugs it off.
    # Single samples varied between 75 and 81 ns (8%); the best of five lands in 74-77 (4%), which
    # is the resolution any further change has to beat.
    best_ms=999999
    insns=""
    attempt=0
    while [ "$attempt" -lt 5 ]; do
        attempt=$((attempt + 1))
        start=$(date +%s%N)
        n=$(E2_ROOT="$root" PYTHONHOME=/usr E2_CAP=400000000 LANG=C.UTF-8 \
            ./tools/rune "$py" -c pass 2>&1 | sed -n 's/.*instructions=\([0-9]*\).*/\1/p')
        end=$(date +%s%N)
        if [ -n "$n" ]; then
            ms=$(( (end - start) / 1000000 ))
            insns="$n"
            [ "$ms" -lt "$best_ms" ] && best_ms=$ms
        fi
    done
    # Measured noise floor: repeated runs of the same binary vary between 75 and 81 ns for the
    # real workload, about seven percent. A change smaller than that cannot be told from noise by
    # this script, and three separate optimisations this session were within it - including a
    # four-entry region cache that read as 75-76 against 73 and was reverted.
    if [ -n "$insns" ] && [ "$insns" -gt 0 ]; then
        printf 'perf-check: real workload %s instructions in %s ms (best of 5) = %s ns/instruction\n' \
            "$insns" "$best_ms" "$(( best_ms * 1000000 / insns ))"
    fi
else
    echo "perf-check: no CPython root for the real-workload measurement"
fi
