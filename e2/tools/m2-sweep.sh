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

while IFS= read -r cmd; do
    [ -z "$cmd" ] && continue
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
