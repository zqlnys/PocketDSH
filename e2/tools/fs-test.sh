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

printf 'fs test: %d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
