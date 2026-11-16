#include "e2/exec.h"

#include <string.h>

// This interpreter is written from the ARM Architecture Reference Manual and the
// AArch64 encoding rules. It shares no code with iSH or any other emulator, which
// is the whole point: the resulting backend is ours to license.

#define BIT(insn, n) (((insn) >> (n)) & 1u)
#define BITS(insn, hi, lo) (((insn) >> (lo)) & ((1u << ((hi) - (lo) + 1)) - 1u))
#define IS64(insn) (BIT((insn), 31) != 0)

static uint64_t sign_extend(uint64_t value, unsigned bits) {
    uint64_t sign = 1ULL << (bits - 1);
    return (value ^ sign) - sign;
}

static void e2_cpu_reset_impl(e2_cpu *cpu, uint64_t entry) {
    memset(cpu, 0, sizeof(*cpu));
    cpu->pc = entry;
}

void e2_cpu_reset(e2_cpu *cpu, uint64_t entry) {
    e2_cpu_reset_impl(cpu, entry);
}

const char *e2_status_name(e2_status status) {
    switch (status) {
        case E2_OK: return "ok";
        case E2_HALTED: return "halted";
        case E2_FAULT: return "fault";
        case E2_UNIMPL: return "unimplemented";
        case E2_SYSCALL: return "syscall";
    }
    return "unknown";
}

void e2_machine_init(e2_machine *machine, uint64_t entry, size_t stack_size) {
    memset(machine, 0, sizeof(*machine));
    e2_memory_init(&machine->mem);
    e2_cpu_reset_impl(&machine->cpu, entry);

    if (stack_size > 0) {
        const size_t rounded = (stack_size + E2_PAGE_SIZE - 1) & ~(size_t)(E2_PAGE_SIZE - 1);
        // One spare page above SP: startup code in musl zeroes the region between
        // a computed base and the stack top, and the top itself is exclusive, so
        // the last store lands one word past SP. Real kernels leave room there;
        // without it the very first line of libc init faults.
        uint64_t stack_base = 0;
        if (e2_memory_map_any(&machine->mem, rounded + E2_PAGE_SIZE,
                              E2_PROT_READ | E2_PROT_WRITE, &stack_base) == 0) {
            machine->cpu.sp = stack_base + rounded - 64;
        }
    }
}

void e2_machine_destroy(e2_machine *machine) {
    e2_memory_destroy(&machine->mem);
}

static void write_reg(e2_cpu *cpu, unsigned reg, uint64_t value, bool is64) {
    if (!is64) {
        value &= 0xFFFFFFFFu;
    }
    if (reg != E2_REG_XZR) {
        cpu->x[reg] = value;
    }
}

/// ARM ARM "DecodeBitMasks": expands the immN:imms:immr encoding into a 64-bit
/// pattern that repeats an element of size 1<<len.
///
/// This is the fiddliest encoding in the integer set. Get it wrong and the
/// failure looks like a wild pointer, because the operands are usually masks and
/// alignments rather than data.
static int decode_bit_masks(unsigned n, unsigned imms, unsigned immr, unsigned width,
                            bool immediate, uint64_t *wmask_out) {
    const unsigned combined = (n << 6) | ((~imms) & 0x3Fu);
    int len = -1;
    for (int i = 6; i >= 0; i--) {
        if (combined & (1u << i)) {
            len = i;
            break;
        }
    }
    if (len < 1) {
        return -1;
    }
    const unsigned levels = (1u << len) - 1;
    if (immediate && (imms & levels) == levels) {
        return -1;  // reserved for MOV (bitmask immediate)
    }
    const unsigned s = imms & levels;
    const unsigned r = immr & levels;
    const unsigned esize = 1u << len;
    if (esize > width) {
        return -1;
    }

    const uint64_t ones = (s + 1 >= 64) ? ~0ULL : ((1ULL << (s + 1)) - 1);
    const uint64_t esize_mask = (esize >= 64) ? ~0ULL : ((1ULL << esize) - 1);
    uint64_t elem = ones & esize_mask;
    const unsigned rotate = r % esize;
    if (rotate != 0) {
        elem = ((elem >> rotate) | (elem << (esize - rotate))) & esize_mask;
    }

    uint64_t result = 0;
    for (unsigned i = 0; i < width; i += esize) {
        result |= elem << i;
    }
    *wmask_out = result;
    return 0;
}

/// Evaluates an AArch64 condition code.
///
/// The 4-bit code is a base condition in the upper three bits plus an invert bit.
/// EQ/NE, CS/CC, MI/PL and the rest are pairs sharing one base test, so the
/// invert must be applied after evaluating the base.
static bool condition_holds(const e2_cpu *cpu, unsigned cond) {
    const e2_flags *f = &cpu->flags;
    bool result;
    switch (cond >> 1) {
        case 0: result = f->z; break;                    // EQ / NE
        case 1: result = f->c; break;                    // CS / CC
        case 2: result = f->n; break;                    // MI / PL
        case 3: result = f->v; break;                    // VS / VC
        case 4: result = f->c && !f->z; break;           // HI / LS
        case 5: result = f->n == f->v; break;            // GE / LT
        case 6: result = (f->n == f->v) && !f->z; break; // GT / LE
        default: result = true; break;                   // AL / NV
    }
    if (cond & 1u) {
        result = !result;
    }
    return result;
}

/// Sets NZCV for an addition or subtraction on unsigned operands of `width` bits.
static void set_flags_addsub(e2_cpu *cpu, uint64_t a, uint64_t b, uint64_t result,
                             bool is_sub, unsigned width) {
    uint64_t sign_bit = 1ULL << (width - 1);
    uint64_t mask = (width == 64) ? ~0ULL : ((1ULL << width) - 1);
    result &= mask;
    cpu->flags.n = (result & sign_bit) != 0;
    cpu->flags.z = result == 0;
    if (!is_sub) {
        cpu->flags.c = result < (a & mask);                      // unsigned carry out
        cpu->flags.v = ((~(a ^ b) & (a ^ result)) & sign_bit) != 0;
    } else {
        cpu->flags.c = (a & mask) >= (b & mask);                  // no borrow
        cpu->flags.v = (((a ^ b) & (a ^ result)) & sign_bit) != 0;
    }
}

e2_status e2_step(e2_machine *machine) {
    e2_cpu *cpu = &machine->cpu;
    machine->ctx.pc = cpu->pc;

    uint32_t insn = 0;
    if (e2_memory_read(&machine->mem, cpu->pc, &insn, sizeof(insn)) != 0) {
        machine->ctx.instruction = 0;
        machine->ctx.reason = "instruction fetch outside mapped memory";
        return E2_FAULT;
    }
    machine->ctx.instruction = insn;

    const bool is64 = IS64(insn);
    const unsigned rd = BITS(insn, 4, 0);
    const unsigned rn = BITS(insn, 9, 5);
    uint64_t next_pc = cpu->pc + 4;

    // --- ADR / ADRP ---
    if ((insn & 0x1F000000u) == 0x10000000u) {
        const bool page = BIT(insn, 31) != 0;
        const uint64_t immlo = BITS(insn, 30, 29);
        const uint64_t immhi = BITS(insn, 23, 5);
        const int64_t imm = (int64_t)sign_extend((immhi << 2) | immlo, 21);
        uint64_t base = cpu->pc;
        int64_t delta = imm;
        if (page) {
            base &= ~0xFFFULL;
            delta = imm << 12;
        }
        write_reg(cpu, rd, (uint64_t)((int64_t)base + delta), true);
        cpu->pc = next_pc;
        machine->instructions++;
        return E2_OK;
    }

    // --- MOVZ / MOVN / MOVK ---
    uint32_t mov_top = insn & 0x7F800000u;
    if (mov_top == 0x52800000u || mov_top == 0x12800000u || mov_top == 0x72800000u) {
        const unsigned hw = BITS(insn, 22, 21);
        const uint64_t imm16 = BITS(insn, 20, 5);
        const unsigned shift = hw * 16;
        const uint64_t value = imm16 << shift;

        if (mov_top == 0x52800000u) {            // MOVZ
            write_reg(cpu, rd, value, is64);
        } else if (mov_top == 0x12800000u) {     // MOVN
            write_reg(cpu, rd, ~value, is64);
        } else {                                 // MOVK
            uint64_t current = is64 ? e2_read_xzr(cpu, rd) : (e2_read_xzr(cpu, rd) & 0xFFFFFFFFu);
            uint64_t mask = (uint64_t)0xFFFF << shift;
            current = (current & ~mask) | value;
            write_reg(cpu, rd, current, is64);
        }
        cpu->pc = next_pc;
        machine->instructions++;
        return E2_OK;
    }

    // --- ADD / SUB (immediate), with or without flags ---
    if ((insn & 0x1F000000u) == 0x11000000u) {
        const bool is_sub = BIT(insn, 30) != 0;
        const bool set_flags = BIT(insn, 29) != 0;
        const unsigned shift = BIT(insn, 22) ? 12 : 0;
        const uint64_t imm = (uint64_t)BITS(insn, 21, 10) << shift;
        const unsigned width = is64 ? 64 : 32;

        // MOV (to/from SP) is an alias of ADD #0 with Rn or Rd = 31.
        uint64_t a = e2_read_sp(cpu, rn);
        uint64_t b = imm;
        uint64_t result = is_sub ? (a - b) : (a + b);
        if (!is64) {
            result &= 0xFFFFFFFFu;
        }
        if (set_flags) {
            set_flags_addsub(cpu, a, b, result, is_sub, width);
        }
        // The meaning of Rd=31 depends on the S bit, and this is the trap that
        // silently corrupted SP: in ADDS/SUBS it is XZR (result discarded), only
        // in ADD/SUB is it SP. Reading Rn is always SP, which is why CMP reads
        // SP correctly but must not write it back.
        if (set_flags) {
            write_reg(cpu, rd, result, is64);
        } else {
            e2_write_sp(cpu, rd, result);
        }
        cpu->pc = next_pc;
        machine->instructions++;
        return E2_OK;
    }

    // --- ADD / SUB (shifted register) ---
    if ((insn & 0x1F200000u) == 0x0B000000u) {
        const bool is_sub = BIT(insn, 30) != 0;
        const bool set_flags = BIT(insn, 29) != 0;
        const unsigned rm = BITS(insn, 20, 16);
        const unsigned shift_type = BITS(insn, 23, 22);
        const unsigned shift_amount = BITS(insn, 15, 10);
        const unsigned width = is64 ? 64 : 32;
        const uint64_t mask = is64 ? ~0ULL : 0xFFFFFFFFULL;

        // Rm=31 is XZR: reading the array directly would run past X30.
        uint64_t operand = e2_read_xzr(cpu, rm) & mask;
        switch (shift_type) {
            case 0: operand <<= shift_amount; break;                                   // LSL
            case 1: operand >>= shift_amount; break;                                   // LSR
            case 2: operand = (uint64_t)((int64_t)(operand << (64 - width)) >> (64 - width)) >> shift_amount; break;  // ASR
            case 3: {                                                                  // ROR
                unsigned r = shift_amount % width;
                operand = r ? ((operand >> r) | (operand << (width - r))) : operand;
                break;
            }
        }
        operand &= mask;
        uint64_t a = e2_read_xzr(cpu, rn) & mask;
        uint64_t result = is_sub ? (a - operand) : (a + operand);
        result &= mask;
        if (set_flags) {
            set_flags_addsub(cpu, a, operand, result, is_sub, width);
        }
        write_reg(cpu, rd, result, is64);
        cpu->pc = next_pc;
        machine->instructions++;
        return E2_OK;
    }

    // --- Logical (immediate): AND / ORR / EOR / ANDS ---
    // musl's startup aligns the stack with `and sp, x0, #~15`, so nothing runs
    // until this exists.
    if ((insn & 0x1F800000u) == 0x12000000u) {
        const unsigned opc = BITS(insn, 30, 29);
        const unsigned n_bit = BIT(insn, 22);
        const unsigned immr = BITS(insn, 21, 16);
        const unsigned imms = BITS(insn, 15, 10);
        const unsigned width = is64 ? 64 : 32;

        uint64_t mask = 0;
        if (decode_bit_masks(n_bit, imms, immr, width, false, &mask) != 0) {
            machine->ctx.reason = "invalid logical immediate";
            return E2_UNIMPL;
        }

        const uint64_t a = e2_read_sp(cpu, rn) & (is64 ? ~0ULL : 0xFFFFFFFFULL);
        uint64_t result;
        switch (opc) {
            case 0: result = a & mask; break;   // AND
            case 1: result = a | mask; break;   // ORR
            case 2: result = a ^ mask; break;   // EOR
            default: result = a & mask; break;  // ANDS
        }
        result &= is64 ? ~0ULL : 0xFFFFFFFFULL;
        if (opc == 3) {
            cpu->flags.n = (result & (1ULL << (width - 1))) != 0;
            cpu->flags.z = result == 0;
            cpu->flags.c = false;
            cpu->flags.v = false;
        }
        // Same S-bit rule as add/sub immediate: Rd=31 is SP for AND/ORR/EOR,
        // and XZR for ANDS.
        if (opc == 3) {
            write_reg(cpu, rd, result, is64);
        } else {
            e2_write_sp(cpu, rd, result);
        }
        cpu->pc = next_pc;
        machine->instructions++;
        return E2_OK;
    }

    // --- Logical (shifted register): AND / ORR / EOR / ANDS ---
    // Also covers MOV (register), which assemblers emit as ORR Xd, XZR, Xm.
    if ((insn & 0x1F000000u) == 0x0A000000u && BIT(insn, 21) == 0) {
        const unsigned opc = BITS(insn, 30, 29);
        const unsigned shift_type = BITS(insn, 23, 22);
        const unsigned shift_amount = BITS(insn, 15, 10);
        const unsigned rm = BITS(insn, 20, 16);
        const unsigned width = is64 ? 64 : 32;
        const uint64_t mask = is64 ? ~0ULL : 0xFFFFFFFFULL;

        // Rm=31 is XZR here too; the shifted-register forms have no SP operand.
        uint64_t operand = e2_read_xzr(cpu, rm) & mask;
        switch (shift_type) {
            case 0: operand = (operand << shift_amount) & mask; break;
            case 1: operand >>= shift_amount; break;
            case 2:
                operand = (uint64_t)((int64_t)(operand << (64 - width)) >> (64 - width));
                operand = (uint64_t)((int64_t)operand >> shift_amount);
                break;
            case 3: {
                unsigned r = shift_amount % width;
                operand = r ? (((operand >> r) | (operand << (width - r))) & mask) : operand;
                break;
            }
        }
        operand &= mask;

        // No SP form here: Rn=31 means XZR, so read through the zero-aware helper.
        const uint64_t a = e2_read_xzr(cpu, rn) & mask;
        uint64_t result;
        switch (opc) {
            case 0: result = a & operand; break;   // AND
            case 1: result = a | operand; break;   // ORR / MOV
            case 2: result = a ^ operand; break;   // EOR
            default: result = a & operand; break;  // ANDS
        }
        result &= mask;
        if (opc == 3) {                            // ANDS sets NZCV, C and V cleared
            cpu->flags.n = (result & (1ULL << (width - 1))) != 0;
            cpu->flags.z = result == 0;
            cpu->flags.c = false;
            cpu->flags.v = false;
        }
        write_reg(cpu, rd, result, is64);
        cpu->pc = next_pc;
        machine->instructions++;
        return E2_OK;
    }

    // --- Bitfield: SBFM / BFM / UBFM ---
    // The field width is immS - immR + 1, not immS + 1. Using the wrong width
    // silently returns extra high bits, which is how `ubfiz` turned into a
    // garbage divisor and the division strength-reduction produced digits above 9.
    //
    // When immS < immR the field wraps: the low immS+1 bits are shifted left,
    // which is the ubfiz/sbfiz alias family.
    if ((insn & 0x1F800000u) == 0x13000000u) {
        const unsigned opc = BITS(insn, 30, 29);  // 00 SBFM, 01 BFM, 10 UBFM
        const unsigned immr = BITS(insn, 21, 16);
        const unsigned imms = BITS(insn, 15, 10);
        const unsigned width = is64 ? 64 : 32;
        const uint64_t width_mask = is64 ? ~0ULL : 0xFFFFFFFFULL;

        if (opc == 3) {
            machine->ctx.reason = "reserved bitfield encoding";
            return E2_UNIMPL;
        }

        const uint64_t src = cpu->x[rn] & width_mask;
        uint64_t result;

        if (imms >= immr) {
            const unsigned field_bits = imms - immr + 1;
            const uint64_t field_mask = (field_bits >= 64) ? ~0ULL
                                                           : ((1ULL << field_bits) - 1);
            const uint64_t field = (src >> immr) & field_mask;
            switch (opc) {
                case 0: {  // SBFM: sign-extend the extracted field
                    const uint64_t sign = 1ULL << (field_bits - 1);
                    result = (field ^ sign) - sign;
                    break;
                }
                case 1: {  // BFM: insert into destination at immr
                    const uint64_t mask = field_mask << immr;
                    const uint64_t dst = e2_read_xzr(cpu, rd) & width_mask;
                    result = (dst & ~mask) | (field << immr);
                    break;
                }
                default:   // UBFM: zero-extend
                    result = field;
                    break;
            }
        } else {
            // Wrapped: the field starts at bit 0 and shifts up by width - immr.
            const unsigned field_bits = imms + 1;
            const uint64_t field_mask = (field_bits >= 64) ? ~0ULL
                                                           : ((1ULL << field_bits) - 1);
            const uint64_t field = src & field_mask;
            const unsigned shift = width - immr;
            switch (opc) {
                case 0: {  // SBFM
                    const uint64_t sign = 1ULL << (field_bits - 1);
                    const uint64_t extended = (field ^ sign) - sign;
                    result = (extended << shift) & width_mask;
                    break;
                }
                case 1: {  // BFM
                    const uint64_t mask = (field_mask << shift) & width_mask;
                    const uint64_t shifted = (field << shift) & width_mask;
                    const uint64_t dst = e2_read_xzr(cpu, rd) & width_mask;
                    result = (dst & ~mask) | shifted;
                    break;
                }
                default:   // UBFM (ubfiz family)
                    result = (field << shift) & width_mask;
                    break;
            }
        }

        write_reg(cpu, rd, result & width_mask, is64);
        cpu->pc = next_pc;
        machine->instructions++;
        return E2_OK;
    }

    // --- LDP / STP (signed offset, pre-index, post-index) ---
    if ((insn & 0x3A000000u) == 0x28000000u && BIT(insn, 26) == 0) {
        const bool is_load = BIT(insn, 22) != 0;
        const unsigned mode = BITS(insn, 24, 23);  // 01 post, 10 offset, 11 pre
        const int64_t imm7 = (int64_t)sign_extend(BITS(insn, 21, 15), 7);
        const unsigned scale = is64 ? 8u : 4u;
        const int64_t offset = imm7 * (int64_t)scale;
        const unsigned rt = BITS(insn, 4, 0);
        const unsigned rn2 = BITS(insn, 9, 5);
        const unsigned rt2 = BITS(insn, 14, 10);

        uint64_t base = e2_read_sp(cpu, rn2);
        uint64_t addr;
        if (mode == 1) {                             // post-index
            addr = base;
        } else if (mode == 3) {                      // pre-index
            base = (uint64_t)((int64_t)base + offset);
            addr = base;
        } else {                                     // signed offset
            addr = (uint64_t)((int64_t)base + offset);
        }

        if (is_load) {
            uint64_t v1 = 0;
            uint64_t v2 = 0;
            if (e2_memory_read(&machine->mem, addr, &v1, scale) != 0 ||
                e2_memory_read(&machine->mem, addr + scale, &v2, scale) != 0) {
                machine->ctx.reason = "ldp outside mapped memory";
                return E2_FAULT;
            }
            write_reg(cpu, rt, v1, is64);
            write_reg(cpu, rt2, v2, is64);
        } else {
            uint64_t v1 = is64 ? e2_read_xzr(cpu, rt) : (e2_read_xzr(cpu, rt) & 0xFFFFFFFFu);
            uint64_t v2 = is64 ? e2_read_xzr(cpu, rt2) : (e2_read_xzr(cpu, rt2) & 0xFFFFFFFFu);
            if (e2_memory_write(&machine->mem, addr, &v1, scale) != 0 ||
                e2_memory_write(&machine->mem, addr + scale, &v2, scale) != 0) {
                machine->ctx.reason = "stp outside mapped memory";
                return E2_FAULT;
            }
        }

        if (mode == 1) {                             // post-index writes back
            e2_write_sp(cpu, rn2, (uint64_t)((int64_t)base + offset));
        } else if (mode == 3) {
            e2_write_sp(cpu, rn2, base);
        }
        cpu->pc = next_pc;
        machine->instructions++;
        return E2_OK;
    }


    // --- SIMD: DUP (general) ---
    // `dup v0.16b, w1` is the first instruction of musl's memset, so this is the
    // entry point for vector support.
    if ((insn & 0x9FF80C00u) == 0x0E000C00u) {
        const bool q = BIT(insn, 30) != 0;
        const unsigned imm5 = BITS(insn, 20, 16);
        const unsigned rn_v = BITS(insn, 9, 5);
        const unsigned rd_v = BITS(insn, 4, 0);

        unsigned width;
        if (imm5 & 1u) {
            width = 8;
        } else if (imm5 & 2u) {
            width = 16;
        } else if (imm5 & 4u) {
            width = 32;
        } else {
            width = 64;
        }
        const uint64_t element = width == 64 ? cpu->x[rn_v] : cpu->x[rn_v] & ((1ULL << width) - 1);
        const unsigned lanes = (q ? 128u : 64u) / width;

        cpu->v[rd_v][0] = 0;
        cpu->v[rd_v][1] = 0;
        for (unsigned i = 0; i < lanes; i++) {
            e2_v_set(cpu, rd_v, width, i, element);
        }
        cpu->pc = next_pc;
        machine->instructions++;
        return E2_OK;
    }

    // --- SIMD: LDR / STR (unsigned immediate) ---
    if ((insn & 0x3F000000u) == 0x3D000000u) {
        const bool is_load = BIT(insn, 22) != 0;
        const unsigned size_code = BITS(insn, 31, 30);
        const unsigned q = BIT(insn, 23);
        // Q=1 selects a 128-bit access (with size=00); size alone covers the
        // narrower forms. They are alternatives, not factors.
        const unsigned bytes = q ? 16u : (1u << size_code);
        const uint64_t offset = (uint64_t)BITS(insn, 21, 10) * bytes;
        const uint64_t addr = e2_read_sp(cpu, rn) + offset;
        const unsigned rt_v = BITS(insn, 4, 0);

        if (is_load) {
            if (e2_memory_read(&machine->mem, addr, &cpu->v[rt_v][0], bytes > 8 ? 8 : bytes) != 0) {
                machine->ctx.reason = "simd load outside mapped memory";
                return E2_FAULT;
            }
            if (bytes > 8 && e2_memory_read(&machine->mem, addr + 8, &cpu->v[rt_v][1], 8) != 0) {
                machine->ctx.reason = "simd load outside mapped memory";
                return E2_FAULT;
            }
        } else {
            if (e2_memory_write(&machine->mem, addr, &cpu->v[rt_v][0], bytes > 8 ? 8 : bytes) != 0) {
                machine->ctx.reason = "simd store outside mapped memory";
                return E2_FAULT;
            }
            if (bytes > 8 && e2_memory_write(&machine->mem, addr + 8, &cpu->v[rt_v][1], 8) != 0) {
                machine->ctx.reason = "simd store outside mapped memory";
                return E2_FAULT;
            }
        }
        cpu->pc = next_pc;
        machine->instructions++;
        return E2_OK;
    }

    // --- SIMD: LDP / STP (V=1 shares the encoding group with the integer pair) ---
    if ((insn & 0x3A000000u) == 0x28000000u && BIT(insn, 26) != 0) {
        const bool is_load = BIT(insn, 22) != 0;
        const unsigned opc = BITS(insn, 31, 30);   // 00=S, 01=D, 10=Q
        const unsigned mode = BITS(insn, 24, 23);  // 01 post, 10 offset, 11 pre
        const unsigned scale = (opc == 2) ? 16u : (4u << opc);
        const int64_t imm7 = (int64_t)sign_extend(BITS(insn, 21, 15), 7);
        const int64_t offset = imm7 * (int64_t)scale;
        const unsigned rt_v = BITS(insn, 4, 0);
        const unsigned rn_v = BITS(insn, 9, 5);
        const unsigned rt2_v = BITS(insn, 14, 10);

        uint64_t base = e2_read_sp(cpu, rn_v);
        uint64_t addr;
        if (mode == 1) {
            addr = base;
        } else if (mode == 3) {
            base = (uint64_t)((int64_t)base + offset);
            addr = base;
        } else {
            addr = (uint64_t)((int64_t)base + offset);
        }

        const size_t half = scale > 8 ? 8 : scale;
        if (is_load) {
            if (e2_memory_read(&machine->mem, addr, &cpu->v[rt_v][0], half) != 0 ||
                e2_memory_read(&machine->mem, addr + half, &cpu->v[rt_v][1], half) != 0 ||
                e2_memory_read(&machine->mem, addr + scale, &cpu->v[rt2_v][0], half) != 0 ||
                e2_memory_read(&machine->mem, addr + scale + half, &cpu->v[rt2_v][1], half) != 0) {
                machine->ctx.reason = "simd ldp outside mapped memory";
                return E2_FAULT;
            }
        } else {
            if (e2_memory_write(&machine->mem, addr, &cpu->v[rt_v][0], half) != 0 ||
                e2_memory_write(&machine->mem, addr + half, &cpu->v[rt_v][1], half) != 0 ||
                e2_memory_write(&machine->mem, addr + scale, &cpu->v[rt2_v][0], half) != 0 ||
                e2_memory_write(&machine->mem, addr + scale + half, &cpu->v[rt2_v][1], half) != 0) {
                machine->ctx.reason = "simd stp outside mapped memory";
                return E2_FAULT;
            }
        }

        if (mode == 1) {
            e2_write_sp(cpu, rn_v, (uint64_t)((int64_t)base + offset));
        } else if (mode == 3) {
            e2_write_sp(cpu, rn_v, base);
        }
        cpu->pc = next_pc;
        machine->instructions++;
        return E2_OK;
    }

    // --- Conditional select: CSEL / CSINC / CSINV / CSNEG ---
    // Ternary operators and min/max compile to these; CSET is CSINC with an
    // inverted condition.
    if ((insn & 0x1FE00000u) == 0x1A800000u) {
        const unsigned op = BITS(insn, 30, 29);
        const unsigned rm = BITS(insn, 20, 16);
        const unsigned cond = BITS(insn, 15, 12);
        const unsigned rn2 = BITS(insn, 9, 5);
        const uint64_t mask = is64 ? ~0ULL : 0xFFFFFFFFULL;

        uint64_t result;
        if (condition_holds(cpu, cond)) {
            result = e2_read_xzr(cpu, rn2);
        } else {
            const uint64_t other = e2_read_xzr(cpu, rm);
            switch (op) {
                case 0: result = other; break;            // CSEL
                case 1: result = other + 1; break;        // CSINC
                case 2: result = ~other; break;           // CSINV
                default: result = (uint64_t)(-(int64_t)other); break;  // CSNEG
            }
        }
        write_reg(cpu, rd, result & mask, is64);
        cpu->pc = next_pc;
        machine->instructions++;
        return E2_OK;
    }

    // --- CCMP / CCMN (conditional compare) ---
    // Chains comparisons without branching, e.g. `a <= b && b < c`. When the
    // condition fails the flags are taken from the instruction instead of being
    // computed, which is how short-circuit evaluation stays branch-free.
    if ((insn & 0x1FE00000u) == 0x1A400000u) {
        const bool is_compare = BIT(insn, 30) != 0;  // 1 = CCMP(sub), 0 = CCMN(add)
        const bool is_immediate = BIT(insn, 11) != 0;
        const unsigned cond = BITS(insn, 15, 12);
        const unsigned nzcv = BITS(insn, 3, 0);
        const unsigned rm_or_imm = BITS(insn, 20, 16);
        const unsigned rn2 = BITS(insn, 9, 5);
        const unsigned width = is64 ? 64 : 32;
        const uint64_t mask = is64 ? ~0ULL : 0xFFFFFFFFULL;

        if (condition_holds(cpu, cond)) {
            const uint64_t a = e2_read_xzr(cpu, rn2) & mask;
            const uint64_t b = is_immediate ? (uint64_t)rm_or_imm
                                            : (e2_read_xzr(cpu, rm_or_imm) & mask);
            const uint64_t result = is_compare ? (a - b) : (a + b);
            set_flags_addsub(cpu, a, b, result, is_compare, width);
        } else {
            cpu->flags.n = (nzcv >> 3) & 1u;
            cpu->flags.z = (nzcv >> 2) & 1u;
            cpu->flags.c = (nzcv >> 1) & 1u;
            cpu->flags.v = nzcv & 1u;
        }
        cpu->pc = next_pc;
        machine->instructions++;
        return E2_OK;
    }

    // --- MADD / MSUB ---
    // `mul xD, Xn, Xm` is MADD with Ra=XZR, so the accumulator being 31 is
    // normal here. The high-multiply variants live in a different encoding group
    // (bit 23 set) and are handled separately below; keying on Ra alone made
    // every plain multiply return the high half of the product, i.e. zero.
    if ((insn & 0x1F000000u) == 0x1B000000u && BIT(insn, 23) == 0) {
        const bool is_sub = BIT(insn, 15) != 0;
        const unsigned rm = BITS(insn, 20, 16);
        const unsigned ra = BITS(insn, 14, 10);
        const unsigned rn2 = BITS(insn, 9, 5);
        const uint64_t mask = is64 ? ~0ULL : 0xFFFFFFFFULL;

        const uint64_t a = e2_read_xzr(cpu, rn2) & mask;
        const uint64_t b = e2_read_xzr(cpu, rm) & mask;
        const uint64_t acc = e2_read_xzr(cpu, ra) & mask;
        const uint64_t result = is_sub ? (acc - a * b) : (acc + a * b);
        write_reg(cpu, rd, result & mask, is64);
        cpu->pc = next_pc;
        machine->instructions++;
        return E2_OK;
    }

    // --- SMADDL / SMSUBL / UMADDL / UMSUBL ---
    // 32x32 -> 64 widen-multiply with accumulate. `umull xD, wN, wM` is UMADDL
    // with Ra=XZR; compilers emit these for any 32-bit multiply whose result is
    // held in 64 bits, so they appear in ordinary integer code.
    //
    // Unsigned is selected by bit 23, not bit 22: UMADDL is 101 while SMADDL is
    // 001 in bits 23-21. Testing bit 22 sign-extended the operands, so an
    // unsigned product came back as 0xFFFFFFFF_00000000.
    if ((insn & 0x7F200000u) == 0x1B200000u) {
        const bool is_unsigned = BIT(insn, 23) != 0;
        const bool is_sub = BIT(insn, 15) != 0;
        const unsigned rm = BITS(insn, 20, 16);
        const unsigned ra = BITS(insn, 14, 10);
        const unsigned rn2 = BITS(insn, 9, 5);

        uint64_t a = cpu->x[rn2] & 0xFFFFFFFFu;
        uint64_t b = cpu->x[rm] & 0xFFFFFFFFu;
        if (!is_unsigned) {
            a = (uint64_t)(int64_t)(int32_t)a;
            b = (uint64_t)(int64_t)(int32_t)b;
        }
        const uint64_t acc = e2_read_xzr(cpu, ra);
        const uint64_t product = a * b;
        const uint64_t result = is_sub ? (acc - product) : (acc + product);
        write_reg(cpu, rd, result, true);
        cpu->pc = next_pc;
        machine->instructions++;
        return E2_OK;
    }

    // --- SMULH / UMULH (Ra field is 11111; bit 23 selects signed vs unsigned) ---
    if ((insn & 0x7FE0FC00u) == 0x1B407C00u || (insn & 0x7FE0FC00u) == 0x1BC07C00u) {
        const bool is_unsigned = BIT(insn, 23) != 0;
        const unsigned rm = BITS(insn, 20, 16);
        const unsigned rn2 = BITS(insn, 9, 5);

        const uint64_t a = e2_read_xzr(cpu, rn2);
        const uint64_t b = e2_read_xzr(cpu, rm);
        uint64_t result;
        if (is_unsigned) {
            const unsigned __int128 product = (unsigned __int128)a * (unsigned __int128)b;
            result = (uint64_t)(product >> 64);
        } else {
            const __int128 product = (__int128)(int64_t)a * (__int128)(int64_t)b;
            result = (uint64_t)(product >> 64);
        }
        write_reg(cpu, rd, result, true);
        cpu->pc = next_pc;
        machine->instructions++;
        return E2_OK;
    }

    // --- B / BL ---
    if ((insn & 0x7C000000u) == 0x14000000u) {
        const bool link = BIT(insn, 31) != 0;
        const int64_t offset = (int64_t)sign_extend(BITS(insn, 25, 0), 26) << 2;
        if (link) {
            cpu->x[E2_REG_LR] = next_pc;
        }
        next_pc = (uint64_t)((int64_t)cpu->pc + offset);
        cpu->pc = next_pc;
        machine->instructions++;
        return E2_OK;
    }

    // --- B.cond ---
    if ((insn & 0xFF000010u) == 0x54000000u) {
        const unsigned cond = BITS(insn, 3, 0);
        const int64_t offset = (int64_t)sign_extend(BITS(insn, 23, 5), 19) << 2;
        if (condition_holds(cpu, cond)) {
            next_pc = (uint64_t)((int64_t)cpu->pc + offset);
        }
        cpu->pc = next_pc;
        machine->instructions++;
        return E2_OK;
    }

    // --- CBZ / CBNZ ---
    if ((insn & 0x7E000000u) == 0x34000000u) {
        const bool nonzero = BIT(insn, 24) != 0;
        const int64_t offset = (int64_t)sign_extend(BITS(insn, 23, 5), 19) << 2;
        const uint64_t value = is64 ? e2_read_xzr(cpu, rd) : (e2_read_xzr(cpu, rd) & 0xFFFFFFFFu);
        const bool take = nonzero ? (value != 0) : (value == 0);
        if (take) {
            next_pc = (uint64_t)((int64_t)cpu->pc + offset);
        }
        cpu->pc = next_pc;
        machine->instructions++;
        return E2_OK;
    }

    // --- TBZ / TBNZ ---
    // Single-bit test and branch. Bit 24 selects test-zero vs test-nonzero; the
    // tested bit number is split across bits 31 and 23-19.
    if ((insn & 0x7E000000u) == 0x36000000u) {
        const bool branch_if_set = BIT(insn, 24) != 0;
        const unsigned bit_pos = (BIT(insn, 31) << 5) | BITS(insn, 23, 19);
        const int64_t offset = (int64_t)sign_extend(BITS(insn, 18, 5), 14) << 2;
        const uint64_t value = is64 ? e2_read_xzr(cpu, rd) : (e2_read_xzr(cpu, rd) & 0xFFFFFFFFu);
        const bool bit_is_set = ((value >> bit_pos) & 1u) != 0;
        if (bit_is_set == branch_if_set) {
            next_pc = (uint64_t)((int64_t)cpu->pc + offset);
        }
        cpu->pc = next_pc;
        machine->instructions++;
        return E2_OK;
    }

    // --- BR / BLR / RET ---
    if ((insn & 0xFFFFFC1Fu) == 0xD61F0000u ||   // BR
        (insn & 0xFFFFFC1Fu) == 0xD63F0000u ||   // BLR
        (insn & 0xFFFFFC1Fu) == 0xD65F0000u) {   // RET
        const uint32_t kind = insn & 0xFFFFFC1Fu;
        const uint64_t target = cpu->x[rn];
        if (kind == 0xD63F0000u) {               // BLR
            cpu->x[E2_REG_LR] = next_pc;
        }
        cpu->pc = target;
        machine->instructions++;
        return E2_OK;
    }

    // --- LDR / STR (unsigned immediate offset) ---
    if ((insn & 0x3B000000u) == 0x39000000u) {
        const unsigned size_code = BITS(insn, 31, 30);
        const bool is_load = BIT(insn, 22) != 0;
        const uint64_t offset = (uint64_t)BITS(insn, 21, 10) * (1u << size_code);
        const uint64_t addr = cpu->x[rn] + offset;
        const unsigned bytes = 1u << size_code;

        if (is_load) {
            uint64_t value = 0;
            if (e2_memory_read(&machine->mem, addr, &value, bytes) != 0) {
                machine->ctx.reason = "load outside mapped memory";
                return E2_FAULT;
            }
            write_reg(cpu, rd, value, size_code == 3);
        } else {
            // Storing XZR is legal and common (zero-init loops); reading x[31]
            // would take a byte from the neighbouring stack slot instead.
            uint64_t value = e2_read_xzr(cpu, rd);
            if (e2_memory_write(&machine->mem, addr, &value, bytes) != 0) {
                machine->ctx.reason = "store outside mapped memory";
                return E2_FAULT;
            }
        }
        cpu->pc = next_pc;
        machine->instructions++;
        return E2_OK;
    }

    // --- LDR / STR (immediate, unscaled / pre-index / post-index) ---
    // Compilers use the signed-offset form for struct and stack slot access, so
    // it shows up as soon as real code runs.
    if ((insn & 0x3B200000u) == 0x38000000u && BIT(insn, 26) == 0) {
        const bool is_load = BIT(insn, 22) != 0;
        const unsigned size_code = BITS(insn, 31, 30);
        const unsigned mode = BITS(insn, 11, 10);   // 00 unscaled, 01 post, 11 pre
        const int64_t imm9 = (int64_t)sign_extend(BITS(insn, 20, 12), 9);
        const unsigned bytes = 1u << size_code;

        uint64_t base = e2_read_sp(cpu, rn);
        uint64_t addr;
        if (mode == 1) {                            // post-index
            addr = base;
        } else {
            addr = (uint64_t)((int64_t)base + imm9);
        }

        if (is_load) {
            uint64_t value = 0;
            if (e2_memory_read(&machine->mem, addr, &value, bytes) != 0) {
                machine->ctx.reason = "unscaled load outside mapped memory";
                return E2_FAULT;
            }
            write_reg(cpu, rd, value, size_code == 3);
        } else {
            uint64_t value = e2_read_xzr(cpu, rd);
            if (e2_memory_write(&machine->mem, addr, &value, bytes) != 0) {
                machine->ctx.reason = "unscaled store outside mapped memory";
                return E2_FAULT;
            }
        }

        if (mode == 1) {                            // post-index writeback
            e2_write_sp(cpu, rn, (uint64_t)((int64_t)base + imm9));
        } else if (mode == 3) {                     // pre-index writeback
            e2_write_sp(cpu, rn, addr);
        }
        cpu->pc = next_pc;
        machine->instructions++;
        return E2_OK;
    }

    // --- LDR / STR (register offset) ---
    // Byte-at-a-time loops over a pointer index compile to this form, so it shows
    // up immediately in any libc string routine.
    if ((insn & 0x3B200C00u) == 0x38200800u) {
        const bool is_load = BIT(insn, 22) != 0;
        const unsigned size_code = BITS(insn, 31, 30);
        const unsigned rm = BITS(insn, 20, 16);
        const unsigned option = BITS(insn, 15, 13);
        const bool scale = BIT(insn, 12) != 0;
        const unsigned bytes = 1u << size_code;

        uint64_t offset;
        switch (option) {
            case 0x2: offset = e2_read_xzr(cpu, rm) & 0xFFFFFFFFu; break;       // UXTW
            case 0x6: offset = (uint64_t)(int64_t)(int32_t)(e2_read_xzr(cpu, rm) & 0xFFFFFFFFu); break;  // SXTW
            default: offset = e2_read_xzr(cpu, rm); break;                       // LSL / SXTX
        }
        if (scale) {
            offset <<= size_code;
        }
        const uint64_t addr = e2_read_sp(cpu, rn) + offset;

        if (is_load) {
            uint64_t value = 0;
            if (e2_memory_read(&machine->mem, addr, &value, bytes) != 0) {
                machine->ctx.reason = "load outside mapped memory";
                machine->ctx.svc_number = (uint16_t)(addr & 0xFFFF);
                return E2_FAULT;
            }
            write_reg(cpu, rd, value, size_code == 3);
        } else {
            uint64_t value = e2_read_xzr(cpu, rd);
            if (e2_memory_write(&machine->mem, addr, &value, bytes) != 0) {
                machine->ctx.reason = "store outside mapped memory";
                machine->ctx.svc_number = (uint16_t)(addr & 0xFFFF);
                return E2_FAULT;
            }
        }
        cpu->pc = next_pc;
        machine->instructions++;
        return E2_OK;
    }

    // --- SVC ---
    if ((insn & 0xFFE0001Fu) == 0xD4000001u) {
        machine->ctx.svc_number = (uint16_t)BITS(insn, 20, 5);
        cpu->pc = next_pc;
        machine->instructions++;
        return E2_SYSCALL;
    }

    // --- NOP ---
    if (insn == 0xD503201Fu) {
        cpu->pc = next_pc;
        machine->instructions++;
        return E2_OK;
    }

    machine->ctx.reason = "unimplemented instruction";
    return E2_UNIMPL;
}

e2_status e2_run(e2_machine *machine, uint64_t max_instructions) {
    uint64_t executed = 0;
    for (;;) {
        if (max_instructions != 0 && executed >= max_instructions) {
            machine->ctx.reason = "instruction budget exhausted";
            return E2_FAULT;
        }
        e2_status status = e2_step(machine);
        executed++;
        if (status != E2_OK) {
            return status;
        }
    }
}
