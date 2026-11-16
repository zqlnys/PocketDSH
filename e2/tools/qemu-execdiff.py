#!/usr/bin/env python3
"""Find the first basic block qemu reaches that this interpreter never does.

    env -i qemu-aarch64-static -d exec -D log BINARY ARGS
    tools/qemu-exectrace.py log > qemu.txt
    tools/rune ... E2_TRACE_PCS=ENTRY > mine.txt     (or any PC trace)
    tools/qemu-execdiff.py qemu.txt mine.txt

The gdb stub costs roughly a second per hundred instructions; a 30,000
instruction run takes an hour. The exec log is written as the emulator runs and
covers the same ground in seconds. It records block entries rather than every
instruction, so this is a subsequence test: it says which block is missing, and
the block is then disassembled by hand.

Two caveats learned the hard way, both of which produce false positives:

  * argv[0] must have the same shape on both sides. busybox scans it for a
    slash, so "busybox.static" and "/tmp/bbs/bin/busybox.static" take different
    branches - use E2_ARGV0 to match qemu.
  * block boundaries are not stable across implementations, so a block can be
    "missing" because it was translated as part of a larger one. Confirm with
    E2_TRACE_HIT before believing it.
"""
import sys


def main():
    if len(sys.argv) < 3:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    q = [l.strip() for l in open(sys.argv[1]) if l.strip()]
    m = [l.strip() for l in open(sys.argv[2]) if l.strip()]
    j = 0
    for i, pc in enumerate(q):
        found = -1
        for k in range(j, len(m)):
            if m[k] == pc:
                found = k
                break
        if found < 0:
            print("qemu block %d (pc=0x%s) not reached here" % (i, pc))
            print("  qemu around: %s" % q[max(0, i - 4):i + 1])
            print("  mine around: %s" % m[max(0, j - 4):j + 8])
            print("  confirm with E2_TRACE_HIT=0x%s before trusting this" % pc)
            return 1
        j = found + 1
    print("all %d qemu blocks appear in order" % len(q))
    return 0


if __name__ == "__main__":
    sys.exit(main())
