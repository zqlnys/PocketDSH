#!/usr/bin/env python3
"""Choose an available iPhone simulator with iOS 17 or newer."""

from __future__ import annotations

import json
import re
import subprocess
import sys


def runtime_version(identifier: str) -> tuple[int, int] | None:
    match = re.search(r"iOS-(\d+)-(\d+)", identifier)
    if not match:
        return None
    return int(match.group(1)), int(match.group(2))


def main() -> int:
    try:
        raw = subprocess.check_output(
            ["xcrun", "simctl", "list", "devices", "available", "--json"],
            text=True,
        )
        payload = json.loads(raw)
    except (OSError, subprocess.CalledProcessError, json.JSONDecodeError) as error:
        print(f"Unable to list simulators: {error}", file=sys.stderr)
        return 1

    candidates = []
    for runtime, devices in payload.get("devices", {}).items():
        version = runtime_version(runtime)
        if version is None or version < (17, 0):
            continue

        for device in devices:
            if not device.get("isAvailable", True):
                continue
            name = str(device.get("name", ""))
            udid = device.get("udid")
            if "iPhone" not in name or not udid:
                continue

            generation_match = re.search(r"iPhone\s+(\d+)", name)
            generation = int(generation_match.group(1)) if generation_match else 0
            pro_bonus = 1 if "Pro" in name else 0
            candidates.append((version, generation, pro_bonus, name, udid))

    if not candidates:
        print("No available iPhone simulator running iOS 17 or newer was found.", file=sys.stderr)
        return 1

    candidates.sort(reverse=True)
    version, _, _, name, udid = candidates[0]
    print(
        f"Selected simulator: {name} (iOS {version[0]}.{version[1]}) {udid}",
        file=sys.stderr,
    )
    print(udid)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
