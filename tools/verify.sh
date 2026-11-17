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
    ( cd "$here/e2" && ./tools/verify-all.sh ) || fail=1
else
    echo "e2/tools/rune missing - build it first"
    fail=1
fi

echo
echo "=== iOS static checks ==="
sh "$here/tools/check-swift.sh" "$here" || fail=1

echo
echo "=== documentation claims ==="
sh "$here/tools/check-docs.sh" || fail=1

echo
if [ "$fail" -eq 0 ]; then
    echo "verify: everything passed"
else
    echo "verify: FAILURES above"
fi
exit "$fail"
