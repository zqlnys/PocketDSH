#!/bin/sh
# Runs every suite against both busybox builds. They take different paths through
# the backend - the static one never touches the dynamic loader - so a green run on
# both says considerably more than a green run on either.
set -u
RUNE=${1:-./tools/rune}
STATIC=${2:-/tmp/bbs/bin/busybox.static}
DYNAMIC=${3:-/tmp/alpine-root/bin/busybox}
fail=0

echo "== unit tests =="
make test 2>&1 | tail -1 || { echo "  FAIL: $?"; fail=1; }
echo "== overlaps =="
make overlaps 2>&1 | tail -1 || { echo "  FAIL: $?"; fail=1; }

for bb in "$STATIC" "$DYNAMIC"; do
    [ -x "$bb" ] || { echo "$bb: missing, skipped"; continue; }
    case "$bb" in
        *static*) label="static" ;;
        *)        label="dynamic" ;;
    esac
    echo "== $label: $bb =="
    out=$(./tools/m2-sweep.sh "$bb" "$RUNE" 40 2>&1 | tail -1)
    echo "  $out"
    case "$out" in *" 0 failed"*) ;; *) echo "  FAIL on: $out"; fail=1 ;; esac
    out=$(./tools/m3-sweep.sh "$bb" "$RUNE" 40 2>&1 | tail -1)
    echo "  $out"
    case "$out" in *" 0 failed"*) ;; *) echo "  FAIL on: $out"; fail=1 ;; esac
    out=$(./tools/diff-test.sh "$bb" "$RUNE" "$STATIC" 2>&1 | tail -1)
    echo "  $out"
    case "$out" in *" 0 different"*) ;; *) echo "  FAIL on: $out"; fail=1 ;; esac
done

echo
if [ "$fail" -eq 0 ]; then echo "verify-all: everything passed"; else echo "verify-all: FAILURES above"; fi
exit "$fail"
