#!/bin/sh
# One entry point for everything that can be checked in this checkout.
#
#   e2/     the aarch64 backend: unit tests, three sweeps, two binaries
#   swift   the iOS sources, which cannot be compiled here - static checks only
#
# Exit status is non-zero if any part is not clean.
set -u
here=$(dirname "$0")/..
fail=0

echo "=== E2 backend ==="
if [ -x "$here/e2/tools/rune" ]; then
    # Capture the output so the counts can be handed to the documentation check below
    # instead of it running every suite again.
    e2out=$( cd "$here/e2" && ./tools/verify-all.sh 2>&1 )
    rc=$?
    printf '%s\n' "$e2out"
    [ "$rc" -eq 0 ] || fail=1
    UNIT_COUNT=$(printf '%s\n' "$e2out" | sed -n 's/^\([0-9]\+\) checks.*/\1/p' | head -1)
    M2_COUNT=$(printf '%s\n' "$e2out" | sed -n 's/^  M2 sweep: \([0-9]\+\) passed.*/\1/p' | head -1)
    M3_COUNT=$(printf '%s\n' "$e2out" | sed -n 's/^  M3 sweep: \([0-9]\+\) passed.*/\1/p' | head -1)
    DIFF_COUNT=$(printf '%s\n' "$e2out" | sed -n 's/^  diff test: \([0-9]\+\) same.*/\1/p' | head -1)
else
    echo "e2/tools/rune missing - build it first"
    fail=1
    UNIT_COUNT= M2_COUNT= M3_COUNT= DIFF_COUNT=
fi

echo
echo "=== iOS static checks ==="
swiftout=$(sh "$here/tools/check-swift.sh" "$here" 2>&1)
printf '%s\n' "$swiftout"
printf '%s\n' "$swiftout" | grep -q "check-swift: clean" || fail=1
STATIC_COUNT=$(printf '%s\n' "$swiftout" | grep -c '^  ')

echo
echo "=== documentation claims ==="
# Pass the counts just measured so the check does not run every suite a second time.
# Standalone use of check-docs.sh still falls back to running them.
DSH_COUNT_UNIT=$UNIT_COUNT DSH_COUNT_M2=$M2_COUNT DSH_COUNT_M3=$M3_COUNT \
DSH_COUNT_DIFF=$DIFF_COUNT DSH_COUNT_STATIC=$STATIC_COUNT \
    sh "$here/tools/check-docs.sh" || fail=1

echo
if [ "$fail" -eq 0 ]; then
    echo "verify: everything passed"
else
    echo "verify: FAILURES above"
fi
exit "$fail"
