#!/bin/sh
# Runs a spread of busybox applets through the interpreter and reports which
# ones survive. M2 claims "busybox basic commands work"; this is how that claim
# is checked rather than remembered.
#
#   tools/m2-sweep.sh /path/to/busybox [tools/rune] [seconds-per-applet]
#
# Each applet runs with stdin from /dev/null: several of them (`sort`, `tr`,
# `wc`) read stdin and will otherwise swallow the rest of the list. Each also
# gets a timeout - without one a single applet that spins hangs the whole sweep,
# which is exactly what happened the first time this was written.
set -u
BUSYBOX=${1:?usage: m2-sweep.sh BUSYBOX [RUNE]}
RUNE=${2:-./tools/rune}
LIMIT=${3:-60}

pass=0
fail=0
failed=""

# A small tree reused by the recursive-walk cases: building it inside each case
# spent most of the timeout on rm and mkdir rather than on the walk being tested.
rm -rf /tmp/alpine-root/tmp/m2tree 2>/dev/null || true
mkdir -p /tmp/alpine-root/tmp/m2tree/a/b 2>/dev/null || true
touch /tmp/alpine-root/tmp/m2tree/a/f1 /tmp/alpine-root/tmp/m2tree/a/b/f2 2>/dev/null || true

while IFS= read -r cmd; do
    [ -z "$cmd" ] && continue
    # A comment is not a test. Running `sh -c "# ..."` succeeds trivially, which
    # used to inflate the pass count by one for every explanatory line.
    case "$cmd" in \#*) continue ;; esac
    out=$(timeout "$LIMIT" "$RUNE" "$BUSYBOX" sh -c "$cmd" < /dev/null 2>&1)
    status=$?
    if [ "$status" -eq 124 ]; then
        fail=$((fail + 1))
        failed="$failed
  [$cmd] timed out after ${LIMIT}s"
        continue
    fi
    if echo "$out" | grep -q "FAULT\|instruction cap\|cannot load"; then
        fail=$((fail + 1))
        failed="$failed
  [$cmd] $(echo "$out" | tail -1 | cut -c1-90)"
    else
        pass=$((pass + 1))
    fi
done <<'EOF'
# /proc and statfs: free, uptime and df all failed on a missing file or a missing
# syscall rather than on anything the interpreter got wrong.
free | head -1
cat /proc/version
df / | tail -1 | awk '{print ($2>0) ? "hasblocks" : "noblocks"}'
# getpid returned a constant 0, which is the value that means "child, in a fork".
sh -c 'test "$$" -gt 0 && echo haspid'
sh -c 'echo $$ | cat' > /dev/null && echo pidstable
ulimit -n
ulimit -s
ulimit -c
printf 'abc' > /tmp/m2c; chown 0:0 /tmp/m2c && stat -c %u /tmp/m2c
printf 'a\n' > /tmp/m2c; chmod 640 /tmp/m2c && stat -c %a /tmp/m2c
mkdir -p /tmp/m2c2/a/b; mv /tmp/m2c2/a /tmp/m2c3; ls /tmp/m2c3
printf 'q\n' > /tmp/m2c4; touch -d '2020-01-01' /tmp/m2c4; date -r /tmp/m2c4 +%Y
# truncate and ftruncate had no implementation at all, so `truncate -s` said
# "Function not implemented".
printf 'abc' > /tmp/m2t; truncate -s 1 /tmp/m2t; wc -c < /tmp/m2t
printf 'abcdef' > /tmp/m2t; truncate -s 0 /tmp/m2t; wc -c < /tmp/m2t
printf 'abc' > /tmp/m2t; truncate -s 10 /tmp/m2t; wc -c < /tmp/m2t
# awk maths: the primitives all agree with qemu, so they pin the layer beneath pow.
printf 'BEGIN{print log(2)}\n' > /tmp/m2a; awk -f /tmp/m2a
printf 'BEGIN{print exp(log(2)*10)}\n' > /tmp/m2a; awk -f /tmp/m2a
printf 'BEGIN{print sqrt(2)}\n' > /tmp/m2a; awk -f /tmp/m2a
printf 'BEGIN{print atan2(1,1)}\n' > /tmp/m2a; awk -f /tmp/m2a
printf 'BEGIN{printf "%.3f\\n", 1/3}\n' > /tmp/m2a; awk -f /tmp/m2a
printf 'BEGIN{printf "%d %d\\n", 3.9, -3.9}\n' > /tmp/m2a; awk -f /tmp/m2a
printf 'BEGIN{printf "%x %o\\n", 255, 8}\n' > /tmp/m2a; awk -f /tmp/m2a
printf 'BEGIN{print substr("hello",2,3) index("hello","ll")}\n' > /tmp/m2a; awk -f /tmp/m2a
printf 'BEGIN{n=split("a:b:c",arr,":"); print n}\n' > /tmp/m2a; awk -f /tmp/m2a
expr 7 % 3
expr length hello
printf '%05.2f\n' 1.5
echo abc | rev
echo abc | fold -w2
echo test | base64
echo dGVzdAo= | base64 -d
printf 'ab\n' | od -An -tu1
echo abc | md5sum
printf 'a\tb\n' | expand
printf 'a\n' | xargs -I{} echo got{}
echo a | sed 'y/a/b/'
echo abc | cut -b2
printf '1\n2\n3\n' > /tmp/m2s; seq 5 > /tmp/m2s; tac /tmp/m2s
echo abc | grep -o b
printf 'a\nb\n' > /tmp/m2s; nl -w2 -s: /tmp/m2s
seq 3 | paste -d, - - -
printf '1\n2\n' > /tmp/m2s; split -l1 /tmp/m2s /tmp/m2sp; ls /tmp/m2sp* | wc -l
date +%Y
date -u +%H
find /tmp/m2tree -name f1
find /tmp/m2tree | sort
ls -R /tmp/m2tree
find /tmp/m2tree -type f
find /tmp/m2tree -type d | wc -l
rm -rf /tmp/m2d; mkdir -p /tmp/m2d; mkdir /tmp/m2d/d && ls /tmp/m2d
rm -rf /tmp/m2d; mkdir -p /tmp/m2d; echo hi > /tmp/m2d/a; cp /tmp/m2d/a /tmp/m2d/b && cat /tmp/m2d/b
rm -rf /tmp/m2d; mkdir -p /tmp/m2d; echo x > /tmp/m2d/a; mv /tmp/m2d/a /tmp/m2d/c && ls /tmp/m2d
rm -rf /tmp/m2d; mkdir -p /tmp/m2d; ln -s target /tmp/m2d/l && readlink /tmp/m2d/l
rm -rf /tmp/m2d; mkdir -p /tmp/m2d; touch /tmp/m2d/t; chmod 700 /tmp/m2d/t && ls -l /tmp/m2d/t
rm -rf /tmp/m2d; mkdir -p /tmp/m2d; touch /tmp/m2d/t && rm /tmp/m2d/t && ls /tmp/m2d | wc -l
printf 'x1\ny2\nz3\n' > /tmp/m2t; od -c /tmp/m2t
printf 'x1\ny2\nz3\n' > /tmp/m2t; xxd /tmp/m2t
printf 'x1\ny2\nz3\n' > /tmp/m2t; cut -c1 /tmp/m2t
printf 'x1\ny2\nz3\n' > /tmp/m2t; tr -d 1 < /tmp/m2t
printf 'x1\ny2\nz3\n' > /tmp/m2t; paste -s /tmp/m2t
printf 'x1\ny2\nz3\n' > /tmp/m2t; sort -r /tmp/m2t
printf 'x1\ny2\nz3\n' > /tmp/m2t; sort -u /tmp/m2t
printf 'x1\ny2\nz3\n' > /tmp/m2t; wc -w /tmp/m2t
printf 'x1\ny2\nz3\n' > /tmp/m2t; grep -E '[xy]' /tmp/m2t
printf 'a\nb\na\n' > /tmp/m2in; uniq /tmp/m2in
printf 'a\nb\na\n' > /tmp/m2in; sort /tmp/m2in
printf 'a\nb\na\n' > /tmp/m2in; comm /tmp/m2in /tmp/m2in
printf 'a\nb\na\n' > /tmp/m2in; tac /tmp/m2in
printf 'a\nb\na\n' > /tmp/m2in; nl /tmp/m2in
printf 'a\nb\na\n' > /tmp/m2in; fold /tmp/m2in
printf 'a\nb\na\n' > /tmp/m2in; paste /tmp/m2in
printf 'a\nb\na\n' > /tmp/m2in; base64 /tmp/m2in
printf 'a\nb\na\n' > /tmp/m2in; md5sum /tmp/m2in
printf 'a\nb\na\n' > /tmp/m2in; cksum /tmp/m2in
printf 'a\nb\na\n' > /tmp/m2in; rev /tmp/m2in
echo abc | tr a-z A-Z
ls
ls -l
ls -a
echo hi
printf %s\n hi
cat /dev/null
wc -l /dev/null
wc -c /dev/null
true
false
sleep 0
pwd
basename /a/b.c
dirname /a/b.c
seq 3
seq 1 5
seq 10 -2 1
expr 1 + 2
expr 6 \* 7
test 1 = 1
touch /tmp/t1
mkdir -p /tmp/d1/d2
ls /tmp/d1
rm -f /tmp/t1
sort
sort -n
head -1
tail -1
cut -d: -f1
tr a-z A-Z
env
id
date
printf %d 42
printf %05d 42
printf %x 255
printf '%.2f\n' 3.14159
printf '%e\n' 1234.5
printf '%g\n' 0.000123
printf %o 8
printf %X 255
printf '%-5s|' ab
printf '%+d\n' 5
printf '%c\n' 65
echo -e a\tb
seq -s, 1 3
base32 /dev/null
sha512sum /dev/null
nproc
sort -u /dev/null
cut -d: -f1 /etc/passwd
rev /dev/null
od -An -tu1 /dev/null
md5sum /dev/null
sha256sum /dev/null
base64 /dev/null
du -s /usr/lib
df -h
stat /tmp
readlink -f /usr
sed -n 1p
awk '{print $1}'
find /tmp -maxdepth 1 -name t*
grep -c x /dev/null
EOF
echo "M2 sweep: $pass passed, $fail failed"
if [ "$fail" -ne 0 ]; then
    printf 'failures:%s\n' "$failed"
    exit 1
fi
