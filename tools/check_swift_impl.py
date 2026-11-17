#!/usr/bin/env python3
"""Static consistency checks for the Swift sources.

Swift cannot be compiled in this checkout, so these stand in for the first half of
what a build would catch: an edit that unbalances braces, a tool name referenced
without being declared, and a test constructing a model with a label it does not
have. They are deliberately narrow - anything closer to type checking would give
false confidence.
"""

import pathlib
import re
import subprocess
import sys


def strip(text: str) -> str:
    """Remove comments and string literals so braces can be counted."""
    text = re.sub(r'"""[\s\S]*?"""', '""', text)
    text = re.sub(r"//[^\n]*", "", text)
    text = re.sub(r'"[^"\n]*"', '""', text)
    return text


def balance(text: str) -> int:
    stripped = strip(text)
    return stripped.count("{") - stripped.count("}")


def repo_root(start: pathlib.Path) -> pathlib.Path | None:
    out = subprocess.run(
        ["git", "-C", str(start), "rev-parse", "--show-toplevel"],
        capture_output=True, text=True).stdout.strip()
    return pathlib.Path(out) if out else None


def check_braces(root: pathlib.Path) -> bool:
    """Compare each file's brace balance against its committed version.

    Comparing against zero would flag files whose quoting defeats a text-level count.
    What matters is whether an edit changed the balance, not whether the crude count
    lands on zero.
    """
    top = repo_root(root)
    if top is None:
        print("  not a git checkout, skipping")
        return True
    bad = 0
    for path in root.rglob("*.swift"):
        if ".build" in str(path):
            continue
        try:
            rel = path.relative_to(top)
        except ValueError:
            continue
        head = subprocess.run(["git", "-C", str(top), "show", f"HEAD:{rel}"],
                              capture_output=True, text=True).stdout
        if not head:
            continue
        now, was = balance(path.read_text(errors="replace")), balance(head)
        if now != was:
            print(f"  {rel}: balance {was:+d} -> {now:+d}")
            bad += 1
    if bad:
        return False
    print("  no file changed its balance")
    return True


def check_tool_names(root: pathlib.Path) -> bool:
    models = (root / "Packages/PocketDSHKit/Sources/PocketDSHCore/AgentModels.swift")
    if not models.exists():
        print("  AgentModels.swift not found, skipping")
        return True
    declared = set(re.findall(r'static let (\w+) = "', models.read_text()))
    used = set()
    for path in root.rglob("*.swift"):
        if ".build" in str(path):
            continue
        used |= set(re.findall(r"ToolNames\.(\w+)", path.read_text(errors="replace")))
    missing = sorted(used - declared)
    if missing:
        print("  used but not declared:", ", ".join(missing))
        return False
    print(f"  {len(used)} used, all declared of {len(declared)}")
    return True


def init_labels(models: str, type_name: str) -> set[str] | None:
    """Labels accepted by `type_name`'s initialiser, or None if it has no public init."""
    match = re.search(rf"public struct {type_name}\b[^{{]*\{{", models)
    if match is None:
        return None
    start = match.end()
    depth, i = 1, start
    while i < len(models) and depth:
        if models[i] == "{":
            depth += 1
        elif models[i] == "}":
            depth -= 1
        i += 1
    body = models[start:i]
    init = re.search(r"public init\(", body)
    if init is None:
        return None
    # Default values contain their own parentheses (UUID(), .now), so the parameter
    # list has to be matched by depth rather than by the first closing paren.
    j, depth2 = init.end(), 1
    while j < len(body) and depth2:
        if body[j] == "(":
            depth2 += 1
        elif body[j] == ")":
            depth2 -= 1
        j += 1
    params = body[init.end():j - 1]
    return set(re.findall(r"(\w+)\s*:", params))


def check_test_labels(root: pathlib.Path) -> bool:
    """Every label a test passes must be a parameter of that type's own initialiser.

    Checking only that the label appears somewhere in the models is too weak: a typo
    that happens to exist on a different type would pass, and that is exactly the
    mistake this is meant to catch.
    """
    models = ""
    for path in (root / "Packages/PocketDSHKit/Sources/PocketDSHCore").glob("*.swift"):
        models += path.read_text(errors="replace")
    tests = root / "PocketDSHTests/PocketDSHTests.swift"
    if not tests.exists():
        print("  tests not found, skipping")
        return True
    text = tests.read_text(errors="replace")
    checked = 0
    problems = []
    for type_name in ("TodoItem", "AgentEvent", "LLMMessage"):
        accepted = init_labels(models, type_name)
        if accepted is None:
            continue
        for label in sorted(set(re.findall(rf"\b{type_name}\((\w+):", text))):
            checked += 1
            if label not in accepted:
                problems.append(f"{type_name}({label}:) not in init {sorted(accepted)}")
    if problems:
        for line in problems:
            print("  " + line)
        return False
    print(f"  {checked} labels checked against their own initialisers")
    return True


def check_tool_handling(root: pathlib.Path) -> bool:
    """Every declared tool must be handled somewhere.

    Most go through ToolExecutor's switch, but a few need the agent's own context -
    subagent needs the API key and the nesting depth - and are intercepted before the
    executor sees them. Either place counts; a tool declared in the catalogue and
    handled in neither is a tool the model can call and nothing will answer.
    """
    models = (root / "Packages/PocketDSHKit/Sources/PocketDSHCore/AgentModels.swift")
    if not models.exists():
        print("  AgentModels.swift not found, skipping")
        return True
    declared = set(re.findall(r'static let (\w+) = "', models.read_text()))
    handled = set()
    for path in (root / "PocketDSH").rglob("*.swift"):
        text = path.read_text(errors="replace")
        handled |= set(re.findall(r"ToolNames\.(\w+)", text))
    missing = sorted(declared - handled)
    if missing:
        print("  declared but never handled:", ", ".join(missing))
        return False
    print(f"  {len(declared)} tools declared, all handled")
    return True


def check_tool_specs(root: pathlib.Path) -> bool:
    """Every declared tool must have a catalogue entry, and vice versa.

    A tool the model is told about only through the catalogue needs a spec there; one
    with a spec but no declared name would be reached by a string that nothing defines.
    """
    models = (root / "Packages/PocketDSHKit/Sources/PocketDSHCore/AgentModels.swift")
    catalog = root / "PocketDSH/Core/Services/ToolCatalog.swift"
    if not models.exists() or not catalog.exists():
        print("  models or catalogue not found, skipping")
        return True
    declared = set(re.findall(r'static let (\w+) = "', models.read_text()))
    spec = set(re.findall(r"name: ToolNames\.(\w+)", catalog.read_text(errors="replace")))
    problems = []
    if declared - spec:
        problems.append("declared without a spec: " + ", ".join(sorted(declared - spec)))
    if spec - declared:
        problems.append("spec without a declaration: " + ", ".join(sorted(spec - declared)))
    if problems:
        for line in problems:
            print("  " + line)
        return False
    print(f"  {len(declared)} tools, spec and declaration agree")
    return True


def main() -> int:
    root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
    results = [
        ("tool names", check_tool_names(root)),
        ("tool handling", check_tool_handling(root)),
        ("tool specs", check_tool_specs(root)),
        ("test labels", check_test_labels(root)),
        ("brace balance", check_braces(root)),
    ]
    for name, ok in results:
        if not ok:
            print(f"  ^ {name}")
    return 0 if all(ok for _, ok in results) else 1


if __name__ == "__main__":
    sys.exit(main())
