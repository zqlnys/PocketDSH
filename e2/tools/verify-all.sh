#!/bin/sh
# Runs every suite against both busybox builds. They take different paths through
# the backend - the static one never touches the dynamic loader - so a green run on
# both says considerably more than a green run on either.
set -u
RUNE=${1:-./tools/rune}
STATIC=${2:-/tmp/bbs/bin/busybox.static}
DYNAMIC=${3:-/tmp/alpine-root/bin/busybox}
fail=0

# The exit status of a pipeline is the last command's, so `make test | tail -1 || fail=1`
# could never fail: tail always succeeds. Capture the output and the status separately.
echo "== unit tests =="
out=$(make test 2>&1) || fail=1
printf '%s\n' "$out" | tail -1
printf '%s' "$out" | grep -qE ', [1-9][0-9]* failure' && fail=1
echo "== overlaps =="
out=$(make overlaps 2>&1) || fail=1
printf '%s\n' "$out" | tail -1

for bb in "$STATIC" "$DYNAMIC"; do
    # A skipped binary is not a passed binary: the claim is that both work, so a
    # missing one has to fail rather than quietly halve the coverage.
    if [ ! -x "$bb" ]; then
        echo "$bb: MISSING - cannot run the suites against it"
        fail=1
        continue
    fi
    case "$bb" in
        *static*) label="static" ;;
        *)        label="dynamic" ;;
    esac
    echo "== $label: $bb =="
    out=$(./tools/m2-sweep.sh "$bb" "$RUNE" 40 2>&1 | tail -1)
    echo "  $out"
    case "$out" in *" 0 failed"*) ;; *) fail=1 ;; esac
    out=$(./tools/m3-sweep.sh "$bb" "$RUNE" 40 2>&1 | tail -1)
    echo "  $out"
    case "$out" in *" 0 failed"*) ;; *) fail=1 ;; esac
    out=$(./tools/diff-test.sh "$bb" "$RUNE" "$STATIC" 2>&1 | tail -1)
    echo "  $out"
    case "$out" in *" 0 different"*) ;; *) fail=1 ;; esac
done

echo
if [ "$fail" -eq 0 ]; then echo "verify-all: everything passed"; else echo "verify-all: FAILURES above"; fi
exit "$fail"
