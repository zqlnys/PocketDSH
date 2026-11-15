#!/usr/bin/env bash
set -euo pipefail

# Package a device .app as an unsigned IPA. Signing happens later on the user's
# Windows machine with Sideloadly (or in a signed CI workflow with secrets).
#
# Usage: package_ipa.sh /path/to/Orbit.app /path/to/output [artifact-name]

APP_PATH="${1:?usage: package_ipa.sh <app-path> <output-dir> [name]}"
OUTPUT_DIR="${2:?usage: package_ipa.sh <app-path> <output-dir> [name]}"
NAME="${3:-$(basename "${APP_PATH}" .app)}"

if [[ ! -d "${APP_PATH}" ]]; then
  echo "App bundle not found: ${APP_PATH}" >&2
  exit 1
fi

mkdir -p "${OUTPUT_DIR}"
OUTPUT_DIR="$(cd "${OUTPUT_DIR}" && pwd)"
STAGE="$(mktemp -d "${TMPDIR:-/tmp}/orbit-ipa.XXXXXX")"
trap 'rm -rf "${STAGE}"' EXIT

mkdir -p "${STAGE}/Payload"
if command -v ditto >/dev/null 2>&1; then
  ditto "${APP_PATH}" "${STAGE}/Payload/${NAME}.app"
else
  cp -R "${APP_PATH}" "${STAGE}/Payload/${NAME}.app"
fi

BUNDLE_APP="${STAGE}/Payload/${NAME}.app"
if [[ ! -f "${BUNDLE_APP}/${NAME}" ]]; then
  echo "App executable not found inside bundle: ${BUNDLE_APP}/${NAME}" >&2
  exit 1
fi

if command -v plutil >/dev/null 2>&1; then
  plutil -lint "${BUNDLE_APP}/Info.plist"
fi

if command -v /usr/libexec/PlistBuddy >/dev/null 2>&1; then
  BUNDLE_ID="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "${BUNDLE_APP}/Info.plist")"
  BUNDLE_VERSION="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "${BUNDLE_APP}/Info.plist")"
  echo "Bundle: ${BUNDLE_ID} ${BUNDLE_VERSION}"
fi

if command -v lipo >/dev/null 2>&1; then
  echo "Architectures:"
  lipo -info "${BUNDLE_APP}/${NAME}"
fi

IPA_PATH="${OUTPUT_DIR}/${NAME}-unsigned.ipa"
rm -f "${IPA_PATH}"
(
  cd "${STAGE}"
  zip -qry "${IPA_PATH}" Payload
)

unzip -t "${IPA_PATH}" >/dev/null
SHA256="$(shasum -a 256 "${IPA_PATH}" | awk '{print $1}')"

echo "IPA: ${IPA_PATH}"
echo "SHA-256: ${SHA256}"
echo "Size: $(du -h "${IPA_PATH}" | awk '{print $1}')"
