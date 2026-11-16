#!/bin/sh
# Runs a spread of shell constructs and reports which survive. M3 claims
# pipelines, redirection and scripts work; this is how that claim is checked.
#
#   tools/m3-sweep.sh /path/to/busybox [tools/rune] [seconds-per-case]
set -u
BUSYBOX=${1:?usage: m3-sweep.sh BUSYBOX [RUNE] [LIMIT]}
RUNE=${2:-./tools/rune}
LIMIT=${3:-40}

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
  [$cmd] timed out - likely a scheduling deadlock"
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
echo a | cat
echo a | cat | cat
printf x\n | wc -l
echo hi > /tmp/m3a; cat /tmp/m3a
echo a > /tmp/m3b; echo b >> /tmp/m3b; wc -l < /tmp/m3b
X=5; echo $X
X=5; Y=6; echo $((X+Y))
echo $(echo nested)
X=$(echo v); echo $X
echo `echo bt`
echo $((2*3))
false || echo fallback
true && echo yes
if true; then echo t; fi
if false; then echo t; else echo f; fi
for i in 1 2 3; do echo $i; done
i=0; while [ $i -lt 3 ]; do echo $i; i=$((i+1)); done
echo a; echo $?
false; echo $?
case x in x) echo match;; esac
f() { echo fn; }; f
echo one two three | cut -d' ' -f2
ls / | head -1
cat /dev/null | wc -c
export V=1; echo $V
echo ${PATH%%:*}
echo hi | tr h H
seq 3 | wc -l
(echo sub)
{ echo grp; }
echo a b c | while read x y z; do echo $y; done
EOF

echo "M3 sweep: $pass passed, $fail failed"
if [ "$fail" -ne 0 ]; then
    printf 'failures:%s\n' "$failed"
    exit 1
fi
