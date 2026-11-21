#!/bin/sh
# Run CPython 3.12 under the E2 interpreter.
#
#   tools/run-python.sh script.py [args...]
#   tools/run-python.sh -c 'print(1)'
#
# Three settings are needed and none of them is obvious, so they live here rather than in
# somebody's shell history:
#
#   PYTHONHOME=/usr    getpath otherwise walks its landmarks and fails before any Python runs.
#   E2_HEAP_MB=256     CPython's allocator needs far more than the 16 MB default, which is
#                      sized for busybox. Raising the default instead made every fork carry
#                      the whole heap and turned a 3-second case into 40 seconds.
#   E2_CAP=...         importing the standard library runs tens of millions of instructions.
#
# Scripts work; `-c` is known to corrupt its source above eight characters, so prefer files.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=${E2_ROOT:-/tmp/alpine-root}
rune=${RUNE:-$here/rune}
py=$root/usr/bin/python3.12

if [ ! -x "$rune" ]; then echo "run-python: $rune missing - run make harness" >&2; exit 1; fi
if [ ! -f "$py" ]; then echo "run-python: $py missing" >&2; exit 1; fi

exec env PYTHONHOME=/usr E2_HEAP_MB=${E2_HEAP_MB:-256} E2_CAP=${E2_CAP:-300000000} \
    "$rune" "$py" "$@"
