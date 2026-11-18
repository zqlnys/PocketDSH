#!/bin/sh
# Build and verify a fresh clone of this repository.
#
# Every check in this project runs inside a working directory that accumulates state -
# files created while testing, device nodes made by hand, fixtures left behind. A clone has
# none of it, which makes this the only honest answer to "is the committed tree enough".
#
#   tools/fresh-clone-check.sh [DEST]
#
# Exits non-zero if the clone fails to build or fails to verify. DEST is removed first, so
# pointing it at something that matters would be a mistake - the default is under /tmp.
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
dest=${1:-/tmp/dsh-fresh-clone}

case "$dest" in
    /tmp/*) ;;
    *) echo "fresh-clone-check: refusing to use $dest outside /tmp" >&2; exit 2 ;;
esac

rm -rf "$dest" || exit 2
echo "== cloning =="
git clone -q "$here" "$dest" || { echo "clone failed" >&2; exit 1; }

echo "== building =="
( cd "$dest/e2" && make ) > "$dest/.build.log" 2>&1
build_rc=$?
tail -2 "$dest/.build.log"
if [ "$build_rc" -ne 0 ]; then
    echo "fresh-clone-check: build FAILED" >&2
    exit 1
fi
for bin in e2/build/e2-tests e2/tools/rune; do
    if [ ! -x "$dest/$bin" ]; then
        echo "fresh-clone-check: $bin was not produced" >&2
        exit 1
    fi
done

echo "== verifying =="
if ( cd "$dest" && ./tools/verify.sh ) > "$dest/.verify.log" 2>&1; then
    tail -3 "$dest/.verify.log"
    echo "fresh-clone-check: a fresh clone builds and passes"
    exit 0
fi

echo "fresh-clone-check: verification FAILED" >&2
tail -12 "$dest/.verify.log" >&2
exit 1
