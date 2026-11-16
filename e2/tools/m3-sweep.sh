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
(exit 3); echo $?
sh -c "exit 3"; echo $?
/bin/false; echo $?
true; echo $?
trap "echo bye" EXIT; echo hi
printf 'p q\nr s\n' > /tmp/m3r; while read x y; do echo $y-$x; done < /tmp/m3r
echo a b | while read x y; do echo $y $x; done
echo a | read x; echo got $x
printf 'a b\nc d\n' > /tmp/m3r; awk '{print $2, $1}' /tmp/m3r
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
echo "a b" | cut -d' ' -f1
x="q w"; echo $x
echo 'single $x'
cat <<EOFX
heredoc line
EOFX
echo a | { read v; echo $v; }
f() { local x=1; echo $x; }; f
f() { return 3; }; f; echo $?
echo {1..3}
echo /tm?
echo $((7/2))
echo $((-5+2))
test -f /etc/passwd && echo yes
test -d /etc && echo dir
i=0; until [ $i -ge 2 ]; do echo $i; i=$((i+1)); done
x=abc; echo ${#x}
x=abc; echo ${x%c}
x=abc; echo ${x#a}
(exit 5); echo $?
echo hi | tee /tmp/m3o | wc -l
printf 'aaa\n' > /tmp/m3in; grep a /tmp/m3in
printf 'aaa\n' > /tmp/m3in; grep -F a /tmp/m3in
printf 'aaa\n' > /tmp/m3in; sed s/a/A/ /tmp/m3in
yes | head -3
seq 1000 | head -2
seq 5 | tail -1
cat /tmp/m3in | wc -l
printf 'a 1 10\nb 2 20\na 3 30\n' > /tmp/m3aw; awk '{print $1}' /tmp/m3aw
printf 'a 1 10\nb 2 20\na 3 30\n' > /tmp/m3aw; awk 'END{print NR}' /tmp/m3aw
printf 'a 1 10\nb 2 20\na 3 30\n' > /tmp/m3aw; awk '$1 == "a" {print $2}' /tmp/m3aw
printf 'a 1 10\nb 2 20\na 3 30\n' > /tmp/m3aw; awk '{s += $3} END{print s}' /tmp/m3aw
printf 'a 1 10\nb 2 20\na 3 30\n' > /tmp/m3aw; awk 'BEGIN{print 1+2}'
printf 'aaa\nbbb\naaa\n' > /tmp/m3in; sed -n '2p' /tmp/m3in
printf 'aaa\nbbb\naaa\n' > /tmp/m3in; sed s/a/X/g /tmp/m3in
printf 'aaa\nbbb\naaa\n' > /tmp/m3in; grep -c a /tmp/m3in
printf 'aaa\nbbb\naaa\n' > /tmp/m3in; grep -v a /tmp/m3in
printf 'aaa\nbbb\naaa\n' > /tmp/m3in; grep -o b /tmp/m3in
expr abc : 'a.c'
EOF

# The BRE engine is fixed, so the sweep no longer excuses grep, sed or awk.
# Anything that fails now is a real failure.
echo "M3 sweep: $pass passed, $fail failed"
if [ "$fail" -ne 0 ]; then
    printf 'failures:%s\n' "$failed"
    exit 1
fi
