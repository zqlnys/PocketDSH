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
# Busybox comes from the repo, not from here: it is static and version-controlled. Only the
# package-managed trees are replaced, so running this over an existing root keeps bin/.
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
# mpdecimal matters: without it, decimal silently falls back to the pure-Python implementation,
# which is correct and far slower - the kind of difference nothing notices.
#
# openssl and expat are deliberately absent. Their package names differ between Alpine releases,
# the guesses did not resolve, and a build that reports which modules are unavailable is worth
# more than one that installs whatever the names happened to be this time. The check below says
# exactly what is missing and what it costs.
for pkg in python3 libffi ncurses-libs readline zlib sqlite-libs xz-libs mpdecimal; do
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

# An iOS app bundle may not contain symlinks that point outside it, and the installer refuses
# the whole app when one does - "validateSymlinksInURLDoNotEscapeURL". Alpine's packages are
# full of absolute links (/usr/bin/python3 -> /usr/bin/python3.12), which read as escaping from
# inside the bundle. They are rewritten relative where the target is inside the root, and
# dropped when it is not.
echo "rewriting absolute symlinks"
find "$DEST" -type l 2>/dev/null | while read -r link; do
    target=$(readlink "$link")
    case "$target" in
        /*)
            resolved="$DEST$target"
            if [ -e "$resolved" ]; then
                rel=$(python3 -c '
import os, sys
link, resolved = sys.argv[1], sys.argv[2]
print(os.path.relpath(resolved, os.path.dirname(link)))
' "$link" "$resolved")
                ln -sfn "$rel" "$link"
            else
                rm -f "$link"
            fi
            ;;
    esac
done
find "$DEST" -type l -xtype l -delete 2>/dev/null || true

mkdir -p "$DEST/tmp"

# Precompiling happens in the app build, not here: a git clean runs between the two and .pyc
# matches a gitignore rule, so files written now do not survive to the build.

# Every extension Python ships must have the libraries it links against. The trim removed
# libbz2 while keeping _bz2, so `import bz2` failed at run time with "Error loading shared library
# libbz2.so.1" - a packaging mistake that only shows when someone imports that module.
missing=0
for ext in "$DEST/usr/lib/python3"*/lib-dynload/*.so; do
    [ -f "$ext" ] || continue
    for need in $(readelf -d "$ext" 2>/dev/null | sed -n 's/.*NEEDED.*\[\(.*\)\]/\1/p'); do
        case "$need" in
            libc.*|ld-musl*) continue ;;
        esac
        if [ ! -e "$DEST/usr/lib/$need" ] && [ ! -e "$DEST/lib/$need" ]; then
            mod=$(basename "$ext" | sed 's/\.cpython.*//')
            echo "  missing: $need - import $mod will fail"
            missing=$((missing + 1))
        fi
    done
done
if [ "$missing" -gt 0 ]; then
    echo "WARNING: $missing shared libraries are missing; those modules will fail to import"
else
    echo "every bundled extension has its libraries"
fi

echo "root is $(du -sh "$DEST" | cut -f1)"
