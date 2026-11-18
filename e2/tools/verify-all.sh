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
echo "== decode families =="
out=$(python3 tools/check-decode-families.py /tmp/bbs/bin/busybox.static 2>&1)
printf '%s\n' "$out" | tail -1
printf '%s' "$out" | grep -qE '^[0-9]+ guards, 0 ' || fail=1
echo "== overlaps =="
out=$(make overlaps 2>&1) || fail=1
printf '%s\n' "$out" | tail -1

# The two binaries are independent, so run them concurrently and print the results in
# order afterwards. Sequential execution cost 562 seconds of a 716-second verification;
# the suites are the same work either way, just no longer one after the other.
run_one() {
    bb=$1
    case "$bb" in
        *static*) label="static" ;;
        *)        label="dynamic" ;;
    esac
    if [ ! -x "$bb" ]; then
        printf '%s\n' "$bb: MISSING - cannot run the suites against it"
        return 1
    fi
    printf '== %s: %s ==\n' "$label" "$bb"
    # Each run gets its own applet directory: the suites create and populate it, and two
    # processes doing that at the same path race.
    DT_APPLETS="/tmp/dt-applets-$label"
    DT_APPLETS=$DT_APPLETS; export DT_APPLETS
    m2=$(./tools/m2-sweep.sh "$bb" "$RUNE" 40 2>&1 | tail -1)
    printf '  %s\n' "$m2"
    case "$m2" in *" 0 failed"*) ;; *) return 1 ;; esac
    m3=$(./tools/m3-sweep.sh "$bb" "$RUNE" 40 2>&1 | tail -1)
    printf '  %s\n' "$m3"
    case "$m3" in *" 0 failed"*) ;; *) return 1 ;; esac
    dt=$(./tools/diff-test.sh "$bb" "$RUNE" "$STATIC" 2>&1 | tail -1)
    printf '  %s\n' "$dt"
    case "$dt" in *" 0 different"*) ;; *) return 1 ;; esac
    # State the counts rather than leaving the caller to parse them back out of prose.
    printf 'COUNT_%s_M2=%s\n' "$label" "${m2##*: }" | sed 's/ passed.*//'
    printf 'COUNT_%s_M3=%s\n' "$label" "${m3##*: }" | sed 's/ passed.*//'
    printf 'COUNT_%s_DIFF=%s\n' "$label" "${dt##*: }" | sed 's/ same.*//'
    return 0
}

par_out_static=$(mktemp) || exit 1
par_out_dynamic=$(mktemp) || exit 1
par_rc_static=0
par_rc_dynamic=0
( run_one "$STATIC"  > "$par_out_static"  2>&1 ) & par_pid_static=$!
( run_one "$DYNAMIC" > "$par_out_dynamic" 2>&1 ) & par_pid_dynamic=$!
wait "$par_pid_static"  || par_rc_static=1
wait "$par_pid_dynamic" || par_rc_dynamic=1
cat "$par_out_static"
cat "$par_out_dynamic"
rm -f "$par_out_static" "$par_out_dynamic"
[ "$par_rc_static" -eq 0 ] || fail=1
[ "$par_rc_dynamic" -eq 0 ] || fail=1

if false; then
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
fi

echo
if [ "$fail" -eq 0 ]; then echo "verify-all: everything passed"; else echo "verify-all: FAILURES above"; fi
exit "$fail"
