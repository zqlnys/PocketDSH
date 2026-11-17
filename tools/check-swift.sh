#!/bin/sh
# Static checks for a checkout where Swift cannot be compiled. Not a substitute for
# building, but they catch the mistakes that actually happen here: an edit that leaves
# braces unbalanced, a tool name used but never declared, and a test constructing a model
# with a label the model does not have.
set -u
root=${1:-.}
fail=0

echo "== brace balance vs HEAD =="
python3 "$(dirname "$0")/check_swift_impl.py" "$root" || fail=1

[ "$fail" -eq 0 ] && echo "check-swift: clean" || echo "check-swift: problems above"
exit "$fail"
