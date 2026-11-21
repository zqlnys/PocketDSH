#!/usr/bin/env python3
"""Find where two PC traces first stop agreeing, after normalising load addresses.

qemu-user maps libpython at guest_base + 0x8e5000 while this interpreter maps it at
guest_base, so the same code has different absolute addresses on each side. Subtracting each
side's own libpython base makes them comparable, which is what turns "the registers differ"
into "this instruction is where they diverge".

    tools/trace-diverge.py --qemu-base 0x4000008e5000 qemu.txt \
                           --mine-base 0x4000000000 mine.txt
"""
import argparse
import sys


def offsets(path, base):
    out = []
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                pc = int(line, 16)
            except ValueError:
                continue
            if pc >= base:
                out.append(pc - base)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu-base", required=True, type=lambda s: int(s, 0))
    ap.add_argument("--mine-base", required=True, type=lambda s: int(s, 0))
    ap.add_argument("--window", type=int, default=0, help="skip this many entries first")
    ap.add_argument("--limit", type=int, default=0, help="compare at most this many")
    ap.add_argument("qemu")
    ap.add_argument("mine")
    args = ap.parse_args()

    q = offsets(args.qemu, args.qemu_base)
    m = offsets(args.mine, args.mine_base)
    if args.window:
        q, m = q[args.window:], m[args.window:]
    if args.limit:
        q, m = q[:args.limit], m[:args.limit]
    print("qemu entries: %d   mine entries: %d" % (len(q), len(m)))

    # Walk both, allowing qemu to skip: its exec log records block entries, mine records
    # every instruction, so a qemu entry is expected to appear somewhere ahead in mine.
    i = j = 0
    skipped = 0
    while i < len(q) and j < len(m):
        if q[i] == m[j]:
            i += 1
            j += 1
            continue
        # Advance mine until qemu's block entry shows up; that is normal (extra detail).
        k = j
        while k < len(m) and k - j < 4096 and m[k] != q[i]:
            k += 1
        if k < len(m) and m[k] == q[i]:
            skipped += k - j
            j = k
            continue
        print("first divergence at qemu entry %d / mine entry %d" % (i, j))
        print("  qemu expects 0x%x" % q[i])
        print("  mine has     0x%x" % m[j])
        lo = max(0, i - 6)
        print("  qemu context: " + " ".join("0x%x" % v for v in q[lo:i + 3]))
        lo2 = max(0, j - 6)
        print("  mine context: " + " ".join("0x%x" % v for v in m[lo2:j + 3]))
        return 1
    print("traces agree for the compared range (mine skipped %d extra entries)" % skipped)
    return 0


if __name__ == "__main__":
    sys.exit(main())
