#!/usr/bin/env python3
"""Check every decode guard against what llvm calls the instructions it matches.

One record per guard: name, mask, value, and the set of mnemonics that guard is allowed to
match. Keeping the allowed set beside the guard is the point - an earlier version kept the
names in one table and the expectations in another, and the two drifted apart, so the check
silently verified 29 of 73 guards. Two attempts to repair that by generating one table from
the other produced a table that looked complete and checked nothing.

The allowed sets were generated from busybox.static with llvm-objdump. A guard that starts
matching something new means either a decoder regression or a binary with instructions not
seen here; both are worth knowing.
"""
import os, re, shutil, subprocess, sys

GUARDS = [
    ("AND/ANDS+8", 0x1F000000, 0x0A000000, {"and", "ands", "bic", "bics", "eor", "mov", "mvn", "orn", "orr", "tst"}),
    ("ADR/ADRP", 0x1F000000, 0x10000000, {"adr", "adrp"}),
    ("ADD/ADDS+5", 0x1F000000, 0x11000000, {"add", "adds", "cmn", "cmp", "mov", "sub", "subs"}),
    ("ADD/ADDS+6", 0x1F200000, 0x0B000000, {"add", "adds", "cmn", "cmp", "neg", "negs", "sub", "subs"}),
    ("ADD/ADDS+3", 0x1F200000, 0x0B200000, {"add", "adds", "cmp", "sub", "subs"}),
    ("EOR/MOV", 0x1F200400, 0x0E200400, {"eor", "mov"}),
    ("MADD/MNEG+2", 0x1F600000, 0x1B000000, {"madd", "mneg", "msub", "mul"}),
    ("AND/ANDS+4", 0x1F800000, 0x12000000, {"and", "ands", "eor", "mov", "orr", "tst"}),
    ("ASR/BFI+10", 0x1F800000, 0x13000000, {"asr", "bfi", "bfxil", "lsl", "lsr", "sbfiz", "sbfx", "sxtb", "sxth", "sxtw", "ubfiz", "ubfx"}),
    ("CINC/CINV+7", 0x1FE00000, 0x1A800000, {"cinc", "cinv", "cneg", "csel", "cset", "csetm", "csinc", "csinv", "csneg"}),
    ("ADC/SBC", 0x1FE0FC00, 0x1A000000, {"adc", "sbc"}),
    ("LDP/LDPSW+1", 0x3A000000, 0x28000000, {"ldp", "ldpsw", "stp"}),
    ("LDR/LDRB+7", 0x3B000000, 0x39000000, {"ldr", "ldrb", "ldrh", "ldrsb", "ldrsh", "ldrsw", "str", "strb", "strh"}),
    ("LDR/LDRB+15", 0x3B200000, 0x38000000, {"ldr", "ldrb", "ldrh", "ldrsb", "ldrsw", "ldur", "ldurb", "ldurh", "ldursb", "ldursh", "ldursw", "str", "strb", "strh", "stur", "sturb", "sturh"}),
    ("LDAXR/STLXR", 0x3F000000, 0x08000000, {"ldaxr", "stlxr"}),
    ("LDR/STR", 0x3F000000, 0x3D000000, {"ldr", "str"}),
    ("LDR/STR", 0x3F200000, 0x3C200000, {"ldr", "str"}),
    ("LDR/LDRB+7", 0x3F200C00, 0x38200800, {"ldr", "ldrb", "ldrh", "ldrsb", "ldrsh", "ldrsw", "str", "strb", "strh"}),
    ("CCMN/CCMP", 0x3FE00000, 0x3A400000, {"ccmn", "ccmp"}),
    ("FCVTZS/FCVTZU", 0x5F3EFC00, 0x1E380000, {"fcvtzs", "fcvtzu"}),
    ("B/BL", 0x7C000000, 0x14000000, {"b", "bl"}),
    ("CBNZ/CBZ", 0x7E000000, 0x34000000, {"cbnz", "cbz"}),
    ("TBNZ/TBZ", 0x7E000000, 0x36000000, {"tbnz", "tbz"}),
    ("SMADDL/SMSUBL+3", 0x7F200000, 0x1B200000, {"smaddl", "smsubl", "smull", "umaddl", "umull"}),
    ("SCVTF", 0x7F3FFC00, 0x1E220000, {"scvtf"}),
    ("UCVTF", 0x7F3FFC00, 0x1E230000, {"ucvtf"}),
    ("EXTR/ROR", 0x7FA00000, 0x13800000, {"extr", "ror"}),
    ("ASR/LSL+3", 0x7FE00000, 0x1AC00000, {"asr", "lsl", "lsr", "sdiv", "udiv"}),
    ("CLZ/RBIT+2", 0x7FE00000, 0x5AC00000, {"clz", "rbit", "rev", "rev16"}),
    ("SMULH", 0x7FE0FC00, 0x1B407C00, {"smulh"}),
    ("UMULH", 0x7FE0FC00, 0x1BC07C00, {"umulh"}),
    ("FMADD/FMSUB+6", 0x8F000400, 0x0F000400, {"fmadd", "fmsub", "movi", "mvni", "shl", "sshll", "ushll", "ushr"}),
    ("guard_32", 0x9F3FFC00, 0x0E212800, {}),
    ("guard_33", 0x9F3FFC00, 0x0E214800, {}),
    ("SSHLL/USHLL", 0x9F80FC00, 0x0F00A400, {"sshll", "ushll"}),
    ("MOV", 0x9FE0FC00, 0x0E001C00, {"mov"}),
    ("MOV", 0x9FE0FC00, 0x0E003C00, {"mov"}),
    ("DUP", 0x9FF0FC00, 0x0E000C00, {"dup"}),
    ("MOVI/MVNI", 0x9FF80400, 0x0F000400, {"movi", "mvni"}),
    ("guard_39", 0xBF200C00, 0x0E000800, {}),
    ("guard_40", 0xBF9FF400, 0x0D008000, {}),
    ("guard_41", 0xBF9FF400, 0x0D008400, {}),
    ("SCVTF/UCVTF", 0xDF3FFC00, 0x5E21D800, {"scvtf", "ucvtf"}),
    ("B.EQ/B.GE+10", 0xFF000010, 0x54000000, {"b.eq", "b.ge", "b.gt", "b.hi", "b.hs", "b.le", "b.lo", "b.ls", "b.lt", "b.mi", "b.ne", "b.pl"}),
    ("EOR/MOV", 0xFF008400, 0x6E000400, {"eor", "mov"}),
    ("FADD/FDIV+2", 0xFF200C00, 0x1E200800, {"fadd", "fdiv", "fmul", "fsub"}),
    ("FCSEL", 0xFF200C00, 0x1E200C00, {"fcsel"}),
    ("FMOV", 0xFF201FE0, 0x1E201000, {"fmov"}),
    ("FCMP/FCMPE", 0xFF207C00, 0x1E202000, {"fcmp", "fcmpe"}),
    ("SCVTF", 0xFF3F0000, 0x1E020000, {"scvtf"}),
    ("guard_50", 0xFF3F0000, 0x1E030000, {}),
    ("LD1/ST1", 0xFF3FF000, 0x4C00A000, {"ld1", "st1"}),
    ("FMADD/FMSUB+1", 0xFF800000, 0x1F000000, {"fmadd", "fmsub", "fnmsub"}),
    ("FCVTZS", 0xFFBFFC00, 0x5EA1B800, {"fcvtzs"}),
    ("FCVTZU", 0xFFBFFC00, 0x7EA1B800, {"fcvtzu"}),
    ("SVC", 0xFFE0001F, 0xD4000001, {"svc"}),
    ("MSR", 0xFFF00000, 0xD5100000, {"msr"}),
    ("MRS", 0xFFF00000, 0xD5300000, {"mrs"}),
    ("NOP", 0xFFFFF01F, 0xD503201F, {"nop"}),
    ("DMB", 0xFFFFF09F, 0xD503309F, {"dmb"}),
    ("FMOV", 0xFFFFFC00, 0x1E260000, {"fmov"}),
    ("FMOV", 0xFFFFFC00, 0x1E270000, {"fmov"}),
    ("FMOV", 0xFFFFFC00, 0x9E660000, {"fmov"}),
    ("FMOV", 0xFFFFFC00, 0x9E670000, {"fmov"}),
    ("FMOV", 0xFFFFFC00, 0x9EAE0000, {"fmov"}),
    ("FMOV", 0xFFFFFC00, 0x9EAF0000, {"fmov"}),
    ("BR", 0xFFFFFC1F, 0xD61F0000, {"br"}),
    ("BLR", 0xFFFFFC1F, 0xD63F0000, {"blr"}),
    ("RET", 0xFFFFFC1F, 0xD65F0000, {"ret"}),
    ("DC", 0xFFFFFFE0, 0xD50B7420, {"dc"}),
]


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: check-decode-families.py BINARY", file=sys.stderr)
        return 2
    path = sys.argv[1]
    od = shutil.which("llvm-objdump-18") or shutil.which("llvm-objdump")
    if od is None:
        print("needs llvm-objdump on PATH", file=sys.stderr)
        return 2
    raw = subprocess.run([od, "-d", path], capture_output=True, text=True).stdout
    words = []
    for line in raw.split("\n"):
        m = re.match(r"\s*[0-9a-f]+:\s+([0-9a-f]{8})\s+(\S+)", line)
        if m:
            words.append((int(m.group(1), 16), m.group(2)))
    if not words:
        print("no instructions disassembled", file=sys.stderr)
        return 2

    bad = 0
    for label, mask, value, allowed in GUARDS:
        hits = {}
        for word, mnemonic in words:
            if (word & mask) == value:
                hits[mnemonic] = hits.get(mnemonic, 0) + 1
        stray = {k: v for k, v in hits.items() if k not in allowed}
        if stray:
            bad += 1
            shown = ", ".join(f"{k}:{v}" for k, v in sorted(stray.items())[:6])
            print(f"  {label:22s} matches outside its set: {shown}")
        elif not hits:
            print(f"  {label:22s} matched nothing in this binary")
    print(f"\n{len(GUARDS)} guards, {bad} matching outside their recorded set")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

