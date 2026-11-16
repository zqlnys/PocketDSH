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

pass=0
fail=0
while IFS= read -r cmd; do
    [ -z "$cmd" ] && continue
    # The harness prints its own exit line on stdout; strip it so the comparison
    # is about the program's output alone.
    mine=$(timeout 30 "$RUNE" "$BUSYBOX" sh -c "$cmd" 2>&1 | grep -v '^\[exit ' | head -8)
    theirs=$(env -i timeout 30 qemu-aarch64-static -L "$ROOT" "$BUSYBOX" sh -c "$cmd" 2>&1 | head -8)
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
EOF
echo "diff test: $pass same, $fail different"
[ "$fail" -eq 0 ]
