#!/usr/bin/env python3
"""Build a per-instruction guest trace from qemu's logs.

    env -i qemu-aarch64-static -d in_asm,exec -D log BINARY ARGS
    tools/qemu-fultrace.py log > qemu.txt

`-d exec` alone gives basic-block entry points, which is too coarse: block
boundaries are not stable between implementations, so a block that exists on
both sides can still look "missing" because it was translated as part of a
larger one. `-d in_asm` supplies each block's bytes, so the block entries can be
expanded back into individual instructions and compared one for one.
"""
import re
import sys

IN_ASM = re.compile(r"^0x([0-9a-f]+):\s*$")
BYTES = re.compile(r"^OBJD-T: ?([0-9a-f]*)$")
EXEC = re.compile(r"\[[0-9a-f]+/([0-9a-f]+)/")


def parse(path):
    blocks = {}
    order = []
    current = None
    pending = ""
    for line in open(path):
        line = line.strip()
        m = IN_ASM.match(line)
        if m:
            current = int(m.group(1), 16)
            pending = ""
            blocks.setdefault(current, [])
            continue
        m = BYTES.match(line)
        if m and current is not None:
            pending += m.group(1)
            continue
        if pending and current is not None:
            # Each instruction is eight hex digits, little endian in memory.
            pcs = blocks[current] = []
            for i in range(0, len(pending) - 7, 8):
                pcs.append(current + i // 2)
            current = None
            pending = ""
        m = EXEC.search(line)
        if m:
            order.append(int(m.group(1), 16))
    return blocks, order


def main():
    if len(sys.argv) < 2:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    blocks, order = parse(sys.argv[1])
    for start in order:
        pcs = blocks.get(start)
        if pcs:
            for pc in pcs:
                print("%x" % pc)
        else:
            print("%x" % start)
    return 0


if __name__ == "__main__":
    sys.exit(main())
