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
#
# Retried: the asset is served through a redirect and the first request can arrive before it is
# ready, which returned 404 once and looked like a failed publish.
URL="https://github.com/zqlnys/PocketDSH/releases/download/$TAG/$NAME"
printf 'verifying %s\n' "$URL"
attempt=1
while [ "$attempt" -le 5 ]; do
    code=$(env -u GH_TOKEN -u GITHUB_TOKEN curl -sL -o /dev/null -w '%{http_code}' "$URL" || echo "000")
    if [ "$code" = "200" ]; then
        size=$(env -u GH_TOKEN -u GITHUB_TOKEN curl -sL -o /dev/null -w '%{size_download}' "$URL" || echo 0)
        printf 'anonymous download HTTP 200  %s bytes\n' "$size"
        exit 0
    fi
    printf 'attempt %d: HTTP %s, waiting\n' "$attempt" "$code"
    attempt=$((attempt + 1))
    sleep 20
done
printf 'the asset never became downloadable; the release exists but the link does not work\n' >&2
exit 1
