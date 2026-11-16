#!/usr/bin/env python3
"""Align two instruction traces and report where they stop agreeing.

A strict index-for-index comparison is useless here: qemu's gdb stub does not
report a step across `svc` consistently, so every syscall shows up as a
divergence that is not one. This uses a longest-common-subsequence alignment so
dropped entries cost nothing, and reports the first place the two sequences stop
sharing a prefix.

    tools/trace-align.py qemu.txt mine.txt
"""
import sys


def align(q, m):
    n, k = len(q), len(m)
    dp = [[0] * (k + 1) for _ in range(n + 1)]
    for i in range(1, n + 1):
        qi = q[i - 1]
        row, prev = dp[i], dp[i - 1]
        for j in range(1, k + 1):
            row[j] = prev[j - 1] + 1 if qi == m[j - 1] else max(prev[j], row[j - 1])
    i, j = n, k
    path = []
    while i > 0 and j > 0:
        if q[i - 1] == m[j - 1]:
            path.append((i - 1, j - 1))
            i -= 1
            j -= 1
        elif dp[i - 1][j] >= dp[i][j - 1]:
            i -= 1
        else:
            j -= 1
    path.reverse()
    return path


def main():
    q = [l.strip() for l in open(sys.argv[1]) if l.strip()]
    m = [l.strip() for l in open(sys.argv[2]) if l.strip()]
    path = align(q, m)
    print("qemu=%d mine=%d common=%d" % (len(q), len(m), len(path)))
    if not path:
        print("nothing in common")
        return
    la, lb = path[-1]
    print("last common: qemu[%d]=%s  mine[%d]=%s" % (la, q[la], lb, m[lb]))
    print("qemu continues: %s" % q[la + 1:la + 6])
    print("mine continues: %s" % m[lb + 1:lb + 6])
    # A growing index gap means one side is executing extra instructions rather
    # than taking a different branch - different causes, different fixes.
    worst = max(range(1, len(path)), key=lambda t: abs(path[t][0] - path[t][1]))
    a, b = path[worst]
    print("largest index gap %d at qemu[%d]=%s / mine[%d]=%s"
          % (abs(a - b), a, q[a], b, m[b]))


if __name__ == "__main__":
    main()
