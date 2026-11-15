#!/usr/bin/env python3
"""Turn raw xcodebuild logs into a compact Markdown CI report."""

from __future__ import annotations

import datetime as dt
import re
import sys
from pathlib import Path

ERROR_PATTERN = re.compile(r"(?:^|\s)(?:error|fatal error):\s*(.+)$", re.IGNORECASE)
WARNING_PATTERN = re.compile(r"(?:^|\s)warning:\s*(.+)$", re.IGNORECASE)
FAILURE_PATTERN = re.compile(
    r"(Test Case '.*' failed|\*\* (?:TEST|BUILD) FAILED \*\*|Testing failed)",
    re.IGNORECASE,
)


def unique_lines(lines: list[str], limit: int = 30) -> list[str]:
    seen: set[str] = set()
    output: list[str] = []
    for line in lines:
        clean = line.strip()
        if not clean or clean in seen:
            continue
        seen.add(clean)
        output.append(clean)
        if len(output) >= limit:
            break
    return output


def code_block(lines: list[str]) -> str:
    if not lines:
        return "_None found._\n"
    return "~~~text\n" + "\n".join(lines) + "\n~~~\n"


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: summarize_logs.py <log-dir> <output.md>", file=sys.stderr)
        return 2

    log_dir = Path(sys.argv[1])
    output_path = Path(sys.argv[2])
    log_files = sorted(log_dir.glob("*.log")) if log_dir.exists() else []

    errors: list[str] = []
    warnings: list[str] = []
    failures: list[str] = []
    total_lines = 0

    for log_file in log_files:
        try:
            text = log_file.read_text(errors="replace")
        except OSError:
            continue
        for line in text.splitlines():
            total_lines += 1
            if ERROR_PATTERN.search(line):
                errors.append(line)
            if WARNING_PATTERN.search(line):
                warnings.append(line)
            if FAILURE_PATTERN.search(line):
                failures.append(line)

    error_lines = unique_lines(errors)
    warning_lines = unique_lines(warnings)
    failure_lines = unique_lines(failures)

    report = [
        "# Orbit build log summary",
        "",
        f"Generated: {dt.datetime.now(dt.timezone.utc).strftime('%Y-%m-%d %H:%M:%S UTC')}",
        "",
        "| Signal | Count |",
        "| --- | ---: |",
        f"| Log files | {len(log_files)} |",
        f"| Scanned lines | {total_lines} |",
        f"| Compiler/test errors | {len(errors)} |",
        f"| Warnings | {len(warnings)} |",
        f"| Failure markers | {len(failures)} |",
        "",
        "## Errors",
        "",
        code_block(error_lines),
        "## Warnings",
        "",
        code_block(warning_lines),
        "## Test / build failure markers",
        "",
        code_block(failure_lines),
    ]

    if len(errors) > len(error_lines):
        report.append(f"_Showing the first {len(error_lines)} unique errors out of {len(errors)}._")
    if len(warnings) > len(warning_lines):
        report.append(f"_Showing the first {len(warning_lines)} unique warnings out of {len(warnings)}._")

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text("\n".join(report) + "\n")
    print(f"Wrote {output_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
