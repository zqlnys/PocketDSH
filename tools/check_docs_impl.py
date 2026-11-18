#!/usr/bin/env python3
"""Compare the numbers the documentation claims against what the suites produce.

Three rounds in a row found documentation describing an older state: a README with no
mention of the backend at all, a handoff with completed milestones marked "not started",
and a features document listing finished work as unstarted. All of those are drift, and
all of them could have been caught by comparing the claim to a command's output.

The check is deliberately narrow. It knows where each headline number is written and what
produces it; anything phrased differently is left alone rather than guessed at.
"""

import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

# (document, pattern capturing the number, command producing it, label)
CLAIMS = [
    ("README.md", r"(?m)^\| \*\*E2 aarch64 后端\*\*.*?(\d+) 项单指令测试", "unit", "unit checks"),
    ("README.md", r"(?m)^\| \*\*E2 aarch64 后端\*\*.*?(\d+) 条 M2", "m2", "M2 cases"),
    ("README.md", r"(?m)^\| \*\*E2 aarch64 后端\*\*.*?(\d+) 条 M3", "m3", "M3 cases"),
    ("README.md", r"(?m)^\| \*\*E2 aarch64 后端\*\*.*?(\d+) 条差分", "diff", "differential cases"),
    ("docs/HANDOFF.md", r"(?m)^\| 单指令测试 \| (\d+) \|", "unit", "unit checks"),
    ("docs/HANDOFF.md", r"(?m)^\| M2 扫描.*?\| (\d+) \|", "m2", "M2 cases"),
    ("docs/HANDOFF.md", r"(?m)^\| M3 扫描.*?\| (\d+) \|", "m3", "M3 cases"),
    ("docs/HANDOFF.md", r"(?m)^\| 差分对比.*?\| (\d+) \|", "diff", "differential cases"),
]


def actual(name: str) -> int | None:
    """Runs the suite and reads its count. None when it cannot be determined."""
    e2 = ROOT / "e2"
    busybox = "/tmp/bbs/bin/busybox.static"
    commands = {
        "unit": ["make", "test"],
        "m2": ["./tools/m2-sweep.sh", busybox, "./tools/rune", "40"],
        "m3": ["./tools/m3-sweep.sh", busybox, "./tools/rune", "40"],
        "diff": ["./tools/diff-test.sh", busybox, "./tools/rune"],
    }
    try:
        out = subprocess.run(commands[name], cwd=e2, capture_output=True, text=True,
                             timeout=900).stdout
    except (OSError, subprocess.TimeoutExpired):
        return None
    patterns = {
        "unit": r"(\d+) checks",
        "m2": r"M2 sweep: (\d+) passed",
        "m3": r"M3 sweep: (\d+) passed",
        "diff": r"diff test: (\d+) same",
    }
    m = re.search(patterns[name], out)
    return int(m.group(1)) if m else None


def check_tool_coverage() -> int:
    """Every declared tool name must appear somewhere in the compatibility document.

    The identifier and the wire name differ (`webSearch` is `web_search`), and the
    document writes the wire name - so this compares the strings the model actually sees.
    """
    models = ROOT / "Packages/PocketDSHKit/Sources/PocketDSHCore/AgentModels.swift"
    doc = ROOT / "docs/DSH-COMPATIBILITY.md"
    if not models.exists() or not doc.exists():
        print("  models or compatibility doc not found, skipping")
        return 0
    names = re.findall(r'static let \w+ = "([a-z_]+)"', models.read_text())
    text = doc.read_text(errors="replace")
    missing = [n for n in sorted(set(names)) if n not in text]
    if missing:
        print("  tools not described in DSH-COMPATIBILITY.md: " + ", ".join(missing))
        return 1
    print(f"  {len(set(names))} tools all described")
    return 0


def check_no_duplicate_headings() -> int:
    """A repeated heading means two sections claiming the same role.

    The status section was once duplicated, so a reader scrolling to it landed on a
    pointer to the real one rather than the table itself.
    """
    doc = ROOT / "docs/E2-BACKEND.md"
    if not doc.exists():
        return 0
    seen: dict[str, int] = {}
    dupes = []
    for line in doc.read_text(errors="replace").split("\n"):
        if line.startswith("## "):
            seen[line] = seen.get(line, 0) + 1
            if seen[line] == 2:
                dupes.append(line[3:])
    if dupes:
        print("  repeated headings in E2-BACKEND.md: " + ", ".join(dupes))
        return 1
    print(f"  {len(seen)} headings, none repeated")
    return 0


def check_any_stale_counts() -> int:
    """Look for count phrasings anywhere in the docs and refuse ones that do not match.

    The fixed CLAIMS list only covers phrasings that were there when it was written. A
    number written a hundred rounds ago in a different sentence - "182 条差分" inside a
    paragraph rather than in a table row - would be missed. This scans for the phrasings
    and checks every occurrence.
    """
    patterns = [
        (r"(\d+) 条差分", "diff", "differential"),
        (r"(\d+) 条 M2", "m2", "M2"),
        (r"(\d+) 条 M3", "m3", "M3"),
        (r"(\d+) 项单指令测试", "unit", "unit"),
        (r"单指令测试 \| \*\*(\d+) 项", "unit", "unit"),
    ]
    cache: dict[str, int | None] = {}
    stale = []
    for path in sorted((ROOT / "docs").glob("*.md")) + [ROOT / "README.md"]:
        if not path.exists():
            continue
        # E2-BACKEND.md is a chronological record and says so at the top: the numbers in
        # its middle chapters are snapshots of what was true then. Scanning it here would
        # flag correct history as wrong. Its current-state table is covered by CLAIMS.
        if path.name == "E2-BACKEND.md":
            continue
        text = path.read_text(errors="replace")
        for pattern, suite, label in patterns:
            for m in re.finditer(pattern, text):
                claimed = int(m.group(1))
                if suite not in cache:
                    cache[suite] = actual(suite)
                got = cache[suite]
                if got is not None and got != claimed:
                    line = text[:m.start()].count("\n") + 1
                    stale.append(f"{path.name}:{line} says {claimed} {label}, actual {got}")
    if stale:
        for item in stale[:10]:
            print("  " + item)
        print(f"  ^ {len(stale)} stale count(s) anywhere in the docs")
        return 1
    print("  no stale counts found outside the declared claims")
    return 0


def main() -> int:
    cache: dict[str, int | None] = {}
    problems = []
    checked = 0
    for document, pattern, suite, label in CLAIMS:
        path = ROOT / document
        if not path.exists():
            continue
        m = re.search(pattern, path.read_text(errors="replace"))
        if not m:
            # Not a failure: the wording may have changed shape. Say so rather than
            # pretending the document was checked.
            print(f"  {document}: no claim found for {label} - not checked")
            continue
        claimed = int(m.group(1))
        if suite not in cache:
            cache[suite] = actual(suite)
        got = cache[suite]
        checked += 1
        if got is None:
            print(f"  {document}: {label} claims {claimed}, suite did not report a count")
            problems.append(label)
        elif got != claimed:
            print(f"  {document}: {label} claims {claimed}, actual is {got}")
            problems.append(label)
        else:
            print(f"  {document}: {label} {claimed} matches")
    stray = check_any_stale_counts()
    headings = check_no_duplicate_headings()
    covered = check_tool_coverage()
    if problems:
        print(f"  ^ {len(problems)} of {checked} claims are out of date")
        return 1
    if covered != 0 or headings != 0 or stray != 0:
        return 1
    print(f"  {checked} claim(s) match the suites")
    return 0


if __name__ == "__main__":
    sys.exit(main())
