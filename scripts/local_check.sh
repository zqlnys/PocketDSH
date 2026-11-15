#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

WITH_TESTS=0
for argument in "$@"; do
  case "$argument" in
    --with-tests) WITH_TESTS=1 ;;
    *) echo "Unknown argument: $argument" >&2; exit 2 ;;
  esac
done

failures=0
pass() { printf '  PASS  %s\n' "$1"; }
fail() { printf '  FAIL  %s\n' "$1"; failures=$((failures + 1)); }

echo "== PocketDSH local checks =="

required_files=(
  "project.yml"
  ".github/workflows/ios.yml"
  "PocketDSH/App/PocketDSHApp.swift"
  "PocketDSH/App/AppState.swift"
  "PocketDSH/Core/Services/AgentRunner.swift"
  "PocketDSH/Core/Services/ToolExecutor.swift"
  "PocketDSH/Core/Services/ProjectStore.swift"
  "PocketDSH/Features/Workspace/FilesView.swift"
  "PocketDSH/Features/Agent/AgentView.swift"
  "PocketDSHTests/PocketDSHTests.swift"
  "Packages/PocketDSHKit/Package.swift"
  "Packages/PocketDSHKit/Sources/CLua/pocket_lua.c"
  "Packages/PocketDSHKit/Sources/LuaRuntime/LuaEngine.swift"
  "PocketDSH/Resources/Assets.xcassets/AppIcon.appiconset/AppIcon-1024.png"
)
for file in "${required_files[@]}"; do
  if [[ -s "$file" ]]; then pass "$file"; else fail "missing or empty: $file"; fi
done

echo "-- YAML and JSON syntax --"
if python3 - <<'PY'
from pathlib import Path
import json, sys, yaml
failed = False
for path in [Path("project.yml"), Path(".github/workflows/ios.yml")]:
    try:
        with path.open() as handle:
            list(yaml.safe_load_all(handle))
        print(f"  PASS  {path}")
    except Exception as error:
        print(f"  FAIL  {path}: {error}")
        failed = True
for path in Path(".").rglob("*.json"):
    if any(part in {".git", "build", "artifacts", ".build"} for part in path.parts):
        continue
    try:
        with path.open() as handle:
            json.load(handle)
    except Exception as error:
        print(f"  FAIL  {path}: {error}")
        failed = True
sys.exit(1 if failed else 0)
PY
then :; else fail "YAML/JSON validation"; fi

if command -v swiftc >/dev/null 2>&1; then
  :
elif [[ -f "${SWIFTLY_HOME_DIR:-$HOME/.local/share/swiftly}/env.sh" ]]; then
  # shellcheck disable=SC1090
  source "${SWIFTLY_HOME_DIR:-$HOME/.local/share/swiftly}/env.sh"
fi

if command -v swiftc >/dev/null 2>&1; then
  echo "-- Swift syntax parse --"
  swift_files=()
  while IFS= read -r -d '' file; do swift_files+=("$file"); done \
    < <(find PocketDSH Packages -path '*/.build' -prune -o -name '*.swift' -print0)
  if ((${#swift_files[@]} > 0)) && swiftc -parse "${swift_files[@]}"; then
    pass "parsed ${#swift_files[@]} Swift files"
  else
    fail "Swift syntax parse"
  fi
else
  echo "  SKIP  swiftc not installed"
fi

echo "-- Guardrails --"
if grep -RInE 'ghp_[A-Za-z0-9]{36}|github_pat_[A-Za-z0-9_]{50,}|sk-[A-Za-z0-9]{32,}|BEGIN (RSA|OPENSSH|EC) PRIVATE KEY' \
  --exclude-dir=.git --exclude-dir=.build --exclude-dir=build --exclude-dir=artifacts \
  --exclude='*.png' . >/tmp/pocketdsh-secret-scan.txt 2>/dev/null; then
  cat /tmp/pocketdsh-secret-scan.txt
  fail "possible secret material found"
else
  pass "no obvious token/private-key material"
fi

if grep -RInE 'WKWebView|WebKit|import Flutter|React Native|Capacitor|Cordova' \
  PocketDSH Packages --include='*.swift' --exclude-dir=.build >/tmp/pocketdsh-framework-scan.txt 2>/dev/null; then
  cat /tmp/pocketdsh-framework-scan.txt
  fail "non-native UI/runtime technology detected"
else
  pass "native SwiftUI + embedded Lua only"
fi

if grep -RInE 'posix_spawn|fork\s*\(|popen\s*\(|system\s*\(' Packages/PocketDSHKit/Sources/CLua --include='*.c' >/tmp/pocketdsh-process-scan.txt 2>/dev/null; then
  cat /tmp/pocketdsh-process-scan.txt
  fail "process launch primitive detected in runtime C sources"
else
  pass "no process launch primitive in runtime C sources"
fi

if grep -RInE 'posix_spawn|popen\s*\(|Process\s*\(' PocketDSH --include='*.swift' >/tmp/pocketdsh-swift-process-scan.txt 2>/dev/null; then
  cat /tmp/pocketdsh-swift-process-scan.txt
  fail "process launch primitive detected in app sources"
else
  pass "no process launch primitive in app sources"
fi

if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  if git check-ignore -q PocketDSH.xcodeproj 2>/dev/null; then pass "generated Xcode project is ignored"; else fail "PocketDSH.xcodeproj must be generated"; fi
  if git check-ignore -q PocketDSH/Resources/Info.plist 2>/dev/null; then pass "generated Info.plist is ignored"; else fail "Info.plist must be generated"; fi
fi

if ((WITH_TESTS == 1)); then
  echo "-- SwiftPM core/runtime tests --"
  if command -v swift >/dev/null 2>&1; then
    swift test --package-path Packages/PocketDSHKit
  else
    fail "swift is required for --with-tests"
  fi
fi

echo
if ((failures > 0)); then echo "Local checks failed: ${failures} issue(s)."; exit 1; fi
echo "Local checks passed."
echo "Note: SwiftUI/Xcode compilation is authoritative and only runs on macOS."
