#!/bin/sh
# Run the same command through this implementation and through qemu, and report
# any difference in what the program produced.
#
# This is the cheapest way to find where the two disagree: an identical result
# over many commands is evidence that a whole subsystem agrees, and a difference
# points at exactly one place to look.
#
# Only read-only commands are used. jqemu is not chrooted, so anything that
# writes a file would have the two implementations see different contents and
# produce a meaningless comparison.
#
#   tools/diff-test.sh [BUSYBOX] [rune]
set -u
BUSYBOX=${1:-/tmp/bbs/bin/busybox.static}
RUNE=${2:-./tools/rune}
ROOT=/tmp/alpine-root

# The fixture lives in both places: the guest root for this implementation, and
# the host root for qemu, which only remaps the loader path.
for f in /tmp/alpine-root/tmp /tmp; do
    printf 'a 1\nb 2\na 3\n' > "$f/dta" 2>/dev/null || true
    printf 'abc\ndef\nghi\n' > "$f/dtb" 2>/dev/null || true
    printf '  spaced  \n\ttab\n' > "$f/dtc" 2>/dev/null || true
done

cat > /tmp/dtsh 2>/dev/null <<'SCRIPT'
count=0
for f in /tmp/dta /tmp/dtb; do
    if [ -f "$f" ]; then count=$((count + 1)); fi
done
echo "files=$count"
sum=0
for i in 1 2 3 4 5; do sum=$((sum + i)); done
echo "sum=$sum"
square() { echo $(( $1 * $1 )); }
echo "sq=$(square 7)"
echo hello | tr a-z A-Z
printf '%s\n' a b c | wc -l
case x in x) echo matched;; esac
while [ "$#" -gt 0 ]; do echo "arg=$1"; shift; done
SCRIPT
cp /tmp/dtsh /tmp/alpine-root/tmp/dtsh 2>/dev/null || true
cat > /tmp/dtbig 2>/dev/null <<'BIGSCRIPT'
set -e
mkdir -p /tmp/dtwork && cd /tmp/dtwork
printf 'name,score\nalice,90\nbob,75\ncarol,88\n' > s.csv
tail -n +2 s.csv | sort -t, -k2 -n -r > sorted.txt
echo "top: $(head -1 sorted.txt | cut -d, -f1)"
total=$(tail -n +2 s.csv | cut -d, -f2 | awk '{s+=$1} END{print s}')
count=$(tail -n +2 s.csv | wc -l)
echo "avg: $((total / count))"
grep -c 'a' s.csv
for n in $(tail -n +2 s.csv | cut -d, -f1); do echo "hi $n"; done
cd /tmp && rm -rf dtwork
echo done
BIGSCRIPT
cp /tmp/dtbig /tmp/alpine-root/tmp/dtbig 2>/dev/null || true
# qemu's -L names a loader; it is not a chroot. A PATH lookup inside the guest
# therefore resolves to the HOST's binaries, so `sh -c "sed"` ran GNU sed on one side
# and busybox on the other - two different programs. Both sides are now pointed at a
# directory of links to one static busybox, which the host can execute directly (no
# loader needed) and which this backend resolves under the guest root.
DT_STATIC="${3:-/tmp/bbs/bin/busybox.static}"
if [ -x "$DT_STATIC" ]; then
    rm -rf /tmp/dt-applets "$ROOT/tmp/dt-applets"
    mkdir -p /tmp/dt-applets "$ROOT/tmp/dt-applets"
    "$RUNE" "$DT_STATIC" busybox --list 2>/dev/null | while read -r app; do
        [ -z "$app" ] && continue
        ln -sf "$DT_STATIC" "/tmp/dt-applets/$app" 2>/dev/null
        ln -sf "$DT_STATIC" "$ROOT/tmp/dt-applets/$app" 2>/dev/null
    done
    # The applet directory comes first, so a busybox applet always wins over the host
    # tool of the same name. Tools the harness itself needs (timeout, env) still
    # resolve, and an applet busybox does not have falls through to the same host
    # binary on both sides - which keeps the comparison fair.
    DT_PATH="/tmp/dt-applets:/bin:/usr/bin"
else
    DT_PATH="/bin:/usr/bin:/sbin:/usr/sbin"
fi
cat > /tmp/dts 2>/dev/null <<'STRESS'
set -e
cd /tmp && rm -rf dtst && mkdir dtst && cd dtst
i=0
while [ $i -lt 20 ]; do
  i=$((i + 1))
  echo "item$i $((i * 3))"
done > data.txt
echo "lines: $(wc -l < data.txt)"
echo "sum: $(awk '{s+=$2} END{print s}' data.txt)"
echo "max: $(sort -k2 -n data.txt | tail -1 | cut -d' ' -f2)"
echo "even: $(awk '$1 ~ /2$/ {c++} END{print c+0}' data.txt)"
first=$(head -1 data.txt | cut -d' ' -f1)
case "$first" in
  item*) echo "prefix ok" ;;
  *) echo "prefix bad" ;;
esac
upper=$(echo "$first" | tr a-z A-Z)
echo "upper: $upper"
echo "len: ${#upper}"
cd /tmp && rm -rf dtst
echo done
STRESS
cp /tmp/dts /tmp/alpine-root/tmp/dts 2>/dev/null || true
# Fixtures for find/grep: created in both roots, so qemu (not chrooted) sees the
# same tree this backend does.
for r in /tmp/dtfind "$ROOT/tmp/dtfind"; do
    rm -rf "$r"; mkdir -p "$r/a" "$r/b"
    echo 1 > "$r/a/x.txt"; echo 2 > "$r/b/y.txt"
done
pass=0
fail=0
while IFS= read -r cmd; do
    [ -z "$cmd" ] && continue
    case "$cmd" in \#*) continue ;; esac
    # The harness prints its own exit line on stdout; strip it so the comparison
    # is about the program's output alone.
    # Compare the whole output. `head -8` used to truncate both sides, which hid any
    # difference past the eighth line - a test that cannot see past a point is worse
    # than a shorter one that can. The harness's own exit line and the blank line
    # before it are stripped; nothing else is.
    mine=$(timeout 60 env PATH="$DT_PATH" "$RUNE" "$DT_STATIC" sh -c "$cmd" 2>&1 \
           | grep -v '^\[exit ' | sed -e :a -e '/^$/{$d;N;ba' -e '}')
    theirs=$(env -i PATH="$DT_PATH" timeout 60 qemu-aarch64-static -L "$ROOT" "$DT_STATIC" sh -c "$cmd" 2>&1 \
           | sed -e :a -e '/^$/{$d;N;ba' -e '}')
    if [ "$mine" = "$theirs" ]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        printf 'DIFF [%s]\n  mine: %s\n  qemu: %s\n' \
            "$cmd" "$(printf '%s' "$mine" | tr '\n' '|')" "$(printf '%s' "$theirs" | tr '\n' '|')"
    fi
done <<'EOF'
cat /tmp/dta
wc -l /tmp/dta
head -2 /tmp/dta
tail -1 /tmp/dta
sort /tmp/dta
sort -r /tmp/dta
sort -k2 -n /tmp/dta
sort -u /tmp/dta
awk '{print $2}' /tmp/dta
awk '$1=="a"' /tmp/dta
awk 'END{print NR}' /tmp/dta
awk '{s+=$2} END{print s}' /tmp/dta
awk 'NR>1 {print $1}' /tmp/dta
awk '{print length($0)}' /tmp/dtb
grep a /tmp/dta
grep -c a /tmp/dta
grep -v a /tmp/dta
grep -n b /tmp/dta
grep -E '[ab]' /tmp/dta
sed s/a/X/ /tmp/dta
sed -n 2p /tmp/dta
sed /a/d /tmp/dta
sed 's/\(a\)/[\1]/' /tmp/dta
tr a-z A-Z < /tmp/dta
cut -d" " -f2 /tmp/dta
cut -c2 /tmp/dtb
paste -s /tmp/dta
uniq /tmp/dta
tac /tmp/dta
nl /tmp/dta
rev /tmp/dtb
fold -w2 /tmp/dtb
md5sum /tmp/dta
cksum /tmp/dta
base64 /tmp/dta
od -c /tmp/dta
xxd /tmp/dta
cat -A /tmp/dtc
expr 3 + 4
expr abc : 'a.c'
expr 5 % 2
seq 3
seq 2 5
printf '%05.2f\n' 3.14159
echo $((7/2)) $((7%2)) $((2**8))
echo abc | rev
echo a b c | awk '{print NF, $NF}'
printf 'x\n' | tr x y
echo test | wc -c
awk 'BEGIN{print int(3.9), int(-3.9), int(0.5)}'
awk 'BEGIN{print 7%3, 7%2, -7%3}'
awk 'BEGIN{printf "%d %d\n", 7/2, 8/2}'
awk 'BEGIN{x="5"; print x+1, x*2}'
awk 'BEGIN{print substr("hello", 2, 3), length("hello")}'
awk 'BEGIN{s="a,b,c"; n=split(s,p,","); print n, p[1], p[3]}'
awk 'BEGIN{print toupper("ab"), tolower("CD")}'
awk 'BEGIN{printf "%s|%5s|%-5s|\n", "x", "y", "z"}'
awk 'BEGIN{printf "%o %x %X\n", 8, 255, 255}'
awk 'BEGIN{print index("hello","ll"), match("abc", /b/)}'
awk 'BEGIN{print sprintf("%.3f", 1/3)}'
awk 'BEGIN{a["k"]=1; a["j"]=2; for (i in a) print i, a[i]}'
awk 'BEGIN{print (1<2), (2<1)}'
awk 'BEGIN{print length()}'
awk 'NR==2 {print $0}' /tmp/dta
awk '{print NR": "$0}' /tmp/dta
awk 'END{print NR, NF}' /tmp/dta
sh /tmp/dtsh alpha beta
printf '%s\n' a b c | sed -n '$p'
echo 'a b' | awk '{print NF}'
printf 'x\ny\n' | sed 's/^/>/'
echo abc | sed 's/\(a\)\(b\)/\2\1/'
printf '1\n2\n3\n' | awk 'NR==2{print}'
echo 'a:b:c' | cut -d: -f2-
printf 'abc' | od -An -c
echo test | tr 'a-z' 'A-Z'
printf 'z\nb\na\n' | sort | tr '\n' ' '
echo 5 | awk '{printf "%03d\n", $1}'
printf 'a b\n' | awk '{print $NF}'
echo abcdef | sed 's/./X/g'
printf 'b\na\n' | sort | uniq -c
echo 3.7 | awk '{print int($1)}'
printf 'aXbXc\n' | tr 'X' '\n' | wc -l
echo 'Hello World' | cut -c1-5
printf '1\n2\n' | sed -n '/2/p'
echo abc | grep -q b && echo yes
seq 3 | awk '{s=s $1} END{print s}'
printf 'x\ty\n' | cut -f2
echo A | tr 'A-Z' 'a-z'
printf 'ab\ncd\n' | wc -l
echo test | rev | rev
seq 4 | awk 'END{print NR}'
echo 'a.b' | sed 's/\./-/'
printf 'q\n' | tee /tmp/dt-tee > /dev/null && cat /tmp/dt-tee
printf '3\n1\n2\n' | sort -n | head -2
echo 'aaa' | sed 's/a/b/2'
printf 'x\n' | awk '{print length($0)}'
echo 'a b c' | awk '{print $2,$3}'
printf '10\n9\n' | sort -n | tail -1
echo 'test' | md5sum | cut -c1-4
printf '%s\n' one two | tac
echo 'A,B' | cut -d, -f2
printf 'q\nq\nq\n' | sort -u | wc -l
echo abc | sed 's/b//'
printf 'b\na\nc\n' | sort | head -2
echo 'x' | sed 's/x/[&]/'
printf '1 2 3\n' | awk '{print $1+$3}'
seq 3 | sed -n '1p;3p'
echo 'a-b-c' | tr '-' '\n' | tail -1
printf 'aa\nbb\n' | uniq -c | tr -s ' '
echo 'Hello' | cut -c2-3
printf 'z\n' | wc -c
echo 'k=v' | awk -F= '{print $2}'
printf 'abc\n' | sum
printf 'abc\n' | cksum
printf 'abc\n' | sha256sum | cut -c1-16
printf 'abc\n' | sha1sum | cut -c1-16
printf 'abc\n' | md5sum | cut -c1-16
printf 'abc\n' | base64
printf 'abc\n' | od -An -tx1
printf 'c\na\nb\n' | sort -r | tr '\n' ' '
seq 5 | awk '{t+=$1; if($1%2) o+=$1} END{print t,o}'
printf 'x1\ny2\n' | sed 's/[0-9]//'
echo 'a,b,c' | awk -F, '{print NF}'
printf 'z\nb\n' | sort -u | wc -l
seq 4 | tac | head -2
printf 'q\n' | sed 's/q/Q/;s/Q/!/'
echo 'A B' | tr -s ' ' '_'
seq 3 | awk 'NR%2'
find /tmp/dtfind -name '*.txt' | sort
find /tmp/dtfind -type d | sort
find /tmp/dtfind -type f | wc -l
find /tmp/dtfind -maxdepth 1 | sort
grep -v 1 /tmp/dtfind/a/x.txt; echo ok
grep -l 1 /tmp/dtfind/a/x.txt /tmp/dtfind/b/y.txt
find /tmp/dtfind -name 'y*'
sh /tmp/dtbig
sh /tmp/dts
EOF
echo "diff test: $pass same, $fail different"
[ "$fail" -eq 0 ]
