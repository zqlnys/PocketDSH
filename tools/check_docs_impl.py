#!/usr/bin/env python3
"""Compare the numbers the documentation claims against what the suites produce.

Three rounds in a row found documentation describing an older state: a README with no
mention of the backend at all, a handoff with completed milestones marked "not started",
and a features document listing finished work as unstarted. All of those are drift, and
all of them could have been caught by comparing the claim to a command's output.

The check is deliberately narrow. It knows where each headline number is written and what
produces it; anything phrased differently is left alone rather than guessed at.
"""

import os
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
    ("docs/HANDOFF.md", r"(?m)^\| 静态检查.*?\| (\d+) 类 \|", "static", "static checks"),
]


def actual(name: str) -> int | None:
    """The count for a suite, from the environment when the caller already knows it.

    verify.sh runs every suite and then calls this check, which used to run them all a
    second time to count them - so a full verification did the work twice, 21 minutes
    instead of 10. The caller now passes what it measured; running the suite here is only
    the fallback for a standalone invocation.
    """
    cached = os.environ.get("DSH_COUNT_" + name.upper())
    if cached is not None and cached.strip().isdigit():
        return int(cached)
    e2 = ROOT / "e2"
    busybox = "/tmp/bbs/bin/busybox.static"
    commands = {
        "unit": ["make", "test"],
        "m2": ["./tools/m2-sweep.sh", busybox, "./tools/rune", "40"],
        "m3": ["./tools/m3-sweep.sh", busybox, "./tools/rune", "40"],
        "diff": ["./tools/diff-test.sh", busybox, "./tools/rune"],
        # Counts the static checks by running them: seven indented result lines means
        # seven checks. Reading the script's own structure would break the moment the
        # output format changed.
        "static": ["sh", str(ROOT / "tools/check-swift.sh"), str(ROOT)],
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
    if name == "static":
        # The checker states its own count. Counting indented lines worked until a check
        # printed two of them, which changed the figure the documentation was compared
        # against without anything actually changing.
        m = re.search(r"SWIFT_CHECK_COUNT=(\d+)", out)
        return int(m.group(1)) if m else None
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
        # SESSION-SUMMARY is the same kind of document: "182 cases did not notice this" is a
        # statement about what was true then, not a claim about now.
        if path.name in ("E2-BACKEND.md", "SESSION-SUMMARY.md"):
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


def check_stated_total() -> int:
    """The "N 项验证" figure must equal the sum of the suites that make it up.

    It appears in README and at the top of E2-BACKEND, and it is the number a reader is
    most likely to quote. Adding a case without updating it was caught only by reading.
    """
    names = {"unit": "单指令测试", "m2": "M2", "m3": "M3", "diff": "差分"}
    total = 0
    for key in names:
        value = actual(key)
        if value is None:
            print(f"  could not determine {names[key]}, skipping the total")
            return 0
        total += value

    stale = []
    # E2-BACKEND is chronological; a "N 项验证" in its middle chapters is a snapshot of
    # what was true then, and flagging it would be flagging correct history.
    for name in ("README.md",):
        path = ROOT / name
        if not path.exists():
            continue
        text = path.read_text(errors="replace")
        for m in re.finditer(r"(\d+) 项验证", text):
            if int(m.group(1)) != total:
                line = text[:m.start()].count("\n") + 1
                stale.append(f"{name}:{line} says {m.group(1)} 项验证, suites total {total}")
    if stale:
        for item in stale:
            print("  " + item)
        return 1
    print(f"  stated total {total} matches the suites")
    return 0


def check_boolean_claims() -> int:
    """Claims of the form "no overlaps" and "0 out of family" are checkable too.

    They are the kind of statement that stays true for a long time and then quietly stops
    being true when a decode rule is added.
    """
    e2 = ROOT / "e2"
    problems = []

    try:
        out = subprocess.run(["./tools/check-rule-mnemonics.py",
                              "/tmp/bbs/bin/busybox.static"],
                             cwd=e2, capture_output=True, text=True, timeout=600).stdout
    except (OSError, subprocess.TimeoutExpired):
        out = ""
    m = re.search(r"(\d+) rule\(s\) match instructions outside their family", out)
    words = re.search(r"(\d+) unique words", out)
    if m is None:
        print("  rule-mnemonics did not report a count - not checked")
    else:
        if int(m.group(1)) != 0:
            problems.append(f"rule-mnemonics reports {m.group(1)} out of family")
        if words is not None:
            doc = (ROOT / "docs/E2-BACKEND.md").read_text(errors="replace")
            claimed = re.search(r"(\d+) 个唯一指令字", doc)
            if claimed and int(claimed.group(1)) != int(words.group(1)):
                problems.append(f"unique words: doc says {claimed.group(1)}, "
                                f"actual {words.group(1)}")

    try:
        out = subprocess.run(["make", "overlaps"], cwd=e2, capture_output=True,
                             text=True, timeout=600).stdout
    except (OSError, subprocess.TimeoutExpired):
        out = ""
    if "no overlaps" in out:
        pass
    elif "skip" in out:
        print("  overlaps skipped - not checked")
    else:
        problems.append("decode rules overlap")

    # The family check covers every guard; the old one covered 29 of 70.
    try:
        out = subprocess.run(["python3", "tools/check-decode-families.py",
                              "/tmp/bbs/bin/busybox.static"],
                             cwd=e2, capture_output=True, text=True, timeout=600).stdout
    except (OSError, subprocess.TimeoutExpired):
        out = ""
    fam = re.search(r"(\d+) guards, (\d+) matching outside", out)
    if fam is None:
        print("  decode families did not report - not checked")
    else:
        if int(fam.group(2)) != 0:
            problems.append(f"{fam.group(2)} guard(s) match outside their set")
        doc = (ROOT / "docs/E2-BACKEND.md").read_text(errors="replace")
        claimed = re.search(r"\*\*(\d+)/(\d+) 条\*\*", doc)
        if claimed and int(claimed.group(1)) != int(fam.group(1)):
            problems.append(f"guard coverage: doc says {claimed.group(1)}, "
                            f"actual {fam.group(1)}")

    if problems:
        for item in problems:
            print("  " + item)
        return 1
    print("  no rule out of family, no guard outside its set, no decode overlap")
    return 0


def check_readme_tool_list() -> int:
    """Every declared tool should be named in the README.

    It listed six of seventeen under "real Agent tools", which reads as the complete set to
    anyone who does not open the catalogue.
    """
    models = ROOT / "Packages/PocketDSHKit/Sources/PocketDSHCore/AgentModels.swift"
    readme = ROOT / "README.md"
    if not models.exists():
        print("  models not found, skipping")
        return 0
    names = set(re.findall(r'static let \w+ = "([a-z_]+)"', models.read_text()))
    text = readme.read_text(errors="replace")
    missing = sorted(n for n in names if f"`{n}`" not in text)
    if missing:
        print("  README does not name: " + ", ".join(missing))
        return 1
    print(f"  all {len(names)} tools named in the README")
    return 0


def check_readme_test_counts() -> int:
    """The per-suite test counts in the README should match the files.

    PocketDSHTests was missing from that list entirely - 26 tests, the largest suite, while
    the two smaller ones were each counted out.
    """
    readme = ROOT / "README.md"
    text = readme.read_text(errors="replace")
    suites = {
        "PocketDSHCore": [ROOT / "Packages/PocketDSHKit/Tests/PocketDSHCoreTests"],
        "LuaRuntime": [ROOT / "Packages/PocketDSHKit/Tests/LuaRuntimeTests"],
        "PocketDSHTests": [ROOT / "PocketDSHTests"],
    }
    problems = []
    for name, dirs in suites.items():
        actual = 0
        for d in dirs:
            if not d.exists():
                # The app test target is generated by XcodeGen and may not be present in
                # every checkout; a missing directory is not a count mismatch.
                actual = None
                break
            for f in d.glob("*.swift"):
                actual += len(re.findall(r"func test\w+\s*\(", f.read_text(errors="replace")))
        if actual is None:
            continue
        m = re.search(r"`" + name + r"`[^\n]*?(\d+) 个测试", text)
        if m is None:
            problems.append(f"README does not state a count for {name} ({actual} tests)")
        elif int(m.group(1)) != actual:
            problems.append(f"{name}: README says {m.group(1)}, actual {actual}")

    if problems:
        for item in problems:
            print("  " + item)
        return 1
    # The status table quotes a single total; it must be the sum of all three suites.
    backend = (ROOT / "docs/E2-BACKEND.md").read_text(errors="replace")
    total_claimed = re.search(r"\| iOS 测试 \| \*\*(\d+) 项", backend)
    if total_claimed:
        grand = 0
        for name in ("PocketDSHCore", "LuaRuntime", "PocketDSHTests"):
            dirs = {"PocketDSHCore": [ROOT / "Packages/PocketDSHKit/Tests/PocketDSHCoreTests"],
                    "LuaRuntime": [ROOT / "Packages/PocketDSHKit/Tests/LuaRuntimeTests"],
                    "PocketDSHTests": [ROOT / "PocketDSHTests"]}[name]
            for d in dirs:
                if d.exists():
                    for f in d.glob("*.swift"):
                        grand += len(re.findall(r"func test\w+\s*\(", f.read_text(errors="replace")))
        if int(total_claimed.group(1)) != grand:
            print(f"  E2-BACKEND says {total_claimed.group(1)} iOS tests, files hold {grand}")
            return 1
    print("  per-suite test counts match the files")
    return 0


def check_doc_index() -> int:
    """Every document in docs/ should be linked from the README.

    EXECUTION-PROBE.md was not - the document that establishes why E2 has to exist at all.
    A reader following the README's index would never have found the premise.
    """
    docs = ROOT / "docs"
    readme = ROOT / "README.md"
    if not docs.exists():
        print("  docs/ not found, skipping")
        return 0
    on_disk = {f.name for f in docs.glob("*.md")}
    linked = set(re.findall(r"docs/([A-Za-z0-9._-]+\.md)", readme.read_text(errors="replace")))
    missing = sorted(on_disk - linked)
    if missing:
        print("  not linked from the README: " + ", ".join(missing))
        return 1
    print(f"  all {len(on_disk)} document(s) linked from the README")
    return 0


def check_e2_tool_mentions() -> int:
    """Every tool under e2/tools/ should be mentioned somewhere in the documentation.

    Four were not, including three trace helpers - the ones a person reaches for when a
    comparison disagrees and they need to see what each side actually executed.
    """
    tools_dir = ROOT / "e2/tools"
    if not tools_dir.exists():
        print("  e2/tools not found, skipping")
        return 0
    docs_text = ""
    for name in ("README.md", "docs/HANDOFF.md", "docs/E2-BACKEND.md"):
        path = ROOT / name
        if path.exists():
            docs_text += path.read_text(errors="replace")
    skip = {"__pycache__", "rune", "rune.c"}
    missing = sorted(f.name for f in tools_dir.iterdir()
                     if f.name not in skip and f.name not in docs_text)
    if missing:
        print("  not mentioned in the documentation: " + ", ".join(missing))
        return 1
    print("  every e2/tools entry is mentioned somewhere")
    return 0


def check_synthesis_linked() -> int:
    """The handoff should point at the failure-mode summary.

    It is the highest-value section in a 8000-line document and the one a new session is least
    likely to find on its own.
    """
    handoff = ROOT / "docs/HANDOFF.md"
    backend = ROOT / "docs/E2-BACKEND.md"
    if not handoff.exists() or not backend.exists():
        print("  documents not found, skipping")
        return 0
    title = "经验总结"
    if title not in backend.read_text(errors="replace"):
        print(f"  E2-BACKEND no longer contains a {title} section")
        return 1
    if title not in handoff.read_text(errors="replace"):
        print(f"  HANDOFF does not point at the {title} section")
        return 1
    print("  the handoff points at the failure-mode summary")
    return 0


def check_capability_card() -> int:
    """The settings card's "mapped now" list must name tools that exist.

    It used to be a sentence, and it said "Not yet: subagents" long after subagent worked -
    the app told the user a capability was missing while the model could call it. As data it
    can be compared against the declarations.
    """
    models = ROOT / "Packages/PocketDSHKit/Sources/PocketDSHCore/AgentModels.swift"
    if not models.exists():
        print("  models not found, skipping")
        return 0
    text = models.read_text(errors="replace")
    declared = set(re.findall(r'static let \w+ = "([a-z_]+)"', text))
    block = re.search(r"mappedNow = \[(.*?)\]", text, re.S)
    if block is None:
        print("  no mappedNow list found")
        return 1
    claimed = set(re.findall(r'"([a-z_]+)"', block.group(1)))
    phantom = sorted(claimed - declared)
    if phantom:
        print("  the card claims tools that do not exist: " + ", ".join(phantom))
        return 1
    print(f"  the settings card names {len(claimed)} real tools")
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
    stated = check_stated_total()
    booleans = check_boolean_claims()
    readme_tools = check_readme_tool_list()
    readme_tests = check_readme_test_counts()
    doc_index = check_doc_index()
    e2_tools = check_e2_tool_mentions()
    synthesis = check_synthesis_linked()
    card = check_capability_card()
    if problems:
        print(f"  ^ {len(problems)} of {checked} claims are out of date")
        return 1
    # Name what failed. This used to return 1 without a summary, so the only trace was
    # whichever check had printed a line - and a line phrased as a statement ("SESSION-SUMMARY
    # says 182 differential") does not look like an error when you are grepping for "fail".
    named = [
        (name, value) for name, value in (
            ("stale counts", stray), ("headings", headings), ("tool coverage", covered),
            ("stated total", stated), ("boolean claims", booleans),
            ("README tool list", readme_tools), ("README test counts", readme_tests),
            ("document index", doc_index), ("e2/tools mentions", e2_tools),
            ("synthesis link", synthesis),
        ) if value != 0
    ]
    if named:
        print("check-docs: " + ", ".join(name for name, _ in named) + " failed")
        return 1
    print(f"  {checked} claim(s) match the suites")
    return 0


if __name__ == "__main__":
    sys.exit(main())
