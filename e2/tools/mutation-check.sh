#!/bin/sh
# Verify that a test can actually catch a regression.
#
# Usage: mutation-check.sh <file> <literal-to-find> <replacement> [test-command]
#
# Applies the change, confirms it took effect, runs the test command, then restores
# the file. Reports one of:
#
#   CAUGHT      the edit compiled and the test failed - the test is doing its job
#   NOT CAUGHT  the edit compiled and the test passed - the test is too weak
#   INVALID     the edit did not compile, or did not change the file
#
# INVALID exists because of a mistake made twice: an edit that failed to compile made
# the test output contain no result line, which was read as a pass. A mutation that
# never runs proves nothing, and saying so is the whole point of this script.
set -u
file=${1:?file}
find_text=${2:?text to find}
replace_text=${3:?replacement}
test_cmd=${4:-make\ test}

[ -f "$file" ] || { echo "mutation-check: no such file: $file"; exit 2; }

backup=$(mktemp) || exit 2
cp "$file" "$backup" || exit 2
trap 'cp "$backup" "$file"; rm -f "$backup"' EXIT INT TERM

python3 - "$file" "$find_text" "$replace_text" <<'PY' || exit 2
import sys
path, find_text, replace_text = sys.argv[1], sys.argv[2], sys.argv[3]
text = open(path, encoding="utf-8").read()
if find_text not in text:
    print("mutation-check: pattern not found - the edit would do nothing")
    sys.exit(1)
open(path, "w", encoding="utf-8").write(text.replace(find_text, replace_text, 1))
PY
[ $? -eq 0 ] || { echo "INVALID  (pattern not found)"; exit 1; }

out=$(eval "$test_cmd" 2>&1)
# The exit status is deliberately not part of the decision: make propagates it for
# reasons other than test failures, and a wrong "CAUGHT" is worse than a missed one.
# The result lines below are the only evidence that counts.

if ! printf '%s' "$out" | grep -qE 'checks|passed|same|FAIL|error'; then
    echo "INVALID  (the test produced no recognisable result - it did not run)"
    printf '%s\n' "$out" | tail -3
    exit 1
fi
# Only compiler diagnostics count as a build failure. `make: *** [target] Error 1` is
# how make reports a non-zero exit from the tests themselves, and treating that as a
# build error made this script call a working mutation INVALID.
if printf '%s' "$out" | grep -qE 'error:|cc1:'; then
    echo "INVALID  (the mutation did not compile)"
    printf '%s\n' "$out" | grep -E 'error:' | head -3
    exit 1
fi

# The patterns require a count, not just the word. A test that prints a sentence
# containing "different" - "truncate and ftruncate reach the host as different calls" -
# once made a harmless mutation look caught. The exit status is not consulted either:
# make propagates it for reasons unrelated to test failures, and a wrong CAUGHT is worse
# than a missed one.
caught=0
printf '%s' "$out" | grep -qE '^FAIL ' && caught=1
printf '%s' "$out" | grep -qE ', [1-9][0-9]* failure' && caught=1
printf '%s' "$out" | grep -qE '[1-9][0-9]* different$' && caught=1

if [ "$caught" -eq 1 ]; then
    echo "CAUGHT   (the test failed as it should)"
    printf '%s\n' "$out" | grep -E '^FAIL |, [1-9][0-9]* failure|[1-9][0-9]* different$' | head -3
    exit 0
fi

echo "NOT CAUGHT  (the test passed against a broken implementation)"
printf '%s\n' "$out" | tail -2
exit 1
