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

# Refuse to run without the pieces, rather than comparing a subset and reporting a
# smaller number. Without this, a missing root filesystem produced "25 same" instead of
# 182 - and the documentation check then reported the docs as out of date, pointing at
# the wrong thing entirely.
if [ ! -d "$ROOT" ]; then
    echo "diff test: $ROOT not found - the guest root filesystem is required" >&2
    echo "           (see docs/E2-BACKEND.md for how it is set up)" >&2
    exit 2
fi
if [ ! -x "$BUSYBOX" ]; then
    echo "diff test: $BUSYBOX not found - the static busybox is required" >&2
    exit 2
fi
if [ ! -x "$RUNE" ]; then
    echo "diff test: $RUNE not found - run 'make' in e2/ first" >&2
    exit 2
fi

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
cat > /tmp/dther 2>/dev/null <<'OUTER'
cat <<EOF
hello
world
EOF
cat <<'EOF'
raw $HOME text
EOF
cat <<-EOF
	indented
	EOF
wc -l <<EOF
a
b
EOF
while read -r l; do echo "[$l]"; done <<EOF
x
y
EOF
cat <<EOF | wc -l
p
q
EOF
OUTER
cp /tmp/dther /tmp/alpine-root/tmp/dther 2>/dev/null || true
cat > /tmp/dtf 2>/dev/null <<'WRAP'
f() { return 7; }; f; echo "ret=$?"
g() { local v=inner; echo "$v"; }; g
case abc in
  a*) echo "prefix" ;;
  *) echo "other" ;;
esac
case x in
  [0-9]) echo digit ;;
  [a-z]) echo letter ;;
esac
i=0; until [ $i -ge 3 ]; do i=$((i+1)); done; echo "until=$i"
for w in "a b" c; do echo "w=$w"; done
printf '%s\n' one two three | while read -r l; do echo "<$l>"; done
x=5; [ $x -gt 3 ] && [ $x -lt 10 ] && echo range
echo $((0x10 + 010))
WRAP
cp /tmp/dtf /tmp/alpine-root/tmp/dtf 2>/dev/null || true
cat > /tmp/dtj 2>/dev/null <<'WRAP'
sleep 0.01 & wait; echo "waited=$?"
sleep 0.01 & p=$!; wait $p; echo "waitedpid=$?"
( echo bg ) & wait
# Background jobs finish in whatever order the scheduler picks, so their output is
# sorted before comparison - otherwise the case measures scheduling luck rather than
# behaviour.
for i in 1 2 3; do ( echo "sub$i" ) >> /tmp/dtjout & done; wait; sort /tmp/dtjout; rm -f /tmp/dtjout
true && echo and || echo or
! false && echo negated
WRAP
cp /tmp/dtj /tmp/alpine-root/tmp/dtj 2>/dev/null || true
cat > /tmp/dt-rp.sh 2>/dev/null <<'WRAP'
cd /tmp && rm -rf dtrp && mkdir dtrp && cd dtrp
for i in $(seq 1 30); do
  printf 'user%02d,%d,%s\n' $i $((i * 7 % 13)) "$([ $((i % 3)) -eq 0 ] && echo active || echo idle)"
done > data.csv
echo "== head =="
head -2 data.csv
echo "== active count =="
grep -c ',active$' data.csv
echo "== max score =="
cut -d, -f2 data.csv | sort -n | tail -1
echo "== avg =="
cut -d, -f2 data.csv | awk '{s+=$1} END{printf "%.2f\n", s/NR}'
echo "== top 3 users =="
sort -t, -k2 -nr data.csv | head -3 | cut -d, -f1
echo "== by status =="
cut -d, -f3 data.csv | sort | uniq -c | sort -nr
cd /tmp && rm -rf dtrp
echo done
WRAP
cp /tmp/dt-rp.sh /tmp/alpine-root/tmp/dt-rp.sh 2>/dev/null || true
cat > /tmp/dtar 2>/dev/null <<'WRAP'
echo $((2147483647 + 1))
echo $((9223372036854775807))
echo $((-9223372036854775807 - 1))
echo $((10 / 3)) $((10 % 3)) $((-10 / 3)) $((-10 % 3))
echo $((2 ** 10))
echo $((1 << 30))
echo $((1024 >> 3))
echo $((~0))
echo $((5 & 3)) $((5 | 3)) $((5 ^ 3))
echo $((1 == 1)) $((1 != 1))
echo $((0x7fffffff + 1))
WRAP
cp /tmp/dtar /tmp/alpine-root/tmp/dtar 2>/dev/null || true
cat > /tmp/dtu 2>/dev/null <<'WRAP'
printf 'héllo\n' | wc -c
printf 'héllo\n' | wc -l
printf '世界\n' | wc -c
printf '世界\n' | cut -c1-3 | od -An -c
printf 'a\tb\n' | expand | od -An -c
printf 'ＡＢ\n' | od -An -tx1
echo 'ñ' | tr 'ñ' 'n' | od -An -c
printf '%s\n' '①' '②' | wc -l
WRAP
cp /tmp/dtu /tmp/alpine-root/tmp/dtu 2>/dev/null || true
rm -rf /tmp/dtfs /tmp/alpine-root/tmp/dtfs
mkdir -p /tmp/dtfs /tmp/alpine-root/tmp/dtfs
cat > /tmp/dtfs.sh 2>/dev/null <<'WRAP'
cd /tmp/dtfs 2>/dev/null || { cd /tmp && rm -rf dtfs && mkdir dtfs && cd dtfs; }
echo a > 'file with spaces.txt'
echo b > 'quote"inside.txt'
mkdir -p a/b/c/d/e
echo deep > a/b/c/d/e/f.txt
name=$(printf 'x%.0s' $(seq 100))
echo long > "$name.txt"
ls | wc -l
find . -type f | wc -l
cat 'file with spaces.txt'
cat "./quote\"inside.txt"
cat a/b/c/d/e/f.txt
wc -c < "$name.txt"
cd /tmp && rm -rf dtfs
WRAP
cp /tmp/dtfs.sh /tmp/alpine-root/tmp/dtfs.sh 2>/dev/null || true
cat > /tmp/dtpf 2>/dev/null <<'WRAP'
printf '%d|%5d|%-5d|%05d\n' 42 42 42 42
printf '%s|%5s|%-5s\n' ab ab ab
printf '%x|%X|%o\n' 255 255 8
printf '%c\n' A
printf '%c%c\n' ABC DEF
printf '%e|%E\n' 1234.5 1234.5
printf '%f|%.2f|%8.3f\n' 3.14159 3.14159 3.14159
printf '%g|%G\n' 0.0001 1000000
printf '%%\n'
printf '%5.2s|\n' abcdef
printf '%+d|% d\n' 5 5
printf '%i\n' 42
WRAP
cp /tmp/dtpf /tmp/alpine-root/tmp/dtpf 2>/dev/null || true
cat > /tmp/dtex 2>/dev/null <<'WRAP'
expr 1 + 2
expr 10 - 3
expr 3 \* 4
expr 10 / 3
expr 10 % 3
expr 1 \< 2
expr 2 \<= 2
expr abc = abc
expr abc != def
expr length abcdef
expr substr abcdef 2 3
expr index abcdef cd
expr match abcdef abc
expr "a:b:c" : '.*:\([a-z]*\)'
[ 1 -eq 1 ] && echo eq
[ 2 -ne 3 ] && echo ne
[ 5 -ge 5 ] && echo ge
[ -n "x" ] && echo nonempty
[ -z "" ] && echo empty
[ "a" = "a" ] && echo streq
test -e /etc && echo exists
test ! -e /nonexistent && echo missing
WRAP
cp /tmp/dtex /tmp/alpine-root/tmp/dtex 2>/dev/null || true
cat > /tmp/dtre 2>/dev/null <<'WRAP'
echo abc | grep '^a'
echo abc | grep 'c$'
echo abc | grep '^abc$'
echo aaa | grep 'a\{3\}'
echo abc | grep '[abc]\{2\}'
echo 'a1b2' | grep '[0-9]'
echo 'a1b2' | grep '[^0-9]'
echo 'foo' | grep -E 'f(o)+'
echo 'foo' | grep -E 'f(o|0)+'
echo 'a.c' | grep '\.'
echo 'axc' | grep 'a.c'
echo 'AbC' | grep -i 'abc'
echo 'a b' | grep '[[:space:]]'
echo 'a1' | grep '[[:digit:]]'
echo 'ab' | grep 'a*b'
echo 'b' | grep 'a*b'
echo 'x' | sed 's/x\|y/Z/'
echo 'aab' | sed 's/a*/X/'
echo 'abc' | sed -E 's/(a)(b)/\2\1/'
echo 'aaa' | grep -c 'a'
WRAP
cp /tmp/dtre /tmp/alpine-root/tmp/dtre 2>/dev/null || true
cat > /tmp/dtlnk.sh 2>/dev/null <<'WRAP'
rm -rf /tmp/dtlnk; mkdir -p /tmp/dtlnk; ln -s target /tmp/dtlnk/l; ls /tmp/dtlnk | wc -l
rm -rf /tmp/dtlnk; mkdir -p /tmp/dtlnk; ln -s target /tmp/dtlnk/l; rm -rf /tmp/dtlnk; ls -d /tmp/dtlnk 2>&1
rm -rf /tmp/dtlnk; mkdir -p /tmp/dtlnk/a; ln -s ../missing /tmp/dtlnk/a/l; find /tmp/dtlnk | sort
WRAP
cp /tmp/dtlnk.sh /tmp/alpine-root/tmp/dtlnk.sh 2>/dev/null || true
cat > /tmp/dtalgo.sh 2>/dev/null <<'WRAP'
fib() { [ $1 -le 1 ] && echo $1 || echo $(( $(fib $(($1-1))) + $(fib $(($1-2))) )); }
echo "fib(10)=$(fib 10)"
primes=""
for n in $(seq 2 40); do
  is=1
  for d in $(seq 2 $((n-1))); do
    [ $((n % d)) -eq 0 ] && { is=0; break; }
  done
  [ $is -eq 1 ] && primes="$primes $n"
done
echo "primes:$primes"
for i in 3 1 2; do for j in 3 1 2; do echo "$i$j"; done; done | sort | head -3
n=10; s=0; while [ $n -gt 0 ]; do s=$((s+n)); n=$((n-1)); done; echo "sum=$s"
WRAP
cp /tmp/dtalgo.sh /tmp/alpine-root/tmp/dtalgo.sh 2>/dev/null || true
cat > /tmp/dtstress.sh 2>/dev/null <<'WRAP'
echo "$(echo "$(echo "$(echo deep)")")"
x=$(printf 'a%.0s' $(seq 500)); echo ${#x}
long=$(seq 200 | tr '\n' ' '); echo "$long" | wc -w
( ( ( ( echo nested ) ) ) )
echo "$(( 1 + 2 * 3 - 4 / 2 ))"
echo $(seq 50 | tail -1)
seq 100 | awk 'BEGIN{m=0} {if($1>m)m=$1} END{print "max="m}'
for i in 1 2; do for j in 3 4; do for k in 5 6; do echo "$i$j$k"; done; done; done | wc -l
WRAP
cp /tmp/dtstress.sh /tmp/alpine-root/tmp/dtstress.sh 2>/dev/null || true
pass=0
fail=0
empty=
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
        # Both sides empty is a pass that proves very little: a case whose command was
        # mangled into doing nothing also produces nothing on both sides. Collected and
        # reported so each one can be judged rather than trusted.
        if [ -z "$mine" ]; then
            empty="$empty
  [$cmd]"
        fi
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
printf 'aaa\n' | sed 's/a/b/g'
printf 'ab\ncd\n' | sed '1d'
printf 'ab\ncd\n' | sed '$d'
printf 'one two\n' | sed 's/\([a-z]*\) \([a-z]*\)/\2 \1/'
seq 5 | awk '{a[NR]=$1} END{for(i=NR;i>0;i--) printf "%s ", a[i]; print ""}'
printf 'x\n' | sed 's/x/y/;t;s/$/!/'
seq 3 | awk 'BEGIN{FS=""}{print NF}'
printf 'a.b.c\n' | sed 's/\./:/g'
echo 'test' | awk '{gsub(/e/,"E"); print}'
seq 4 | awk 'NR==2,NR==3'
f() { [ $1 -le 0 ] && echo done || { echo $1; f $(($1 - 1)); }; }; f 5
f() { echo "x$1"; [ $1 -lt 3 ] && f $(($1 + 1)); }; f 1
for i in 1 2 3; do for j in a b; do echo $i$j; done; done
i=0; while [ $i -lt 3 ]; do i=$((i+1)); done; echo $i
echo $(( (2 + 3) * 4 ))
x=abc; echo ${x}def ${x%c} ${x#a}
set -- a b c; echo $# $1 $3
set -- a b; echo "$@"
set -- "a b" c; echo $#
false; echo $?
true; echo $?
IFS=:; x=a:b; for i in $x; do echo $i; done
name=world; echo "hello $name"
echo "${undefined:-default}"
x=; echo "${x:-empty}"
( exit 3 ); echo $?
( echo sub ); echo main
{ echo a; echo b; } | wc -l
trap 'echo caught' EXIT; echo body
trap '' INT; echo ignored
x=$(exit 4); echo $?
false | true; echo $?
true | false; echo $?
echo start; ( exit 1 ) || echo failed
sh /tmp/dtbig
sh /tmp/dts
sh /tmp/dther
sh /tmp/dtf
sh /tmp/dtre
sh /tmp/dtj
sh /tmp/dt-rp.sh
printf 'a\nb\n' | sort -r
seq 6 | sed -n '2,4p'
echo 'x:y:z' | cut -d: -f2
printf '1\n2\n2\n3\n' | uniq -c
seq 5 | paste -sd,
echo 'Hello' | tr 'A-Z' 'a-z'
printf 'b\na\nc\n' | sort | head -2
seq 3 | while read n; do echo "n=$n"; done
echo '  trim  ' | sed 's/^ *//;s/ *$//'
printf 'x\ty\n' | cut -f2
printf 'abc' | od -An -tx1
echo 'test' | md5sum
seq 4 | xargs -n2 echo
echo '1.5 2.5' | awk '{printf "%.1f\n", $1+$2}'
expr 17 % 5
echo 'a b c' | cut -d' ' -f2-3
printf 'x\n' | sed 'p'
echo 'aXbXc' | tr 'X' '\n'
echo 'aaa' | awk '{gsub(/a/,"b"); print}'
printf '1\n10\n2\n' | sort -n
printf '1\n10\n2\n' | sort
seq 4 | tac
echo 'xy' | sed 's/x//'
sh /tmp/dtar
sh /tmp/dtu
sh /tmp/dtfs.sh
sh /tmp/dtpf
# Argument limits that were silently truncating: a long single argument, and many
# arguments. Both regressed once already, in opposite directions. The awk program below
# is 2018 characters, past the 1024 that used to cut it in half and make awk report a
# syntax error in a program it was never given whole.
awk 'BEGIN{s=0;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;s+=1;print s}'
echo $(seq 30)
set -- $(seq 25); echo $#
printf 'abc 123\nxy 789\nqq 456\n' | sed 's/a/X/g'
printf 'abc 123\nxy 789\nqq 456\n' | sed 's/[0-9]/N/g'
printf 'abc 123\nxy 789\nqq 456\n' | sed 's/^/>/'
printf 'abc 123\nxy 789\nqq 456\n' | sed 's/$/</'
printf 'abc 123\nxy 789\nqq 456\n' | sed 's/  */ /g'
printf 'abc 123\nxy 789\nqq 456\n' | sed 's/.*/\U&/'
printf 'abc 123\nxy 789\nqq 456\n' | sed 'y/abc/xyz/'
printf 'abc 123\nxy 789\nqq 456\n' | sed 's/\(.\)\(.\)/\2\1/'
printf 'abc 123\nxy 789\nqq 456\n' | awk '{print NR": "$0}'
printf 'abc 123\nxy 789\nqq 456\n' | awk '{s+=$2; print s}'
printf 'abc 123\nxy 789\nqq 456\n' | awk 'END{print NR}'
printf 'abc 123\nxy 789\nqq 456\n' | awk '/abc/{print}'
printf 'abc 123\nxy 789\nqq 456\n' | awk '{print toupper($1)}'
printf 'abc 123\nxy 789\nqq 456\n' | awk 'NF>1{print $2}'
printf 'abc 123\nxy 789\nqq 456\n' | awk '{print length($0)}'
printf 'abc 123\nxy 789\nqq 456\n' | awk 'BEGIN{print "x"}{print $1}'
sh /tmp/dtalgo.sh
sh /tmp/dtstress.sh
sh /tmp/dtlnk.sh
sh /tmp/dtex
EOF
# The same guard the sweeps have: an emptied case list would report "0 same, 0 different",
# which reads like a clean run.
if [ $((pass + fail)) -lt 100 ]; then
    echo "diff test: only $((pass + fail)) case(s) ran, expected at least 100" >&2
    exit 1
fi
echo "diff test: $pass same, $fail different"
if [ -n "$empty" ]; then
    echo "  $(( $(printf '%s' "$empty" | grep -c '^  \[') )) case(s) produced no output on either side:"
    printf '%s\n' "$empty"
fi
[ "$fail" -eq 0 ]
