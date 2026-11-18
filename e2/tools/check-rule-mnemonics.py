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

def _rules_from_source():
    """Read the decode guards straight out of exec.c.

    RULES used to be a hand-maintained copy, and it drifted: exec.c grew to 73 guards
    while this list still held 29, so the check silently verified 40% of the decoder it
    claimed to check. A copy of a changing thing is a copy that will be wrong; reading the
    source removes the failure mode rather than asking people to be careful.
    """
    import os
    import re
    here = os.path.dirname(os.path.abspath(__file__))
    src = open(os.path.join(here, "..", "src", "exec.c"), encoding="utf-8").read()
    found = []
    for mask, value in re.findall(
            r"\(insn & (0x[0-9A-Fa-f]+)u?\)\s*==\s*(0x[0-9A-Fa-f]+)u?", src):
        pair = (int(mask, 16), int(value, 16))
        if pair not in [(m, v) for _, m, v in found]:
            found.append((f"rule_{len(found)}", pair[0], pair[1]))
    return found


RULES = _rules_from_source()


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
                 "crc32cb", "crc32ch", "crc32cw", "crc32cx", "subp",
                 # SIMD min/max share the mask; their opcode fields are outside
                 # the handled set, so the default branch rejects them.
                 "umin", "umax", "smin", "smax",
                 # Insert Random Tag shares the mask; its opcode is outside the
                 # handled set and is rejected.
                 "irg"},
    "FMOV general": {"fmov", "unknown"},
    "FMOV scalar imm": {"fmov", "unknown"},
    "FP 2-source": {"fmul", "fdiv", "fadd", "fsub", "fmax", "fmin",
                    "fmaxnm", "fminnm", "fnmul"},
    "SHLL": {"shll", "sshll", "ushll", "sshll2", "ushll2", "shll2", "unknown"},
    "LD1/ST1 pair": {"ld1", "st1", "unknown"},
    "LD1/ST1 lane": {"ld1", "st1", "unknown"},
    "LD/ST uoff general": {"ldr", "str", "ldrb", "strb", "ldrh", "strh",
                           "ldrsb", "ldrsh", "ldrsw", "prfm"},
    "LD/ST uoff SIMD": {"ldr", "str", "ldur", "stur", "prfm", "ld1", "st1"},
    # Unscaled covers both the *ur aliases and the unprivileged *tr forms; llvm
    # prints the shorter name when the offset happens to be zero.
    "LD/ST unscaled general": {"ldur", "stur", "ldurb", "sturb", "ldurh", "sturh",
                               "ldursb", "ldursh", "ldursw", "prfum",
                               "ldtr", "ldtrb", "ldtrh", "ldtrsb", "ldtrsh",
                               "ldtrsw", "sttr", "sttrb", "sttrh",
                               "ldr", "str", "ldrb", "strb", "ldrh", "strh",
                               "ldrsb", "ldrsh", "ldrsw"},
    "LD/ST unscaled SIMD": {"ldur", "stur", "ldr", "str", "prfum"},
    "LD/ST reg offset general": {"ldr", "str", "ldrb", "strb", "ldrh", "strh",
                                 "ldrsb", "ldrsh", "ldrsw", "prfm"},
    "LD/ST reg offset SIMD": {"ldr", "str", "prfm"},
    # stgp shares the pair encoding space; without MTE it behaves as stp, which
    # is what the handler does, so it is listed rather than reported.
    "LD/ST pair general": {"ldp", "stp", "ldpsw", "ldnp", "stnp", "stgp"},
    "SIMD shift immediate": {"shl", "sshr", "ushr", "sli", "sri", "sri", "shrn",
                              "sshr", "ushr", "ssra", "usra", "srsra", "ursra",
                              "sri", "sli", "sqshl", "uqshl", "sqshrn", "uqshrn",
                              "shll", "sshll", "ushll", "sqshlu", "scvtf", "ucvtf"},
    "MOVI/MVNI": {"movi", "mvni", "movi", "orr", "bic", "fmov", "mvni"},
    "Int to FP fixed point": {"scvtf", "ucvtf"},
    "FP to int (in FP reg)": {"fcvtzs", "fcvtzu", "fcvtms", "fcvtps", "fcvtns",
                              "fcvtas", "fcvtmu", "fcvtpu", "fcvtnu", "fcvtau",
                              "fcvt"},
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
            if (w & mask) != val:
                continue
            # The shift-by-immediate family is only claimed when immh != 0;
            # movi/mvni share the mask with immh == 0. Without this the checker
            # reports those as a loose rule when the decoder already excludes
            # them, and a checker that cries wolf gets ignored.
            if rule == "SIMD shift immediate":
                if ((w >> 30) & 1) == 0 or ((w >> 19) & 0xF) == 0:
                    continue
                # The decoder only claims SHL/SSHR/USHR; every other opcode in
                # the family falls through to its own rule.
                if ((w >> 11) & 0x1F) not in (0x0A, 0x00):
                    continue
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
