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
