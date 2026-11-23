#!/bin/sh
# Things that can be checked without Xcode, run before pushing.
#
# Written after five consecutive CI failures that were all editing mistakes - a block inserted
# into the wrong function, a property type left behind when its initialiser widened, a header not
# included, an environment variable left set. Every one of them would have failed in under a
# second here, and each cost a fifteen-minute round trip instead.
#
# This does not replace CI. It catches the mistakes I actually make.
set -u

fail=0
note() { printf '  %s %s\n' "$1" "$2"; }
bad() { note "FAIL" "$1"; fail=1; }
ok() { note "ok  " "$1"; }

echo "preflight"

# 1. The C the app compiles. E2Bridge.c and the harness are host-side C, and a missing include or
#    a name that does not exist shows up here rather than in a device build.
for f in PocketDSH/Core/Services/E2Bridge.c; do
    if cc -std=c11 -Wall -Wextra -c "$f" -o /tmp/preflight-bridge.o 2>/tmp/preflight-err; then
        ok "$f"
    else
        bad "$f"; sed -n '1,6p' /tmp/preflight-err
    fi
done

if make -s -C e2 harness >/tmp/preflight-make 2>&1; then
    ok "e2 harness"
else
    bad "e2 harness"; grep -E 'error:|warning:' /tmp/preflight-make | head -6
fi

# 2. Every Swift file must at least parse. This does not type-check - that needs the SDK - but it
#    catches an unbalanced brace or a mangled edit, which is most of what has gone wrong.
swift_files=$(git diff --name-only HEAD~1 2>/dev/null | grep '\.swift$')
[ -z "$swift_files" ] && swift_files=$(git ls-files '*.swift' | head -40)
swift_bad=0
for f in $swift_files; do
    [ -f "$f" ] || continue
    if ! swiftc -parse "$f" >/tmp/preflight-swift 2>&1; then
        # A file that imports the app module cannot be parsed alone; only syntax errors matter.
        if grep -qE 'error: (expected|unexpected|unterminated|extraneous)' /tmp/preflight-swift; then
            bad "$f"; sed -n '1,4p' /tmp/preflight-swift; swift_bad=1
        fi
    fi
done
[ "$swift_bad" -eq 0 ] && ok "swift syntax"

# 2b. Doubled escapes. Writing Swift through a heredoc twice produced sources containing a literal
#     backslash-paren, which is not an interpolation: it compiled, and produced a filename with
#     parentheses in it that the guest's shell rejected. Three of these in one file, so it is worth
#     a check.
doubled=$(grep -rln '\\\\(' --include='*.swift' PocketDSH PocketDSHTests 2>/dev/null | head -5)
if [ -n "$doubled" ]; then
    bad "doubled interpolation escapes in: $doubled"
else
    ok "no doubled escapes"
fi

# 3. The unit suite, which is fast and covers the interpreter.
if make -s -C e2 test >/tmp/preflight-test 2>&1; then
    ok "e2 unit suite"
else
    bad "e2 unit suite"; tail -3 /tmp/preflight-test
fi

if [ "$fail" -eq 0 ]; then
    echo "preflight: all clear"
else
    echo "preflight: fix the above before pushing"
fi
exit "$fail"
