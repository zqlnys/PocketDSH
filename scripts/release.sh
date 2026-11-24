#!/bin/sh
# Publishes an IPA, taking the version from the artifact rather than from the person writing the
# notes. Last round the notes said 919 and the file was 918: the number was typed, and typing it is
# the thing that can be wrong.
#
# Usage: scripts/release.sh <ipa> <tag> <title-suffix> <notes-file>
set -eu
IPA="${1:?usage: release.sh <ipa> <tag> <title-suffix> <notes-file>}"
TAG="${2:?tag required}"
SUFFIX="${3:?title suffix required}"
NOTES="${4:?notes file required}"

[ -f "$IPA" ] || { echo "no such file: $IPA" >&2; exit 1; }
[ -f "$NOTES" ] || { echo "no notes file: $NOTES" >&2; exit 1; }

NAME=$(basename "$IPA")
VERSION=$(printf '%s' "$NAME" | sed -n 's/^PocketDSH-\([0-9.]*\)-\([0-9]*\)-unsigned\.ipa$/\1/p')
BUILD=$(printf '%s' "$NAME" | sed -n 's/^PocketDSH-\([0-9.]*\)-\([0-9]*\)-unsigned\.ipa$/\2/p')

if [ -z "$VERSION" ] || [ -z "$BUILD" ]; then
    echo "the file name does not carry a version; refusing to guess" >&2
    exit 1
fi

TITLE="PocketDSH iOS ${VERSION} (${BUILD}) · ${SUFFIX}"
echo "releasing $TITLE from $NAME"

gh release create "$TAG" "$IPA" --title "$TITLE" --notes-file "$NOTES"

# Anonymous download, because a link that only works when authenticated is not a link.
URL="https://github.com/zqlnys/PocketDSH/releases/download/$TAG/$NAME"
printf 'verifying %s\n' "$URL"
env -u GH_TOKEN -u GITHUB_TOKEN curl -sL -o /dev/null \
    -w "anonymous download HTTP %{http_code}  %{size_download} bytes\n" "$URL"
