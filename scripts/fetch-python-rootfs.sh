#!/bin/sh
# Assembles a guest root with CPython for Tier D.
#
# Tier D ships a busybox so that shell commands work. Python needs rather more: the
# interpreter, libpython, the standard library and musl. That is about 37 MB, which is more
# than belongs in git, so it is fetched from Alpine at build time.
#
#   scripts/fetch-python-rootfs.sh <destination> [--minimal]
#
# --minimal drops the parts CPython can start without: the test suite, the Tk bindings, and
# the modules that are only useful for building extensions. `import json`, `re`, `datetime`,
# `collections` and `math` all still work.
set -eu

DEST=${1:?usage: fetch-python-rootfs.sh <destination> [--minimal]}
MINIMAL=${2:-}

MIRROR=${ALPINE_MIRROR:-https://dl-cdn.alpinelinux.org/alpine}
VERSION=${ALPINE_VERSION:-v3.20}
ARCH=aarch64

WORK=$(mktemp -d "${TMPDIR:-/tmp}/pyroot.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

INDEX="$MIRROR/$VERSION/main/$ARCH/APKINDEX.tar.gz"
echo "fetching $INDEX"
curl -fsSL "$INDEX" -o "$WORK/APKINDEX.tar.gz"

# APKINDEX is a tar with an APKINDEX file inside; each package is a paragraph of K=V lines.
tar -xzf "$WORK/APKINDEX.tar.gz" -C "$WORK" APKINDEX 2>/dev/null || \
    tar -xzf "$WORK/APKINDEX.tar.gz" -C "$WORK"
# Note the found flag: `exit` does not skip the END block, so without it the version was
# printed twice and the URL was built from both lines.
awk -F: '
    /^P:/ { pkg = substr($0, 3) }
    /^V:/ { ver = substr($0, 3) }
    /^$/  { if (pkg == want) { print ver; found = 1; exit } pkg = ""; ver = "" }
    END   { if (!found && pkg == want) print ver }
' want=python3 "$WORK/APKINDEX" > "$WORK/python.version"

PYVER=$(head -1 "$WORK/python.version")
if [ -z "$PYVER" ]; then
    echo "python3 not found in the index" >&2
    exit 1
fi
echo "python3 $PYVER"

mkdir -p "$DEST"
# Busybox comes from the repo, not from here: it is static and version-controlled.
rm -rf "$DEST/usr" "$DEST/lib" "$DEST/etc"
mkdir -p "$DEST/usr" "$DEST/lib"

fetch_pkg() {
    name=$1
    ver=$(awk -F: -v w="$name" '
        /^P:/ { pkg = substr($0, 3) }
        /^V:/ { ver = substr($0, 3) }
        /^$/  { if (pkg == w) { print ver; found = 1; exit } pkg = ""; ver = "" }
        END   { if (!found && pkg == w) print ver }
    ' "$WORK/APKINDEX")
    [ -n "$ver" ] || { echo "missing package $name" >&2; return 1; }
    url="$MIRROR/$VERSION/main/$ARCH/$name-$ver.apk"
    echo "  $name $ver"
    curl -fsSL "$url" -o "$WORK/$name.apk"
    # An .apk is a gzipped tar whose members are the package's files, alongside a signature
    # and .PKGINFO. Older Alpine releases nested them in data.tar.gz; both shapes are handled
    # because the index does not say which.
    if tar -tzf "$WORK/$name.apk" 2>/dev/null | grep -qx 'data.tar.gz'; then
        tar -xzf "$WORK/$name.apk" -C "$WORK" data.tar.gz 2>/dev/null || true
        tar -xzf "$WORK/data.tar.gz" -C "$DEST" 2>/dev/null || true
        rm -f "$WORK/data.tar.gz"
    else
        tar -xzf "$WORK/$name.apk" -C "$DEST" \
            --exclude='.SIGN*' --exclude='.PKGINFO' --exclude='.INSTALL' 2>/dev/null || true
    fi
}

# musl provides the loader every dynamically linked guest binary needs.
fetch_pkg musl
for pkg in python3 libffi ncurses-libs readline zlib sqlite-libs xz-libs; do
    fetch_pkg "$pkg" || echo "  (skipping $pkg)"
done

if [ "$MINIMAL" = "--minimal" ]; then
    echo "trimming"
    rm -rf "$DEST/usr/lib/python3"*"/test" \
           "$DEST/usr/lib/python3"*"/idlelib" \
           "$DEST/usr/lib/python3"*"/tkinter" \
           "$DEST/usr/lib/python3"*"/turtledemo" \
           "$DEST/usr/lib/python3"*"/ensurepip" \
           "$DEST/usr/lib/python3"*"/config-"* \
           "$DEST/usr/lib/python3"*"/pydoc_data"
    # The shared objects CPython can live without.
    for so in _tkinter.cpython-*.so _ctypes_test*.so xxlimited*.so _test*.so; do
        find "$DEST" -name "$so" -delete 2>/dev/null || true
    done
fi

ln -sf /usr/bin/python3 "$DEST/usr/bin/python" 2>/dev/null || true
mkdir -p "$DEST/tmp"

echo "root is $(du -sh "$DEST" | cut -f1)"
