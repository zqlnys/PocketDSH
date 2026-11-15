#!/usr/bin/env bash
set -euo pipefail

# Pick the newest stable Xcode installed on a GitHub macOS runner (falling back
# to a beta only if no stable image is present), then activate it for this job.

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "select_xcode.sh only runs on macOS." >&2
  exit 1
fi

if [[ ! -d /Applications ]]; then
  echo "/Applications is missing; cannot discover Xcode." >&2
  exit 1
fi

echo "Installed Xcode applications:"
find /Applications -maxdepth 1 -type d -name 'Xcode*.app' -print | sort || true

SELECTED="$(python3 - <<'PY'
import glob
import os
import plistlib
import re
import sys

candidates = []
for path in glob.glob("/Applications/Xcode*.app"):
    plist_path = os.path.join(path, "Contents", "Info.plist")
    try:
        with open(plist_path, "rb") as handle:
            plist = plistlib.load(handle)
        version = str(plist.get("CFBundleShortVersionString", "0"))
    except Exception as error:
        print(f"Skipping {path}: {error}", file=sys.stderr)
        continue

    numbers = tuple(int(part) for part in re.findall(r"\d+", version)[:3])
    numbers = numbers + (0,) * (3 - len(numbers))
    is_beta = "beta" in version.lower() or "beta" in os.path.basename(path).lower()
    candidates.append((is_beta, numbers, path, version))

if not candidates:
    print("No Xcode installations found under /Applications.", file=sys.stderr)
    sys.exit(1)

stable = [candidate for candidate in candidates if not candidate[0]]
pool = stable or candidates
pool.sort(key=lambda candidate: (candidate[1], candidate[2]))
selected = pool[-1]

print(f"Xcode candidate list: {[(c[3], c[0]) for c in pool]}", file=sys.stderr)
print(f"Selected Xcode {selected[3]} at {selected[2]}", file=sys.stderr)
print(selected[2])
PY
)"

if [[ -z "${SELECTED}" || ! -d "${SELECTED}/Contents/Developer" ]]; then
  echo "Failed to resolve a usable Xcode." >&2
  exit 1
fi

if [[ "$(id -u)" -eq 0 ]]; then
  xcode-select --switch "${SELECTED}/Contents/Developer"
else
  sudo xcode-select --switch "${SELECTED}/Contents/Developer"
fi

echo "Active developer directory: $(xcode-select -p)"
xcodebuild -version

if [[ -n "${GITHUB_OUTPUT:-}" ]]; then
  echo "path=${SELECTED}" >> "${GITHUB_OUTPUT}"
fi
