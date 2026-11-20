#!/bin/sh
# M4/CPython 最小复现
# 需要：Alpine aarch64 根文件系统在 /tmp/alpine-root（含 python3.12 + musl），
#      以及构建出的 e2/tools/rune。
set -u
cd "$(dirname "$0")/e2" || exit 1
make || exit 1

echo "== 能跑（不进解释器主循环）=="
E2_CAP=20000000 ./tools/rune /tmp/alpine-root/usr/bin/python3.12 --version 2>&1 | tail -1

echo "== 崩溃（要进解释器主循环）=="
E2_CAP=200000000 ./tools/rune /tmp/alpine-root/usr/bin/python3.12 -c "pass" 2>&1 | tail -2

echo "== 崩溃点细节 =="
E2_CAP=200000000 E2_DUMP_REGIONS=1 ./tools/rune \
    /tmp/alpine-root/usr/bin/python3.12 -c "pass" 2>&1 | grep -E "FAULT|region" | head -6
