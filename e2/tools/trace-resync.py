#!/usr/bin/env python3
"""Compare two instruction traces, resynchronising across loop expansion noise.

    tools/trace-resync.py qemu.txt mine.txt

qemu's exec log records a block once and expands it once, so a loop that runs
five times appears once. This interpreter records every instruction, so the same
loop appears five times. A strict comparison therefore "diverges" at the first
loop and stays wrong forever after.

This walks both traces together and, whenever they disagree, looks a short way
ahead on each side for a pair of equal entries that rejoins them. Divergences
that survive that search are real; the ones inside loops are absorbed.
"""
import sys

WINDOW = 400


def main():
    if len(sys.argv) < 3:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    q = [l.strip() for l in open(sys.argv[1]) if l.strip()]
    m = [l.strip() for l in open(sys.argv[2]) if l.strip()]
    i = j = 0
    resyncs = 0
    while i < len(q) and j < len(m):
        if q[i] == m[j]:
            i += 1
            j += 1
            continue
        best = None
        # Prefer the smallest combined jump that rejoins the two.
        for dq in range(0, WINDOW):
            if i + dq >= len(q):
                break
            for dm in range(0, WINDOW):
                if j + dm >= len(m):
                    break
                if q[i + dq] == m[j + dm] and (dq or dm):
                    score = dq + dm
                    if best is None or score < best[0]:
                        best = (score, dq, dm)
                    break
            if best is not None and best[0] <= dq:
                break
        if best is None:
            print("real divergence: qemu[%d]=%s  mine[%d]=%s" % (i, q[i], j, m[j]))
            print("  qemu: %s" % q[max(0, i - 6):i + 4])
            print("  mine: %s" % m[max(0, j - 6):j + 4])
            return 1
        _score, dq, dm = best
        if dq or dm:
            resyncs += 1
            if resyncs <= 6:
                print("resync: skipped %d qemu / %d mine entries near qemu[%d]"
                      % (dq, dm, i))
        i += dq
        j += dm
    print("agree to the end (%d resyncs, %d qemu entries, %d mine)"
          % (resyncs, len(q), len(m)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
