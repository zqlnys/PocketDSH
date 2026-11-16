#!/usr/bin/env python3
"""Report aarch64 instructions matched by more than one E2 decode rule.

Most of the encoding bugs in this interpreter had the same shape: one prefix,
several families selected by a few bits, and the rule only tested some of
those bits. The instruction then ran as the wrong family, and the symptom
appeared somewhere unrelated.

Running this against real binaries catches that class before it becomes a
debugging session. Update RULES whenever a decode rule changes.

    python3 tools/check-decode-overlaps.py BINARY [BINARY...]

Exits non-zero when any overlap is found.
"""
import struct, sys

def load_elf_text(path):
    data = open(path,'rb').read()
    e_phoff, = struct.unpack_from('<Q', data, 0x20)
    e_phentsize, e_phnum = struct.unpack_from('<HH', data, 0x36)
    segs = []
    for i in range(e_phnum):
        off = e_phoff + i*e_phentsize
        p_type, = struct.unpack_from('<I', data, off)
        if p_type != 1: continue
        p_flags, = struct.unpack_from('<I', data, off+4)
        p_offset, p_vaddr, _, p_filesz = struct.unpack_from('<QQQQ', data, off+8)
        if p_flags & 0x1:  # executable
            segs.append((p_offset, p_vaddr, p_filesz))
    return data, segs

# (name, mask, value, extra predicate)
RULES = [
    ("ADR/ADRP",        0x1F000000, 0x10000000, None),
    ("ADD/SUB imm",     0x1F000000, 0x11000000, None),
    ("ADD/SUB shifted", 0x1F200000, 0x0B000000, None),
    ("ADD/SUB extended",0x1F200000, 0x0B200000, None),
    ("logical imm",     0x1F800000, 0x12000000, None),
    ("logical shifted", 0x1F000000, 0x0A000000, None),
    ("bitfield",        0x1F800000, 0x13000000, None),
    ("SIMD LDP/STP",    0x3A000000, 0x28000000, lambda i: (i>>26)&1),
    ("int LDP/STP",     0x3A000000, 0x28000000, lambda i: not ((i>>26)&1)),
    # DUP and UMOV share bits 11-10; only bits 15-12 tell them apart, so the
    # mask has to include those. It did not, and UMOV ran as DUP.
    ("DUP",             0x9FF0FC00, 0x0E000C00, None),
    ("UMOV",            0x9F80FC00, 0x0E003C00, None),
    ("FMOV general",    0xFFFFFC00, 0x1E270000, None),
    ("FMOV scalar imm", 0xFF201FE0, 0x1E201000, None),
    ("SCVTF/UCVTF int", 0x7F3FFC00, 0x1E230000, None),
    ("FP 2-source",     0xFF200C00, 0x1E200800, None),
    ("MOVI",            0x9FF80400, 0x0F000400, None),
    ("SIMD ld/st uoff", 0x3F000000, 0x3D000000, None),
    ("CSEL",            0x1FE00000, 0x1A800000, None),
    ("2-source",        0x7FE00000, 0x1AC00000, None),
    ("CCMP",            0x3FE00000, 0x3A400000, None),
    ("MADD",            0x1F600000, 0x1B000000, None),
    ("SMADDL",          0x7F200000, 0x1B200000, None),
    ("SMULH/UMULH",     0x7FE0FC00, 0x1BC07C00, None),
    ("SMULH/UMULH-2",   0x7FE0FC00, 0x1B407C00, None),
    ("B/BL",            0x7C000000, 0x14000000, None),
    ("B.cond",          0xFF000010, 0x54000000, None),
    ("CBZ",             0x7E000000, 0x34000000, None),
    ("TBZ",             0x7E000000, 0x36000000, None),
    ("BR family",       0xFFFFFC1F, 0xD61F0000, None),
    ("BLR family",      0xFFFFFC1F, 0xD63F0000, None),
    ("RET family",      0xFFFFFC1F, 0xD65F0000, None),
    ("LDR/STR uoff",    0x3B000000, 0x39000000, lambda i: not ((i>>26)&1)),
    ("unscaled int",    0x3B200000, 0x38000000, lambda i: not ((i>>26)&1)),
    ("unscaled simd",   0x3B200000, 0x38000000, lambda i: (i>>26)&1),
    ("reg offset",      0x3B200C00, 0x38200800, None),
    ("MRS/MSR",         0xFFF00000, 0xD5300000, None),
    ("MRS/MSR-2",       0xFFF00000, 0xD5100000, None),
    ("exclusive",       0x3F000000, 0x08000000, None),
    ("barrier",         0xFFFFF09F, 0xD503309F, None),
    ("DC ZVA",          0xFFFFFFE0, 0xD50B7420, None),
]

def matches(insn):
    out = []
    for name, mask, val, pred in RULES:
        if (insn & mask) == val and (pred is None or pred(insn)):
            out.append(name)
    return out

failures = 0
for path in sys.argv[1:]:
    data, segs = load_elf_text(path)
    total = 0
    conflicts = {}
    for poff, pvaddr, pfilesz in segs:
        for o in range(0, pfilesz - 3, 4):
            insn, = struct.unpack_from('<I', data, poff + o)
            total += 1
            m = matches(insn)
            if len(m) > 1:
                key = tuple(sorted(m))
                conflicts.setdefault(key, []).append((pvaddr + o, insn))
    print(f"{path}: {total} words scanned")
    for key, hits in sorted(conflicts.items(), key=lambda kv: -len(kv[1]))[:8]:
        addr, insn = hits[0]
        print(f"  {' + '.join(key)}: {len(hits)} hits, e.g. 0x{addr:x} 0x{insn:08x}")
    if not conflicts:
        print("  no overlaps")
    else:
        failures += 1

sys.exit(1 if failures else 0)
