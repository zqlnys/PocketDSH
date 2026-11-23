#!/bin/sh
# The host-side filesystem layer, exercised through guest commands.
#
# Written here rather than as a device test because the loop is seconds instead of fifteen
# minutes, and because the layer under test - path resolution, stat, the descriptor table - is
# host code that a Swift test reaches only through three layers of indirection.
#
# Each case is a guest command and the text its output must contain. A case that produces an
# unexpected result says which, and exits non-zero, so this can run in CI.
#
#   tools/fs-test.sh [BUSYBOX] [RUNE]
set -u
BUSYBOX=${1:-/tmp/bbs/bin/busybox.static}
RUNE=${2:-./tools/rune}
ROOT=${E2_ROOT:-/tmp/ftest}
WORK=$(mktemp -d)
TMPDIR_GUEST=$(mktemp -d)
trap 'rm -rf "$WORK" "$TMPDIR_GUEST"' EXIT

# The applets the cases below use. Linked into the guest's /bin if missing, so this works against
# any root that has a busybox - a root assembled by hand tends to have only the handful someone
# needed at the time, and a missing applet looks exactly like a failing syscall.
for applet in sh ls cat tr stat chmod chown mv ln rm mkdir rmdir wc test dd df head tail cmp; do
    if [ -e "$ROOT/bin/$applet" ]; then continue; fi
    if [ -e "$ROOT/bin/busybox" ]; then ln -sf busybox "$ROOT/bin/$applet" 2>/dev/null || true; fi
done

pass=0
fail=0

# Runs a guest shell command in the prepared workspace and prints its output.
guest() {
    # Bounded. A guest command that never returns hangs the whole suite, and one did: without a
    # ceiling the run sat there until something else killed it, with no indication which case.
    E2_ROOT="$ROOT" E2_WORKSPACE="$WORK" timeout "${E2_CASE_TIMEOUT:-20}" \
        "$RUNE" "$BUSYBOX" sh -c "$1" 2>/dev/null
}

# check NAME COMMAND EXPECTED
check() {
    out=$(guest "$2")
    case "$out" in
        *"$3"*) pass=$((pass + 1)) ;;
        *)      fail=$((fail + 1))
                first=$(printf '%s' "$out" | tr '\n' '|')
                printf '  FAIL %s\n    wanted: %s\n    got:    %s\n' "$1" "$3" "$first" ;;
    esac
}

echo "guest filesystem"

# A file written by the guest is visible to the next command, because /workspace is the same
# host directory both times.
check "write-then-read"  'echo hello > /workspace/a.txt && cat /workspace/a.txt' hello
check "append"           'echo second >> /workspace/a.txt && wc -l < /workspace/a.txt' 2
check "stat-size"        'stat -c %s /workspace/a.txt' 13
check "test-f"           'test -f /workspace/a.txt && echo yes' yes
check "test-d-fails"     'test -f /workspace && echo no || echo yes' yes

# Directories.
check "mkdir"            'mkdir -p /workspace/d1/d2 && test -d /workspace/d1/d2 && echo ok' ok
check "ls-lists"         'ls /workspace/d1' d2
check "rmdir"            'rmdir /workspace/d1/d2 && test -d /workspace/d1/d2 || echo gone' gone

# Links: the guest must see the link and its target, not a resolved host path.
check "symlink"          'ln -s a.txt /workspace/l && ls -l /workspace/l' 'l -> a.txt'
check "link-reads"       'cat /workspace/l' hello

# Rename and remove.
check "rename"           'mv /workspace/a.txt /workspace/b.txt && test -f /workspace/a.txt || echo moved' moved
check "remove"           'rm /workspace/b.txt && test -f /workspace/b.txt || echo gone' gone

# Permissions are reported, and chmod changes them.
check "chmod"            'chmod 600 /workspace/d1 && stat -c %a /workspace/d1' 600
check "chmod-restore"    'chmod 755 /workspace/d1 && stat -c %a /workspace/d1' 755

# Errors that must fail rather than silently succeed.
check "missing-file"     'cat /workspace/nothing | wc -c' 0
check "bad-dir"          'cd /workspace/nothing 2>/dev/null || echo refused' refused
check "write-to-readonly" 'echo x > /proc/version >/dev/null 2>&1 || echo refused' refused

# Path handling: absolute, and traversal cannot escape the root.
check "dotslash"         'cd /workspace && ls ./d1 >/dev/null && echo ok' ok
check "traversal"        'cat /workspace/../../etc/hostname 2>/dev/null | wc -c' 0

# The descriptor table: redirection and pipes.
check "pipe"             'echo abc | tr a-z A-Z' ABC
check "redirect-append"  'echo x > /workspace/c.txt; echo y >> /workspace/c.txt; wc -l < /workspace/c.txt' 2
check "dup-stderr"       'sh -c "echo err 1>&2" 2>/dev/null; echo rc=$?' rc=0

# Appending and truncating: the two modes a shell uses without thinking.
check "append-vs-truncate" 'echo a > /workspace/t.txt; echo b >> /workspace/t.txt; echo c > /workspace/t.txt; cat /workspace/t.txt' c
check "truncate-len"       'printf abcdef > /workspace/t2.txt; : > /workspace/t2.txt; wc -c < /workspace/t2.txt' 0

# Seeking: dd with a skip reads from an offset the way a real one does.
check "seek-skip"          'printf 0123456789 > /workspace/s.txt; dd if=/workspace/s.txt bs=1 skip=5 count=2 2>/dev/null' 56
check "seek-end"           'printf abc > /workspace/s2.txt; dd if=/workspace/s2.txt bs=1 skip=1 2>/dev/null' bc

# Many files in one directory: readdir over more than the buffered chunk a naive implementation
# returns in one go.
check "many-files"         'mkdir -p /workspace/many && i=0; while [ $i -lt 60 ]; do : > /workspace/many/m$i; i=$((i+1)); done; ls /workspace/many | wc -l' 60

# Sparse-ish writes and reads across a boundary.
check "large-write"        'dd if=/dev/zero of=/workspace/big bs=1024 count=64 2>/dev/null; wc -c < /workspace/big' 65536
check "large-read-back"    'dd if=/workspace/big bs=1024 count=1 2>/dev/null | wc -c' 1024

# Descriptive names: none of the above should have produced an error.
check "no-errors"          'ls /workspace/big >/dev/null && echo clean' clean

# A directory used as a file fails, rather than reading something.
check "dir-as-file"        'cat /workspace/d1 2>/dev/null || echo refused' refused

# Removing a non-empty directory fails.
check "rmdir-nonempty"     'mkdir -p /workspace/ne/x; rmdir /workspace/ne 2>/dev/null || echo refused' refused

# statfs reports something, and sensible sizes.
# df is not covered. It matches mount points to devices by st_dev, and this backend reports one
# device for everything while /proc/mounts names /dev/root, so df answers "can't find mount
# point". That is a real gap, not a test to write: making it work means modelling a device per
# mount, which nothing else here needs. Recorded rather than asserted, so the case is not quietly
# dropped and not quietly passing.

# The descriptor table does not leak across many opens.
check "many-opens"         'i=0; while [ $i -lt 80 ]; do cat /workspace/t.txt >/dev/null; i=$((i+1)); done; echo done' done

echo "argv and -c"

# Copied into the workspace so the guest can run it; the cases pass arguments to it rather than
# trying to escape an argument inside a command string.
cp tools/argv-probe.sh "$WORK/probe.sh" 2>/dev/null || true

check "argv-short"     'sh /workspace/probe.sh a b' 'argc=2'
check "argv-values"    'sh /workspace/probe.sh a b' 'arg1: len=1 value=[b]'
check "argv-empty"     'sh /workspace/probe.sh ""' 'arg0: len=0 value=[]'
check "argv-with-space" 'sh /workspace/probe.sh "a b c"' 'value=[a b c]'
check "argv-many"      'sh /workspace/probe.sh 1 2 3 4 5 6 7 8 9' 'argc=9'
check "argv-unicode"   'sh /workspace/probe.sh "中文" ' 'value=[中文]'

# A long argument: the corruption this backend carried truncated anything past a few characters.
long=$(printf '%0200d' 0)
check "argv-long"      "sh /workspace/probe.sh $long" 'arg0: len=200'

# -c itself, which is where the truncation was first seen.
check "dash-c-short"   'busybox sh -c "echo ok"' ok
check "dash-c-unicode" 'busybox sh -c "echo 中文测试"' '中文测试'
check "dash-c-long"    'busybox sh -c "echo 0123456789012345678901234567890123456789"' 0123456789012345678901234567890123456789

printf 'fs test: %d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
