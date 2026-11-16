#!/usr/bin/env python3
"""Turn qemu's `-d exec` log into a guest-PC sequence.

    env -i qemu-aarch64-static -d exec -D log BINARY ARGS
    tools/qemu-exectrace.py log > qemu.txt

Single-stepping through the gdb stub costs about a second per hundred
instructions, which makes comparing a 30,000-instruction run take an hour. The
exec log is written as the interpreter runs and covers the same ground in
seconds. It records basic-block entries rather than every instruction, so the
comparison is a subsequence test rather than equality - which still localises a
divergence to one block.
"""
import re
import sys

LINE = re.compile(r"\[[0-9a-f]+/([0-9a-f]+)/")


def main():
    if len(sys.argv) < 2:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    for line in open(sys.argv[1]):
        m = LINE.search(line)
        if m:
            print(m.group(1).lstrip("0") or "0")
    return 0


if __name__ == "__main__":
    sys.exit(main())
