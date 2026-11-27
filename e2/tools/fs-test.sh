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
# A directory, not a read-only file. Writing to /proc/version fails on a real system, but this suite
# runs as root on the build machine, and root ignores the permission bits - so the file form passed on
# a device and failed here. EISDIR is independent of who is asking.
check "write-to-readonly" 'echo x > /proc >/dev/null 2>&1 || echo refused' refused

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

echo "cancellation"

# A long command must stop when asked, not run to the instruction ceiling. E2_CANCEL_AFTER_MS
# raises the same flag the UI raises, so what this exercises is the real mechanism.
long_out=$(E2_CANCEL_AFTER_MS=300 E2_ROOT="$ROOT" E2_WORKSPACE="$WORK" timeout 30 \
    "$RUNE" "$BUSYBOX" sh -c 'i=0; while [ $i -lt 1000000 ]; do i=$((i+1)); done; echo finished' \
    2>&1)
case "$long_out" in
    *"reason=cancelled"*) pass=$((pass + 1)) ;;
    *) fail=$((fail + 1)); printf '  FAIL cancellation did not engage: %s\n' "$long_out" ;;
esac

echo "python standard library"

# The interpreter is only useful if the library on top of it works, and a fault deep inside
# math.isclose is invisible until a script reaches it. This runs json, re, hashlib, math,
# unicodedata and the string and bytes builtins, and every line must match what qemu produces for
# the same script - the expected values below were captured from qemu, not from this backend.
if [ -x "$ROOT/usr/bin/python3.12" ]; then
    cp tools/stdlib-probe.py "$WORK/stdlib_probe.py" 2>/dev/null || true
    # The ceiling matters: the default is sixty million instructions and this script needs about
    # seventy-five, so without raising it every case fails for a reason that has nothing to do
    # with the library.
    py_out=$(E2_ROOT="$ROOT" E2_WORKSPACE="$WORK" PYTHONHOME=/usr LANG=C.UTF-8 E2_CAP=400000000 \
        timeout 120 \
        "$RUNE" "$ROOT/usr/bin/python3.12" /workspace/stdlib_probe.py 2>/dev/null)
    check_std() {
        case "$py_out" in
            *"$1"*) pass=$((pass + 1)) ;;
            *) fail=$((fail + 1)); printf '  FAIL stdlib %s\n    wanted: %s\n' "$2" "$1" ;;
        esac
    }
    check_std 'json {"a": [1, 2.5, null, true], "b": "中文"}' json
    check_std "re ['1', '22', '333']" re
    check_std 'hash e98f70c29cea5aac' hashlib
    check_std 'math 1.414213562373 3628800 12' math
    check_std 'fmod 1.5 True' isclose-fmod
    check_std 'int 18446744073709551616 -4 -1' ints
    check_std 'fmt 0003.142|    42|0xff' formatting
    check_std 'uni CJK UNIFIED IDEOGRAPH-4E2D 6' unicodedata
    check_std 'bytes 0001ff 1' bytes
else
    echo "  (no CPython in this root; skipping)"
fi

# A wider sweep: thirty standard-library modules that a real script reaches for - compression,
# number formats, structural sharing, text layout, the collections and iterators. The narrow probe
# above found FABD hiding inside math.isclose; this one exists to find the next such thing by
# breadth rather than by luck. Expected values came from qemu.
if [ -x "$ROOT/usr/bin/python3.12" ]; then
    cp tools/stdlib-wide.py "$WORK/wide_probe.py" 2>/dev/null || true
    wide_out=$(E2_ROOT="$ROOT" E2_WORKSPACE="$WORK" PYTHONHOME=/usr LANG=C.UTF-8 E2_CAP=400000000 \
        timeout 180 "$RUNE" "$ROOT/usr/bin/python3.12" /workspace/wide_probe.py 2>/dev/null)
    # Counted rather than pattern-matched: every line is either a result or an ERR, so the two
    # numbers say whether anything failed without depending on how a value happens to print.
    wide_lines=$(printf '%s' "$wide_out" | grep -cE '^[a-z0-9-]+ (ERR )?[^ ]')
    wide_errs=$(printf '%s' "$wide_out" | grep -c ' ERR ')
    if [ "$wide_errs" -eq 0 ] && [ "$wide_lines" -ge 29 ]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        printf '  FAIL wide standard library: %d lines, %d errors\n' "$wide_lines" "$wide_errs"
    fi
fi

# Which standard-library modules actually import. A packaging gap shows up here as an
# ImportError naming a shared library, which is a build problem rather than an interpreter one -
# and without this list nobody knows which modules a guest script can rely on.
if [ -x "$ROOT/usr/bin/python3.12" ]; then
    mods=$(E2_ROOT="$ROOT" E2_WORKSPACE="$WORK" PYTHONHOME=/usr LANG=C.UTF-8 E2_CAP=400000000 \
        timeout 120 "$RUNE" "$ROOT/usr/bin/python3.12" -c \
        'import importlib
for m in ("ssl", "hashlib", "bz2", "lzma", "sqlite3", "pyexpat", "decimal", "xml.etree.ElementTree", "curses", "curses.panel", "gdbm"):
    try:
        importlib.import_module(m); print(m, "ok")
    except Exception as e:
        print(m, "no")' 2>/dev/null)
    ok_count=$(printf '%s' "$mods" | grep -c ' ok$')
    if [ "$ok_count" -ge 10 ]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        printf '  FAIL only %d standard modules import\n%s\n' "$ok_count" "$mods"
    fi
    printf '  modules: %s\n' "$(printf '%s' "$mods" | tr '\n' ' ')"
fi

echo "awkward input"

# Names and contents that a path-handling layer tends to get wrong, and that a real project will
# contain sooner or later.
check "name-with-space"   'echo hi > "/workspace/a b.txt" && cat "/workspace/a b.txt"' hi
check "name-unicode"      'echo hi > "/workspace/中文.txt" && ls /workspace' "中文.txt"
check "name-quotes"       'echo hi > "/workspace/q'"'"'uote.txt" && cat "/workspace/q'"'"'uote.txt"' hi
check "name-leading-dash" 'echo hi > "/workspace/-dash.txt" && cat "/workspace/-dash.txt"' hi
check "deep-nesting"      'mkdir -p /workspace/a/b/c/d/e && echo deep > /workspace/a/b/c/d/e/f && cat /workspace/a/b/c/d/e/f' deep
check "dot-in-name"       'echo hi > /workspace/.hidden && cat /workspace/.hidden' hi

# Binary content survives a round trip byte for byte.
check "binary-roundtrip"  'printf "\377\376\000A" > /workspace/bin.dat && wc -c < /workspace/bin.dat' 4
check "binary-not-text"   'printf "\000\000\000" > /workspace/z.bin && cat /workspace/z.bin | wc -c' 3

# An empty file and an empty directory are not errors.
check "empty-file"        ': > /workspace/empty && wc -c < /workspace/empty' 0
check "empty-dir"         'mkdir -p /workspace/emptydir && ls /workspace/emptydir | wc -l' 0

# Epoll instances are released between runs and there are enough of them for one program. Eight
# slots, never freed, meant the ninth epoll_create anywhere in the app's life returned EMFILE - a
# leak the command line could not show, because each run there is a fresh process.
if [ -x "$ROOT/usr/bin/python3.12" ]; then
    ep_out=$(E2_ROOT="$ROOT" E2_WORKSPACE="$WORK" PYTHONHOME=/usr LANG=C.UTF-8 E2_CAP=400000000 \
        timeout 120 "$RUNE" "$ROOT/usr/bin/python3.12" -c \
        'import select
for i in range(12):
    try:
        e = select.epoll(); e.close()
    except OSError:
        print("exhausted at", i); break
else:
    print("all ok")' 2>/dev/null)
    case "$ep_out" in
        *"all ok"*) pass=$((pass + 1)) ;;
        *) fail=$((fail + 1)); printf '  FAIL epoll slots: %s\n' "$ep_out" ;;
    esac
fi

# A hundred forks in one process. Process slots are reused when a child is reaped, and this is the
# case that would show it if they were not - the table has thirty-two.
if [ -x "$ROOT/usr/bin/python3.12" ]; then
    fk_out=$(E2_ROOT="$ROOT" E2_WORKSPACE="$WORK" PYTHONHOME=/usr LANG=C.UTF-8 E2_CAP=400000000 \
        timeout 180 "$RUNE" "$ROOT/usr/bin/python3.12" -c \
        'import os
ok = 0
for i in range(100):
    pid = os.fork()
    if pid == 0: os._exit(0)
    if os.waitpid(pid, 0)[1] == 0: ok += 1
print("reaped", ok)' 2>/dev/null)
    case "$fk_out" in
        *"reaped 100"*) pass=$((pass + 1)) ;;
        *) fail=$((fail + 1)); printf '  FAIL fork reuse: %s\n' "$fk_out" ;;
    esac
fi

# A command that floods is stopped by the write ceiling rather than filling storage. The ceiling
# only applies to regular files - a pipe is not limited - which is why this redirects rather than
# piping, and why the first attempt to test it appeared not to work.
flood_file="$WORK/../flood-$$.out"
E2_MAX_OUTPUT_MB=1 E2_ROOT="$ROOT" E2_WORKSPACE="$WORK" timeout 60 \
    "$RUNE" "$BUSYBOX" sh -c 'cat /usr/lib/libpython3.12.so.1.0' >"$flood_file" 2>/dev/null
flood_size=$(wc -c <"$flood_file" 2>/dev/null | tr -d ' ')
rm -f "$flood_file"
if [ -n "$flood_size" ] && [ "$flood_size" -le 1100000 ]; then
    pass=$((pass + 1))
else
    fail=$((fail + 1))
    printf '  FAIL output ceiling: wrote %s bytes under a 1 MB limit\n' "${flood_size:-0}"
fi

echo "exec"

# A failed exec must fail. It used to fall back to the running image, so execve of an absent path
# re-executed the interpreter with that path as argv[0]; Python then found no such file and exited
# zero, printing nothing - including whatever the caller wrote after catching the error it should
# have received.
exec_out=$(E2_ROOT="$ROOT" E2_WORKSPACE="$WORK" PYTHONHOME=/usr LANG=C.UTF-8 E2_CAP=400000000 \
    timeout 120 "$RUNE" "$ROOT/usr/bin/python3.12" -c \
    'import os
try:
    os.execve("/usr/bin/absent", ["absent"], {})
    print("returned")
except OSError as e:
    print("errno", e.errno)
print("alive")' 2>/dev/null)
case "$exec_out" in
    *"errno 2"*"alive"*) pass=$((pass + 1)) ;;
    *) fail=$((fail + 1)); printf '  FAIL exec failure: %s\n' "$exec_out" ;;
esac

echo "select"

# select(2) was unimplemented, so Python's select.select and selectors both raised ENOSYS - and a
# program that waits on a pipe could not be written. Expected values from qemu.
sel_out=$(E2_ROOT="$ROOT" E2_WORKSPACE="$WORK" PYTHONHOME=/usr LANG=C.UTF-8 E2_CAP=400000000 \
    timeout 120 "$RUNE" "$ROOT/usr/bin/python3.12" -c \
    'import os, select, selectors
r, w = os.pipe()
os.write(w, b"x")
print("select", len(select.select([r], [], [], 0.5)[0]) == 1)
s = selectors.DefaultSelector()
s.register(r, selectors.EVENT_READ)
print("selectors", len(s.select(0.5)) == 1)
print("empty", select.select([], [], [], 0.05) == ([], [], []))' 2>/dev/null)
sel_ok=$(printf '%s' "$sel_out" | grep -c 'True')
if [ "$sel_ok" -ge 3 ]; then
    pass=$((pass + 1))
else
    fail=$((fail + 1))
    printf '  FAIL select: %s\n' "$sel_out"
fi

echo "syscall surface"

# Twenty-seven syscalls in one program, compared line by line against what qemu produces for the same
# file. This exists because claims in the capability list twice turned out to be untested: a failed
# exec reported success, and select was never implemented at all.
if [ -x "$ROOT/usr/bin/python3.12" ]; then
    cp tools/syscall-probe.py "$WORK/syscall_probe.py" 2>/dev/null || true
    probe_out=$(E2_ROOT="$ROOT" E2_WORKSPACE="$WORK" PYTHONHOME=/usr LANG=C.UTF-8 E2_CAP=400000000 \
        timeout 180 "$RUNE" "$ROOT/usr/bin/python3.12" /workspace/syscall_probe.py 2>/dev/null | head -27)
    expected=$(cat <<'PROBE_EXPECTED'
getpid True
getppid True
umask True
stat-enoent E2
unlink-enoent E2
open-enoent E2
mkdir-rmdir ok
rename ok
pipe ping
select True
select-empty True
fd-blocking True
environ True
time True
monotonic True
lseek ok
seek-read cd
symlink E17
hardlink E17
mode 0o640
listdir ['a', 'b', 'c']
walk 3
size 5
isfile True True
cwd /tmp
close ok
stdin X NameError
PROBE_EXPECTED
    )
    if [ "$probe_out" = "$expected" ]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        printf '  FAIL syscall probe differs from qemu:\n%s\n' "$(printf '%s' "$probe_out" | diff --unchanged-line-format="" --old-line-format="- %L" --new-line-format="" <(printf '%s' "$expected") 2>/dev/null | head -6)"
    fi
fi

echo "single precision"

# Converting between double and single precision. `fcvt s0, d0` writes a single-precision
# destination while reading a double-precision source, and the handler decided both ends from one
# bit - so struct.pack("<f", 1.5) produced four zero bytes.
sp_out=$(E2_ROOT="$ROOT" E2_WORKSPACE="$WORK" PYTHONHOME=/usr LANG=C.UTF-8 E2_CAP=400000000 \
    timeout 120 "$RUNE" "$ROOT/usr/bin/python3.12" -c \
    'import struct
print("pack", struct.pack("<f", 1.5).hex())
print("roundtrip", struct.unpack("<f", struct.pack("<f", 0.1))[0])
print("wide", struct.unpack("<d", struct.pack("<d", 1.5))[0])' 2>/dev/null)
case "$sp_out" in
    *"pack 0000c03f"*"roundtrip 0.10000000149011612"*"wide 1.5"*) pass=$((pass + 1)) ;;
    *) fail=$((fail + 1)); printf '  FAIL single precision: %s\n' "$sp_out" ;;
esac

echo "locale"

# Under LANG=C alone, CPython truncates its own argv: /workspace/a.py becomes /wor. LC_ALL wins for
# every category, and the guest had never been given it - the bridge set it and the harness's
# hand-built environment list did not pass it through, so the mitigation was a no-op.
cp /tmp/ftest/len.py "$WORK/len.py" 2>/dev/null || \
    printf 'import sys\nprint("arg1", len(sys.argv[1]) if len(sys.argv) > 1 else 0)\n' > "$WORK/len.py"
loc_out=$(LC_ALL=C.UTF-8 LANG=C E2_ROOT="$ROOT" E2_WORKSPACE="$WORK" PYTHONHOME=/usr \
    E2_CAP=400000000 timeout 120 \
    "$RUNE" "$ROOT/usr/bin/python3.12" /workspace/len.py abcdefghijklmnop 2>&1 | head -2)
case "$loc_out" in
    "arg1 16") pass=$((pass + 1)) ;;
    *) fail=$((fail + 1)); printf '  FAIL locale argv: %s\n' "$loc_out" ;;
esac

# And the guest really does receive LC_ALL.
env_out=$(LC_ALL=C.UTF-8 E2_ROOT="$ROOT" E2_WORKSPACE="$WORK" timeout 60 \
    "$RUNE" "$BUSYBOX" env 2>/dev/null)
case "$env_out" in
    *"LC_ALL=C.UTF-8"*) pass=$((pass + 1)) ;;
    *) fail=$((fail + 1)); printf '  FAIL LC_ALL did not reach the guest\n' ;;
esac

echo "argument size"

# The guest stack's string area was a fixed two kilobytes with no bound on the write, so a long
# command line ran past the end of it silently. It is sized from what has to go in it now, and
# something that genuinely cannot fit is refused instead of corrupting the stack.
long_script=$(python3 -c 'print("BEGIN{s=0;" + "s+=1;" * 400 + "print s}")')
long_out=$(E2_ROOT="$ROOT" E2_WORKSPACE="$WORK" timeout 120 \
    "$RUNE" "$BUSYBOX" awk "$long_script" 2>&1 | head -2)
case "$long_out" in
    "400") pass=$((pass + 1)) ;;
    *) fail=$((fail + 1)); printf '  FAIL long argument: %s\n' "$long_out" ;;
esac

# An argument too long for the exec buffer is refused rather than shortened. A truncated path is a
# path to somewhere else, and the program would be started with a string it was never given.
# Reached through subprocess, because a shell's echo is a builtin and never execs anything.
big_out=$(E2_ROOT="$ROOT" E2_WORKSPACE="$WORK" PYTHONHOME=/usr LANG=C.UTF-8 E2_CAP=400000000 \
    timeout 180 "$RUNE" "$ROOT/usr/bin/python3.12" -c \
    'import subprocess
big = "y" * 3000
try:
    subprocess.run(["/bin/busybox", "echo", big])
    print("ran")
except OSError as e:
    print("errno", e.errno)' 2>/dev/null)
case "$big_out" in
    *"errno 7"*) pass=$((pass + 1)) ;;
    *) fail=$((fail + 1)); printf '  FAIL oversized exec argument: %s\n' "$big_out" ;;
esac

# More environment variables than the exec path can carry is refused. The loop used to run to 256
# while the array held 64, so the sixty-fifth wrote past the end of it.
env_out=$(E2_ROOT="$ROOT" E2_WORKSPACE="$WORK" PYTHONHOME=/usr LANG=C.UTF-8 E2_CAP=400000000 \
    timeout 180 "$RUNE" "$ROOT/usr/bin/python3.12" -c \
    'import subprocess
env = {"V%d" % i: "x" for i in range(80)}
try:
    subprocess.run(["/bin/busybox", "env"], env=env, capture_output=True)
    print("ran")
except OSError as e:
    print("errno", e.errno)' 2>/dev/null)
case "$env_out" in
    *"errno 7"*) pass=$((pass + 1)) ;;
    *) fail=$((fail + 1)); printf '  FAIL too many env vars: %s\n' "$env_out" ;;
esac

# More arguments than the exec buffer holds is refused. The loop stopped at the array size, which is
# indistinguishable from reaching the terminator, so the rest were dropped without a word.
argc_out=$(E2_ROOT="$ROOT" E2_WORKSPACE="$WORK" PYTHONHOME=/usr LANG=C.UTF-8 E2_CAP=400000000 \
    timeout 180 "$RUNE" "$ROOT/usr/bin/python3.12" -c \
    'import subprocess
argv = ["/bin/busybox", "echo"] + ["a"] * 40
try:
    subprocess.run(argv, capture_output=True)
    print("ran")
except OSError as e:
    print("errno", e.errno)' 2>/dev/null)
case "$argc_out" in
    *"errno 7"*) pass=$((pass + 1)) ;;
    *) fail=$((fail + 1)); printf '  FAIL too many arguments: %s\n' "$argc_out" ;;
esac

echo "proc"

# /proc is seeded by the harness, and the seed used to return early when the root had no /proc
# directory - so nothing was seeded at all and every reader failed as though the files did not
# exist. The trimmed Alpine root has no /proc, which made that the normal case.
check "proc-mounts"  'cat /proc/mounts | head -1' '/dev/root / ext4 rw,relatime 0 0'
check "proc-version" 'cat /proc/version' 'Linux version 6.1.0-e2 (e2@localhost) #1 SMP aarch64'
check "df-root"      'df / | tail -1 | wc -l' 1
# Through `busybox` rather than a bare name: the trimmed guest root links only some applets, so
# `free` on PATH is "not found" while `busybox free` works.
check "free-total"   'busybox free' '       1024000      377904      512000           0      134096      768000'
# /proc/self is synthesised per request; the fd directory is a real one the guest lists. Both used to
# be built under the root, which is read-only on a device.
check "proc-self-cmdline" 'cat /proc/self/cmdline' '/bin/cat/proc/self/cmdline'
check "proc-self-fd"      'ls /proc/self/fd | wc -l' '4'
# The status body is written to a scratch file before the guest reads it. That file used to live under
# the root, which is read-only on a device - so /proc/self/status worked here and failed there.
check "proc-self-status"  'head -1 /proc/self/status' 'Name:	busybox'

# Every seeded file, in one line. The seed lists them and any one of them going missing is a reader
# failing for no stated reason - which is how /proc/version spent rounds containing a stray "x".
check "proc-all-seeded" \
    'for f in meminfo uptime version stat cpuinfo filesystems mounts; do test -r /proc/$f || echo $f; done | wc -l' \
    0

printf 'fs test: %d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
