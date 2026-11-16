#!/usr/bin/env python3
"""Cross-check E2's decode rules against an independent disassembler.

The overlap checker only finds rules that clash with each other. It cannot see
a rule that is simply too loose: DUP's mask omitted bits 15-12, so UMOV fell
into it and every memset wrote zeros - and because the overlap checker mirrors
the same mask in Python, it agreed with the bug instead of catching it.

This tool answers a different question: for every instruction a rule matches in
a real binary, what does llvm actually call it? A rule that matches several
unrelated mnemonics is too loose. A rule that matches only its own family is
fine, even when that family has many names (CSEL/CSINC/CSET, LD1/LD1R...).

    python3 tools/check-rule-mnemonics.py BINARY

Needs llvm-mc and llvm-objdump on PATH.
"""
import re
import shutil
import subprocess
import sys
import tempfile
from collections import Counter, defaultdict
from pathlib import Path

# Keep this in step with exec.c; it is the whole point of the check.
RULES = [
    ("ADR/ADRP",        0x1F000000, 0x10000000),
    ("ADD/SUB imm",     0x1F000000, 0x11000000),
    ("ADD/SUB shifted", 0x1F200000, 0x0B000000),
    ("ADD/SUB extended",0x1F200000, 0x0B200000),
    ("logical imm",     0x1F800000, 0x12000000),
    ("logical shifted", 0x1F000000, 0x0A000000),
    ("bitfield",        0x1F800000, 0x13000000),
    ("DUP",             0x9FF0FC00, 0x0E000C00),
    ("UMOV",            0x9FE0FC00, 0x0E003C00),
    ("MOVI",            0x9FF80400, 0x0F000400),
    ("CSEL",            0x1FE00000, 0x1A800000),
    ("2-source",        0x7FE00000, 0x1AC00000),
    ("FMOV general",    0xFFFFFC00, 0x1E270000),
    ("FMOV scalar imm", 0xFF201FE0, 0x1E201000),
    ("FP 2-source",     0xFF200C00, 0x1E200800),
    ("SHLL",            0x9F80FC00, 0x0F00A400),
    ("LD1/ST1 pair",    0xFF3FF000, 0x4C00A000),
    ("LD1/ST1 lane",    0xBF9FF400, 0x0D008400),
]

# llvm reports "unknown" for words that are data rather than instructions; a
# binary's .rodata is full of them, so they are not a signal.
IGNORE = {"unknown", "<unknown>"}

# Families whose members legitimately share one rule. CRC32/SUBP share the
# data-processing (2 source) encoding group but the handler's default branch
# rejects them with E2_UNIMPL, so they are reported as an error rather than
# executed wrongly.
FAMILY = {
    "DUP": {"dup", "unknown"},
    "UMOV": {"mov", "unknown"},
    "MOVI": {"movi", "mvni", "orr", "bic", "fmov", "unknown"},
    "CSEL": {"csel", "csinc", "csinv", "csneg", "cset", "csetm", "cinc", "cinv", "cneg", "unknown"},
    "2-source": {"lsl", "lsr", "asr", "ror", "sdiv", "udiv",
                 "crc32b", "crc32h", "crc32w", "crc32x",
                 "crc32cb", "crc32ch", "crc32cw", "crc32cx", "subp"},
    "FMOV general": {"fmov", "unknown"},
    "FMOV scalar imm": {"fmov", "unknown"},
    "FP 2-source": {"fmul", "fdiv", "fadd", "fsub", "fmax", "fmin", "fnmul", "unknown"},
    "SHLL": {"shll", "sshll", "ushll", "sshll2", "ushll2", "shll2", "unknown"},
    "LD1/ST1 pair": {"ld1", "st1", "unknown"},
    "LD1/ST1 lane": {"ld1", "st1", "unknown"},
}


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    binary = sys.argv[1]
    data = Path(binary).read_bytes()
    words = sorted({int.from_bytes(data[o:o + 4], 'little')
                    for o in range(0, len(data) - 4, 4)})

    mc = shutil.which("llvm-mc-18") or shutil.which("llvm-mc")
    od = shutil.which("llvm-objdump-18") or shutil.which("llvm-objdump")
    if not mc or not od:
        print("needs llvm-mc and llvm-objdump on PATH")
        return 2

    with tempfile.TemporaryDirectory() as td:
        asm = Path(td) / "w.s"
        obj = Path(td) / "w.o"
        asm.write_text(".text\n" + "\n".join(".inst 0x%08x" % w for w in words) + "\n")
        subprocess.run([mc, "--assemble", "--filetype=obj", "--triple=aarch64",
                        str(asm), "-o", str(obj)], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        dump = subprocess.run([od, "-d", "--no-show-raw-insn", str(obj)],
                              capture_output=True, text=True, check=True).stdout

    mnem = {}
    for line in dump.splitlines():
        m = re.match(r'^\s+([0-9a-f]+):\s+(\S+)', line)
        if m:
            mnem[int(m.group(1), 16)] = m.group(2)

    by_rule = defaultdict(Counter)
    for i, w in enumerate(words):
        name = mnem.get(i * 4)
        if name is None:
            continue
        for rule, mask, val in RULES:
            if (w & mask) == val:
                by_rule[rule][name] += 1

    bad = 0
    for rule, _mask, _val in RULES:
        c = by_rule.get(rule)
        if not c:
            continue
        allowed = FAMILY.get(rule)
        stray = {k: v for k, v in c.items()
                 if allowed and k not in allowed and k not in IGNORE}
        if stray:
            bad += 1
            print(f"{rule:<18} TOO LOOSE: " +
                  ", ".join(f"{k}:{v}" for k, v in sorted(stray.items())))
        else:
            print(f"{rule:<18} ok ({sum(c.values())} instructions)")

    print(f"\n{len(words)} unique words; {bad} rule(s) match instructions outside their family")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
