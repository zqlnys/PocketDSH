#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DAYS=14
APPLY=0

while (($# > 0)); do
  case "$1" in
    --days)
      DAYS="${2:?--days requires a number}"
      shift 2
      ;;
    --apply)
      APPLY=1
      shift
      ;;
    *)
      echo "Unknown argument: $1" >&2
      echo "usage: cleanup_local.sh [--days N] [--apply]" >&2
      exit 2
      ;;
  esac
done

if ! [[ "$DAYS" =~ ^[0-9]+$ ]] || ((DAYS < 1)); then
  echo "--days must be a positive integer" >&2
  exit 2
fi

targets=(
  "build"
  "artifacts"
  "Packages/PocketDSHKit/.build"
  "scripts/__pycache__"
)

echo "Orbit cleanup"
echo "Project: $ROOT"
echo "Older than: $DAYS days"
if ((APPLY == 1)); then
  echo "Mode: APPLY (files will be deleted)"
else
  echo "Mode: DRY RUN (nothing will be deleted)"
fi
echo

for relative in "${targets[@]}"; do
  target="$ROOT/$relative"

  case "$target" in
    "$ROOT"/*) ;;
    *)
      echo "Refusing to operate outside project root: $target" >&2
      exit 1
      ;;
  esac

  if [[ ! -d "$target" ]]; then
    echo "skip  $relative (not present)"
    continue
  fi

  count="$(find "$target" -type f -mtime +"$DAYS" | wc -l | tr -d ' ')"
  if ((count == 0)); then
    echo "clean $relative (nothing older than $DAYS days)"
    continue
  fi

  if ((APPLY == 1)); then
    echo "purge $relative ($count file(s))"
    find "$target" -type f -mtime +"$DAYS" -print -delete
    find "$target" -type d -empty -delete 2>/dev/null || true
  else
    echo "would purge $relative ($count file(s)):"
    find "$target" -type f -mtime +"$DAYS" -print
  fi
done

echo
if ((APPLY == 0)); then
  echo "Dry run complete. Re-run with --apply after reviewing the list."
else
  echo "Cleanup complete. Source, Git history and XcodeGen input were not touched."
fi
