#include "e2/exec.h"

#include <stdio.h>
#include <stdlib.h>
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
    machine->cycle_clock = 1000000;
    e2_memory_init(&machine->mem);
    e2_cpu_reset_impl(&machine->cpu, entry);

    if (stack_size > 0) {
        const size_t rounded = (stack_size + E2_PAGE_SIZE - 1) & ~(size_t)(E2_PAGE_SIZE - 1);
        // Place the stack high, the way Linux does. A low stack collides with
        // fixed-address executables: statically linked busybox owns 0x400000.
        {
            const uint64_t high = 0x7F000000ULL;
            if (e2_memory_map_at(&machine->mem, high, rounded + E2_PAGE_SIZE,
                                 E2_PROT_READ | E2_PROT_WRITE) == 0) {
                machine->cpu.sp = high + rounded - 64;
                return;
            }
        }
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

/// Applies the opc field of a load to the value read from memory.
///
/// opc is two bits: 00 store, 01 zero-extend, 10 sign-extend to 64,
/// 11 sign-extend to 32. Bit 22 alone is not enough - LDRSW has it clear and
/// was being decoded as a store, writing a register into unrelated memory.
static uint64_t apply_load_opc(uint64_t value, unsigned opc, unsigned bytes, bool is64) {
    if (opc == 2) {
        const unsigned bits = bytes * 8;
        if (bits < 64) {
            const uint64_t sign = 1ULL << (bits - 1);
            value = (value ^ sign) - sign;
        }
    } else if (opc == 3) {
        const unsigned bits = bytes * 8;
        uint64_t extended = value;
        if (bits < 32) {
            const uint64_t sign = 1ULL << (bits - 1);
            extended = (value ^ sign) - sign;
        }
        value = (uint64_t)(int64_t)(int32_t)extended & 0xFFFFFFFFu;
    } else if (!is64) {
        value &= 0xFFFFFFFFu;
    }
    return value;
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

int e2_machine_clone(const e2_machine *src, e2_machine *dst) {
    *dst = *src;
    if (e2_memory_clone(&src->mem, &dst->mem) != 0) {
        return -1;
    }
    dst->ctx.reason = NULL;
    return 0;
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

    // --- ADD / SUB (extended register) ---
    // Distinguished from the shifted-register form by bit 21. The second operand
    // is zero- or sign-extended to the operation width before shifting, and both
    // Rn and Rd may be SP. `sub sp, sp, x2` compiles to this.
    if ((insn & 0x1F200000u) == 0x0B200000u) {
        const bool is_sub = BIT(insn, 30) != 0;
        const bool set_flags = BIT(insn, 29) != 0;
        const unsigned rm = BITS(insn, 20, 16);
        const unsigned option = BITS(insn, 15, 13);
        const unsigned shift_amount = BITS(insn, 12, 10);
        const unsigned width = is64 ? 64 : 32;
        const uint64_t mask = is64 ? ~0ULL : 0xFFFFFFFFULL;

        uint64_t operand = e2_read_xzr(cpu, rm);
        switch (option) {
            case 0x0: operand &= 0xFF; break;                       // UXTB
            case 0x1: operand &= 0xFFFF; break;                     // UXTH
            case 0x2: operand &= 0xFFFFFFFFu; break;                // UXTW
            case 0x3: break;                                        // UXTX / LSL
            case 0x4: operand = (uint64_t)(int64_t)(int8_t)operand; break;   // SXTB
            case 0x5: operand = (uint64_t)(int64_t)(int16_t)operand; break;  // SXTH
            case 0x6: operand = (uint64_t)(int64_t)(int32_t)operand; break;  // SXTW
            default: break;                                         // SXTX
        }
        operand = (operand << shift_amount) & mask;

        const uint64_t a = e2_read_sp(cpu, rn) & mask;
        const uint64_t result = (is_sub ? (a - operand) : (a + operand)) & mask;
        if (set_flags) {
            set_flags_addsub(cpu, a, operand, result, is_sub, width);
        }
        if (set_flags) {
            write_reg(cpu, rd, result, is64);
        } else {
            e2_write_sp(cpu, rd, result);
        }
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

        // The source operand is a general register: Rn=31 is XZR, not SP. Only
        // the destination (for AND/ORR/EOR) may be SP.
        const uint64_t a = e2_read_xzr(cpu, rn) & (is64 ? ~0ULL : 0xFFFFFFFFULL);
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
    if ((insn & 0x1F000000u) == 0x0A000000u) {
        const bool invert = BIT(insn, 21) != 0;
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
        if (invert) {
            operand = ~operand & mask;
        }

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
    if ((insn & 0x9FE00C00u) == 0x0E000C00u) {
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

    // --- Floating point to integer: FCVTZS / FCVTZU ---
    // C's float-to-int casts lower to these. AArch64 saturates out-of-range
    // values and maps NaN to zero; the host cast is undefined there, so the
    // range is clamped explicitly.
    if ((insn & 0x5F3EFC00u) == 0x1E380000u) {
        const bool wide = BIT(insn, 31) != 0;
        const bool is_unsigned = BIT(insn, 16) != 0;
        const unsigned ftype = BITS(insn, 23, 22);
        const unsigned rn2 = BITS(insn, 9, 5);
        double v;
        if (ftype == 0) {
            uint32_t b32 = (uint32_t)cpu->v[rn2][0];
            float f;
            memcpy(&f, &b32, 4);
            v = (double)f;
        } else {
            uint64_t b64 = cpu->v[rn2][0];
            memcpy(&v, &b64, 8);
        }
        uint64_t result;
        if (v != v) {
            result = 0;                                  // NaN
        } else if (is_unsigned) {
            if (v <= 0.0) result = 0;
            else if (wide && v >= 18446744073709551615.0) result = ~0ULL;
            else if (!wide && v >= 4294967295.0) result = 0xFFFFFFFFULL;
            else result = (uint64_t)v;
        } else {
            if (wide) {
                if (v <= -9223372036854775808.0) result = 0x8000000000000000ULL;
                else if (v >= 9223372036854775807.0) result = 0x7FFFFFFFFFFFFFFFULL;
                else result = (uint64_t)(int64_t)v;
            } else {
                if (v <= -2147483648.0) result = (uint64_t)(int64_t)(int32_t)0x80000000;
                else if (v >= 2147483647.0) result = 0x7FFFFFFFULL;
                else result = (uint64_t)(int64_t)(int32_t)v;
            }
        }
        write_reg(cpu, rd, result, wide);
        cpu->pc = next_pc;
        machine->instructions++;
        return E2_OK;
    }

    // --- SIMD scalar SCVTF / UCVTF (integer in a vector register) ---
    // Python's float formatting uses this form rather than the general-register
    // one. U is bit 29, sz is bit 22; `ucvtf d0, d0` is 0x7E61D800 and
    // `scvtf d0, d0` 0x5E61D800, so those two bits stay out of the mask.
    if ((insn & 0xDF3FFC00u) == 0x5E21D800u) {
        const bool is_unsigned = BIT(insn, 29) != 0;
        const bool dbl = BIT(insn, 22) != 0;
        const unsigned rn2 = BITS(insn, 9, 5);
        const uint64_t raw = cpu->v[rn2][0];
        if (dbl) {
            const double d = is_unsigned ? (double)(uint64_t)raw : (double)(int64_t)raw;
            uint64_t bits;
            memcpy(&bits, &d, 8);
            cpu->v[rd][0] = bits;
            cpu->v[rd][1] = 0;
        } else {
            const uint32_t r32 = (uint32_t)raw;
            const float f = is_unsigned ? (float)r32 : (float)(int32_t)r32;
            uint32_t bits;
            memcpy(&bits, &f, 4);
            cpu->v[rd][0] = bits;
            cpu->v[rd][1] = 0;
        }
        cpu->pc = next_pc;
        machine->instructions++;
        return E2_OK;
    }

    // --- Advanced SIMD, three same-width register operations ---
    // Python's integer arithmetic runs on these. In the logical subgroup the
    // operation is selected by the size field (0 AND, 1 BIC, 2 ORR, 3 ORN),
    // while add/sub/mul use the (U, opcode) pair.
    if ((insn & 0x1F200400u) == 0x0E200400u) {
        const bool q = BIT(insn, 30) != 0;
        const unsigned u = BIT(insn, 29);
        const unsigned size = BITS(insn, 23, 22);
        const unsigned rm = BITS(insn, 20, 16);
        const unsigned opcode = BITS(insn, 15, 11);
        const unsigned rn2 = BITS(insn, 9, 5);
        const unsigned key = (u << 5) | opcode;
        const uint64_t a0 = cpu->v[rn2][0], a1 = cpu->v[rn2][1];
        const uint64_t b0 = cpu->v[rm][0], b1 = cpu->v[rm][1];
        uint64_t r0 = 0, r1 = 0;
        if (key == 3) {                       // logical subgroup
            switch (size) {
                case 0: r0 = a0 & b0;  r1 = a1 & b1;  break;   // and
                case 1: r0 = a0 & ~b0; r1 = a1 & ~b1; break;   // bic
                case 2: r0 = a0 | b0;  r1 = a1 | b1;  break;   // orr
                default: r0 = a0 | ~b0; r1 = a1 | ~b1; break;  // orn
            }
        } else if (key == 35 && size == 0) {  // eor
            r0 = a0 ^ b0; r1 = a1 ^ b1;
        } else if (key == 16 || key == 48 || key == 19) {   // add / sub / mul
            const unsigned lanes = q ? (128u / (8u << size)) : (64u / (8u << size));
            const unsigned width = 8u << size;
            const uint64_t mask = (width >= 64) ? ~0ULL : ((1ULL << width) - 1);
            for (unsigned i = 0; i < lanes; i++) {
                const unsigned bit_off = i * width;
                const int half = bit_off / 64;
                const unsigned off = bit_off % 64;
                const uint64_t av = ((half ? a1 : a0) >> off) & mask;
                const uint64_t bv = ((half ? b1 : b0) >> off) & mask;
                uint64_t rv;
                if (key == 16) rv = (av + bv) & mask;
                else if (key == 48) rv = (av - bv) & mask;
                else rv = (av * bv) & mask;
                if (half) r1 = (r1 & ~(mask << off)) | (rv << off);
                else      r0 = (r0 & ~(mask << off)) | (rv << off);
            }
        } else {
            machine->ctx.reason = "unimplemented SIMD three-same opcode";
            return E2_UNIMPL;
        }
        cpu->v[rd][0] = r0;
        cpu->v[rd][1] = q ? r1 : 0;
        cpu->pc = next_pc;
        machine->instructions++;
        return E2_OK;
    }

    // --- Scalar floating point compare: FCMP / FCMPE ---
    // AArch64 sets NZCV in a specific way for unordered comparisons, which is
    // how C's <, <=, == on floats end up branching.
    if ((insn & 0xFF207C00u) == 0x1E202000u) {
        const unsigned ftype = BITS(insn, 23, 22);
        if (ftype == 0 || ftype == 1) {
            const bool dbl = ftype == 1;
            const unsigned rm = BITS(insn, 20, 16);
            const unsigned rn2 = BITS(insn, 9, 5);
            double a, b;
            if (dbl) {
                a = cpu->v[rn2][0] == 0 ? 0.0 : 0.0;
                memcpy(&a, &cpu->v[rn2][0], 8);
                if (rm == 0) { b = 0.0; } else { memcpy(&b, &cpu->v[rm][0], 8); }
            } else {
                uint32_t ba = (uint32_t)cpu->v[rn2][0];
                float fa; memcpy(&fa, &ba, 4);
                a = (double)fa;
                if (rm == 0) { b = 0.0; }
                else { uint32_t bb = (uint32_t)cpu->v[rm][0]; float fb; memcpy(&fb, &bb, 4); b = (double)fb; }
            }
            if (a != a || b != b) {          // unordered
                cpu->flags.n = false; cpu->flags.z = false;
                cpu->flags.c = true;  cpu->flags.v = true;
            } else if (a == b) {
                cpu->flags.n = false; cpu->flags.z = true;
                cpu->flags.c = true;  cpu->flags.v = false;
            } else if (a < b) {
                cpu->flags.n = true;  cpu->flags.z = false;
                cpu->flags.c = false; cpu->flags.v = false;
            } else {
                cpu->flags.n = false; cpu->flags.z = false;
                cpu->flags.c = true;  cpu->flags.v = false;
            }
            cpu->pc = next_pc;
            machine->instructions++;
            return E2_OK;
        }
    }

    // --- Scalar floating point, two sources: FMUL/FDIV/FADD/FSUB/FMAX/FMIN ---
    // Python's number formatting does its arithmetic through these. Host float
    // and double arithmetic follows the same IEEE rules including NaN and
    // signed zero, so the operands are converted and the result stored back.
    if ((insn & 0xFF200C00u) == 0x1E200800u) {
        const unsigned ftype = BITS(insn, 23, 22);
        const unsigned opcode = BITS(insn, 15, 12);
        const unsigned rm = BITS(insn, 20, 16);
        const unsigned rn2 = BITS(insn, 9, 5);
        if (ftype == 0 || ftype == 1) {
            const bool dbl = ftype == 1;
            #define E2_FP_IN(fr)                                                     \
                (dbl ? ({ uint64_t _b = (fr)[0]; double _d; memcpy(&_d, &_b, 8); _d; })  \
                     : ({ uint32_t _b = (uint32_t)(fr)[0]; float _f; memcpy(&_f, &_b, 4); \
                          (double)_f; }))
            const double a = E2_FP_IN(cpu->v[rn2]);
            const double b = E2_FP_IN(cpu->v[rm]);
            double r;
            switch (opcode) {
                case 0: r = a * b; break;                 // fmul
                case 1: r = a / b; break;                 // fdiv
                case 2: r = a + b; break;                 // fadd
                case 3: r = a - b; break;                 // fsub
                case 4: case 6: r = (a != a) ? b : (b != b) ? a : (a > b ? a : b); break;
                case 5: case 7: r = (a != a) ? b : (b != b) ? a : (a < b ? a : b); break;
                case 8: r = -(a * b); break;              // fnmul
                default:
                    machine->ctx.reason = "unimplemented FP 2-source opcode";
                    return E2_UNIMPL;
            }
            #undef E2_FP_IN
            if (dbl) {
                uint64_t bits; memcpy(&bits, &r, 8);
                cpu->v[rd][0] = bits;
                cpu->v[rd][1] = 0;
            } else {
                float f = (float)r;
                uint32_t bits; memcpy(&bits, &f, 4);
                cpu->v[rd][0] = bits;
                cpu->v[rd][1] = 0;
            }
            cpu->pc = next_pc;
            machine->instructions++;
            return E2_OK;
        }
    }

    // --- Integer to scalar floating point: SCVTF / UCVTF ---
    // Python's float formatting converts integers this way. The host cast is
    // IEEE round-to-nearest-even, which is the architecturally default mode.
    if ((insn & 0x7F3FFC00u) == 0x1E230000u || (insn & 0x7F3FFC00u) == 0x1E220000u) {
        const bool wide = BIT(insn, 31) != 0;
        const bool is_unsigned = BIT(insn, 20) != 0;
        const bool to_double = BIT(insn, 22) != 0;
        const unsigned rn2 = BITS(insn, 9, 5);
        const uint64_t raw = e2_read_xzr(cpu, rn2);
        if (to_double) {
            double dv;
            if (wide) {
                dv = is_unsigned ? (double)(uint64_t)raw : (double)(int64_t)raw;
            } else {
                dv = is_unsigned ? (double)(uint32_t)raw : (double)(int32_t)raw;
            }
            uint64_t bits;
            memcpy(&bits, &dv, 8);
            cpu->v[rd][0] = bits;
            cpu->v[rd][1] = 0;
        } else {
            float fv;
            if (wide) {
                fv = is_unsigned ? (float)(uint64_t)raw : (float)(int64_t)raw;
            } else {
                fv = is_unsigned ? (float)(uint32_t)raw : (float)(int32_t)raw;
            }
            uint32_t bits;
            memcpy(&bits, &fv, 4);
            cpu->v[rd][0] = bits;
            cpu->v[rd][1] = 0;
        }
        cpu->pc = next_pc;
        machine->instructions++;
        return E2_OK;
    }

    // --- FMOV (scalar, immediate) ---
    // The 8-bit immediate expands to a float by a fixed bit pattern: bit7 is the
    // sign, bit6 picks the exponent range, bits5-4 continue the exponent and
    // bits3-0 start the fraction. Verified against the assembler:
    //   #0.5 -> imm8 0x60 -> exp 126,  #2.0 -> imm8 0x00 -> exp 128.
    if ((insn & 0xFF201FE0u) == 0x1E201000u) {
        const unsigned ftype = BITS(insn, 23, 22);
        const unsigned imm8 = BITS(insn, 20, 13);
        const unsigned b6 = (imm8 >> 6) & 1;
        const unsigned c = (imm8 >> 5) & 1;
        const unsigned d = (imm8 >> 4) & 1;
        const uint64_t frac = (uint64_t)(imm8 & 0xF);
        if (ftype == 0) {                      // single
            const unsigned exp = ((1u - b6) << 7) | (b6 ? 0x7Cu : 0u) | (c << 1) | d;
            const uint32_t bits = ((uint32_t)(imm8 >> 7) << 31) | (exp << 23) |
                                  ((uint32_t)frac << 19);
            cpu->v[rd][0] = bits;
            cpu->v[rd][1] = 0;
        } else if (ftype == 1) {               // double
            const unsigned exp = ((1u - b6) << 10) | (b6 ? 0x3FCu : 0u) | (c << 1) | d;
            const uint64_t bits = ((uint64_t)(imm8 >> 7) << 63) | ((uint64_t)exp << 52) |
                                  (frac << 48);
            cpu->v[rd][0] = bits;
            cpu->v[rd][1] = 0;
        } else {
            machine->ctx.reason = "unimplemented FMOV immediate type";
            return E2_UNIMPL;
        }
        cpu->pc = next_pc;
        machine->instructions++;
        return E2_OK;
    }

    // --- FMOV between a general register and a scalar FP register ---
    // Shells and printf format float output through these, so they show up as
    // soon as a program that prints a number runs.
    if ((insn & 0xFFFFFC00u) == 0x1E270000u) {      // fmov Sd, Wn
        const unsigned rn2 = BITS(insn, 9, 5);
        cpu->v[rd][0] = e2_read_xzr(cpu, rn2) & 0xFFFFFFFFULL;
        cpu->v[rd][1] = 0;
        cpu->pc = next_pc; machine->instructions++; return E2_OK;
    }
    if ((insn & 0xFFFFFC00u) == 0x1E260000u) {      // fmov Wd, Sn
        const unsigned rn2 = BITS(insn, 9, 5);
        write_reg(cpu, rd, cpu->v[rn2][0] & 0xFFFFFFFFULL, false);
        cpu->pc = next_pc; machine->instructions++; return E2_OK;
    }
    if ((insn & 0xFFFFFC00u) == 0x9E670000u) {      // fmov Dd, Xn
        const unsigned rn2 = BITS(insn, 9, 5);
        cpu->v[rd][0] = e2_read_xzr(cpu, rn2);
        cpu->v[rd][1] = 0;
        cpu->pc = next_pc; machine->instructions++; return E2_OK;
    }
    if ((insn & 0xFFFFFC00u) == 0x9E660000u) {      // fmov Xd, Dn
        const unsigned rn2 = BITS(insn, 9, 5);
        write_reg(cpu, rd, cpu->v[rn2][0], true);
        cpu->pc = next_pc; machine->instructions++; return E2_OK;
    }

    // --- SIMD: SHLL / SSHLL / USHLL (shift left long, widening) ---
    // printf's integer formatting path widens lanes with sshll before doing
    // arithmetic on them, so this appears as soon as a shell prints a number.
    if ((insn & 0x9F80FC00u) == 0x0F00A400u) {
        const bool q = BIT(insn, 30) != 0;
        const bool is_unsigned = BIT(insn, 29) != 0;
        const unsigned immh = BITS(insn, 22, 19);
        const unsigned immb = BITS(insn, 18, 16);
        const unsigned rn2 = BITS(insn, 9, 5);
        if (immh == 0) {
            machine->ctx.reason = "invalid SHLL immh";
            return E2_UNIMPL;
        }
        // The highest set bit of immh picks the source element width: 1->8,
        // 2->16, 4->32, 8->64. The 7-bit immh:immb is the shift plus that width.
        unsigned hi_bit = 0;
        while (!(immh & (1u << hi_bit))) {
            hi_bit++;
        }
        const unsigned esize = 8u << hi_bit;   // verified: immh 1->8 2->16 4->32 8->64
        const unsigned dsize = esize * 2;
        const unsigned shift = ((immh << 3) | immb) - esize;
        const unsigned lanes = 64u / esize;
        const uint64_t emask = (esize >= 64) ? ~0ULL : ((1ULL << esize) - 1);
        // Q selects the upper half of the source; every destination lane is
        // twice as wide as its source lane.
        for (unsigned i = 0; i < lanes; i++) {
            const unsigned src_idx = (q ? lanes : 0u) + i;
            uint64_t v = (cpu->v[rn2][0] >> (src_idx * esize)) & emask;
            if (!is_unsigned && (v & (1ULL << (esize - 1))) != 0) {
                v |= ~emask;   // sign-extend to 64 bits before shifting
            }
            const uint64_t wide = v << shift;   // v is already 64-bit signed here
            const int half = (src_idx * dsize) / 64;
            const unsigned off = (src_idx * dsize) % 64;
            const uint64_t keep = (dsize >= 64) ? 0 : ~(((1ULL << dsize) - 1) << off);
            cpu->v[rd][half] = (cpu->v[rd][half] & keep) |
                               ((wide & ((dsize >= 64) ? ~0ULL : ((1ULL << dsize) - 1))) << off);
        }
        cpu->pc = next_pc;
        machine->instructions++;
        return E2_OK;
    }

    // --- SIMD: LD1 / ST1 with two structure registers ---
    // memcpy's wide path moves 32 bytes per iteration with this form, so the
    // whole of strcpy/memcpy shows up here. Bits 14-13 = 01 selects two
    // registers, and Q chooses 16- or 8-byte elements.
    if ((insn & 0xFF3FF000u) == 0x4C00A000u) {
        const bool is_load = BIT(insn, 22) != 0;
        const bool q = BIT(insn, 30) != 0;
        const unsigned rn2 = BITS(insn, 9, 5);
        const unsigned base_v = BITS(insn, 4, 0);
        const uint64_t addr = e2_read_sp(cpu, rn2);
        const unsigned bytes = q ? 16u : 8u;
        for (unsigned k = 0; k < 2; k++) {
            const unsigned vreg = (base_v + k) & 31u;
            const uint64_t at = addr + k * bytes;
            if (is_load) {
                uint64_t lo = 0, hi = 0;
                if (e2_memory_read(&machine->mem, at, &lo, 8) != 0 ||
                    (bytes == 16 && e2_memory_read(&machine->mem, at + 8, &hi, 8) != 0)) {
                    machine->ctx.reason = "ld1 outside mapped memory";
                    return E2_FAULT;
                }
                cpu->v[vreg][0] = lo;
                cpu->v[vreg][1] = bytes == 16 ? hi : 0;
            } else {
                if (e2_memory_write(&machine->mem, at, &cpu->v[vreg][0], 8) != 0 ||
                    (bytes == 16 && e2_memory_write(&machine->mem, at + 8, &cpu->v[vreg][1], 8) != 0)) {
                    machine->ctx.reason = "st1 outside mapped memory";
                    return E2_FAULT;
                }
            }
        }
        cpu->pc = next_pc;
        machine->instructions++;
        return E2_OK;
    }

    // --- SIMD: LD1 / ST1, one element into one lane ---
    // `ld1 {v0.d}[1], [x0]` loads a single element into one lane of a register.
    // Index comes from Q and S; the element width is size=00 (32-bit) or
    // 01 (64-bit) in the forms the compiler emits here.
    if ((insn & 0xBF9F8400u) == 0x0D008400u || (insn & 0xBF9F8400u) == 0x0D008000u) {
        const bool is_load = BIT(insn, 22) != 0;
        const unsigned q = BIT(insn, 30);
        const unsigned s_bit = BIT(insn, 12);
        const unsigned size2 = BITS(insn, 11, 10);
        const unsigned rn2 = BITS(insn, 9, 5);
        const unsigned vt = BITS(insn, 4, 0);
        const unsigned esize = (size2 == 0) ? 32u : 64u;
        const unsigned lanes = 128u / esize;
        const unsigned idx = ((q << 1) | s_bit) & (lanes - 1);
        const uint64_t addr = e2_read_sp(cpu, rn2);
        const unsigned bytes = esize / 8;
        if (is_load) {
            uint64_t v = 0;
            if (e2_memory_read(&machine->mem, addr, &v, bytes) != 0) {
                machine->ctx.reason = "ld1 lane outside mapped memory";
                return E2_FAULT;
            }
            const unsigned bit_off = idx * esize;
            const int half = bit_off / 64;
            const unsigned off = bit_off % 64;
            const uint64_t mask = esize == 64 ? ~0ULL : ((1ULL << esize) - 1);
            cpu->v[vt][half] = (cpu->v[vt][half] & ~(mask << off)) | ((v & mask) << off);
        } else {
            const unsigned bit_off = idx * esize;
            const int half = bit_off / 64;
            const unsigned off = bit_off % 64;
            const uint64_t mask = esize == 64 ? ~0ULL : ((1ULL << esize) - 1);
            const uint64_t v = (cpu->v[vt][half] >> off) & mask;
            if (e2_memory_write(&machine->mem, addr, &v, bytes) != 0) {
                machine->ctx.reason = "st1 lane outside mapped memory";
                return E2_FAULT;
            }
        }
        cpu->pc = next_pc;
        machine->instructions++;
        return E2_OK;
    }

    // --- SIMD: MOVI (immediate) ---
    // libc clears vector registers with `movi vD.16b, #0` before using them, so
    // this appears before any vector arithmetic.
    if ((insn & 0x9FF80400u) == 0x0F000400u) {
        const bool q = BIT(insn, 30) != 0;
        const bool op = BIT(insn, 29) != 0;
        const unsigned cmode = BITS(insn, 15, 12);
        const unsigned imm8 = (BITS(insn, 18, 16) << 5) | BITS(insn, 9, 5);
        const unsigned rd_v = BITS(insn, 4, 0);

        cpu->v[rd_v][0] = 0;
        cpu->v[rd_v][1] = 0;

        // cmode selects the element size and shift. The mapping was taken from
        // the disassembler rather than from memory, because the obvious guess
        // (cmode 0 = byte lanes) is wrong: 0xE is byte lanes and 0x0 is 32-bit.
        /*
         *  cmode  op  element  shift
         *  0x0    0   32       -
         *  0x2    0   32       8
         *  0x4    0   32       16
         *  0x6    0   32       24
         *  0x8    0   16       -
         *  0xA    0   16       8
         *  0xC    0   32       msl 8
         *  0xD    0   32       msl 16
         *  0xE    0   8        -
         *  0xE    1   64       -
         *  0xF    x   floating point (not handled here)
         */
        if (cmode == 0xE && !op) {
            const unsigned lanes = q ? 16u : 8u;
            for (unsigned i = 0; i < lanes; i++) {
                e2_v_set(cpu, rd_v, 8, i, imm8);
            }
        } else if (cmode == 0xE && op) {
            // movi Vd.2d, #imm8 - replicate the byte across the whole register.
            uint64_t pattern = 0;
            for (unsigned i = 0; i < 8; i++) {
                pattern = (pattern << 8) | imm8;
            }
            cpu->v[rd_v][0] = pattern;
            cpu->v[rd_v][1] = pattern;
        } else if (cmode == 0x8 || cmode == 0xA) {
            const unsigned shift = (cmode == 0x8) ? 0u : 8u;
            const unsigned lanes = q ? 8u : 4u;
            for (unsigned i = 0; i < lanes; i++) {
                e2_v_set(cpu, rd_v, 16, i, (uint64_t)imm8 << shift);
            }
        } else if (cmode == 0x0 || cmode == 0x2 || cmode == 0x4 || cmode == 0x6) {
            const unsigned shift = (cmode >> 1) * 8u;
            const unsigned lanes = q ? 4u : 2u;
            for (unsigned i = 0; i < lanes; i++) {
                e2_v_set(cpu, rd_v, 32, i, (uint64_t)imm8 << shift);
            }
        } else if (cmode == 0xC || cmode == 0xD) {
            // movi Vd.4s, #imm8, msl #8/#16 - the low bits become ones.
            const unsigned shift = (cmode == 0xC) ? 8u : 16u;
            const uint64_t ones = (1ULL << shift) - 1;
            const unsigned lanes = q ? 4u : 2u;
            for (unsigned i = 0; i < lanes; i++) {
                e2_v_set(cpu, rd_v, 32, i, ((uint64_t)imm8 << shift) | ones);
            }
        } else if (false) {
            // placeholder so the chain still ends in the unimplemented branch
            machine->ctx.reason = "unimplemented MOVI cmode";
            return E2_UNIMPL;
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

    // --- EXTR (extract from a register pair) ---
    // Bit 23 distinguishes this from the bitfield group (100110 vs 100111), and
    // Rn may differ from Rm, so this is not just a rotate.
    if ((insn & 0x7FA00000u) == 0x13800000u) {
        const unsigned rm = BITS(insn, 20, 16);
        const unsigned imms = BITS(insn, 15, 10);
        const unsigned rn2 = BITS(insn, 9, 5);
        const unsigned width = is64 ? 64u : 32u;
        const uint64_t lo_v = e2_read_xzr(cpu, rm) & (is64 ? ~0ULL : 0xFFFFFFFFULL);
        const uint64_t hi_v = e2_read_xzr(cpu, rn2) & (is64 ? ~0ULL : 0xFFFFFFFFULL);
        uint64_t result;
        if (imms == 0) {
            result = lo_v;
        } else {
            result = (lo_v >> imms) | (hi_v << (width - imms));
        }
        write_reg(cpu, rd, result & (is64 ? ~0ULL : 0xFFFFFFFFULL), is64);
        cpu->pc = next_pc;
        machine->instructions++;
        return E2_OK;
    }

    // --- Conditional select: CSEL / CSINC / CSINV / CSNEG ---
    // Ternary operators and min/max compile to these; CSET is CSINC with an
    // inverted condition.
    if ((insn & 0x1FE00000u) == 0x1A800000u) {
        // The four operations are selected by bit 30 together with bit 10 (op2),
        // NOT by bits 30-29: bit 29 is the S field and is always zero here.
        // Reading bit 29 made CSINC look like CSEL, so `cset` (which lowers to
        // `csinc Rd, ZR, ZR, invert(cond)`) returned ZR - always zero.
        const unsigned op = BIT(insn, 30);
        const unsigned op2 = BIT(insn, 10);
        const unsigned rm = BITS(insn, 20, 16);
        const unsigned cond = BITS(insn, 15, 12);
        const unsigned rn2 = BITS(insn, 9, 5);
        const uint64_t mask = is64 ? ~0ULL : 0xFFFFFFFFULL;
        const unsigned sel = (op << 1) | op2;

        uint64_t result;
        if (condition_holds(cpu, cond)) {
            result = e2_read_xzr(cpu, rn2);
        } else {
            const uint64_t other = e2_read_xzr(cpu, rm);
            switch (sel) {
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

    // --- Data processing (2 source): UDIV / SDIV / LSLV / LSRV / ASRV / RORV ---
    // These share the 11010110 prefix in bits 28-21 and are separated by the
    // opcode field in bits 15-10. Variable shifts are common in real code even
    // though the assembler's `lsl xD, xN, xM` form looks exotic.
    if ((insn & 0x7FE00000u) == 0x1AC00000u) {
        const unsigned opcode = BITS(insn, 15, 10);
        const unsigned rm = BITS(insn, 20, 16);
        const unsigned rn2 = BITS(insn, 9, 5);
        const unsigned width = is64 ? 64 : 32;
        const uint64_t mask = is64 ? ~0ULL : 0xFFFFFFFFULL;

        const uint64_t a = e2_read_xzr(cpu, rn2) & mask;
        const uint64_t b = e2_read_xzr(cpu, rm) & mask;
        uint64_t result;
        switch (opcode) {
            case 0x02:  // UDIV
                result = (b == 0) ? 0 : (a / b);
                break;
            case 0x03: {  // SDIV
                if (b == 0) {
                    result = 0;
                } else if (is64) {
                    result = (b == (uint64_t)-1 && a == (1ULL << 63))
                                 ? a
                                 : (uint64_t)((int64_t)a / (int64_t)b);
                } else {
                    const int32_t sa = (int32_t)a;
                    const int32_t sb = (int32_t)b;
                    result = (uint64_t)(int64_t)(sb == -1 ? (int32_t)(0u - (uint32_t)sa) : sa / sb);
                }
                break;
            }
            case 0x08: {  // LSLV
                const unsigned shift = (unsigned)(b & (width - 1));
                result = (a << shift) & mask;
                break;
            }
            case 0x09: {  // LSRV
                const unsigned shift = (unsigned)(b & (width - 1));
                result = a >> shift;
                break;
            }
            case 0x0A: {  // ASRV
                const unsigned shift = (unsigned)(b & (width - 1));
                const uint64_t sign = is64 ? a : (a | (a & (1ULL << 31) ? ~mask : 0));
                result = (uint64_t)((int64_t)sign >> shift) & mask;
                break;
            }
            case 0x0B: {  // RORV
                unsigned shift = (unsigned)(b & (width - 1));
                if (shift == 0) {
                    result = a;
                } else {
                    result = ((a >> shift) | (a << (width - shift))) & mask;
                }
                break;
            }
            default:
                machine->ctx.reason = "unimplemented data-processing (2 source)";
                return E2_UNIMPL;
        }
        write_reg(cpu, rd, result, is64);
        cpu->pc = next_pc;
        machine->instructions++;
        return E2_OK;
    }

    // --- CCMP / CCMN (conditional compare) ---
    // Chains comparisons without branching, e.g. `a <= b && b < c`. When the
    // condition fails the flags are taken from the instruction instead of being
    // computed, which is how short-circuit evaluation stays branch-free.
    if ((insn & 0x3FE00000u) == 0x3A400000u) {
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
    if ((insn & 0x1F600000u) == 0x1B000000u) {
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
    if ((insn & 0x3B000000u) == 0x39000000u && BIT(insn, 26) == 0) {
        const unsigned size_code = BITS(insn, 31, 30);
        const unsigned opc = BITS(insn, 23, 22);
        const bool is_load = opc != 0;
        const uint64_t offset = (uint64_t)BITS(insn, 21, 10) * (1u << size_code);
        const uint64_t addr = cpu->x[rn] + offset;
        const unsigned bytes = 1u << size_code;

        if (is_load) {
            uint64_t value = 0;
            if (e2_memory_read(&machine->mem, addr, &value, bytes) != 0) {
                machine->ctx.reason = "load outside mapped memory";
                return E2_FAULT;
            }
            value = apply_load_opc(value, opc, bytes, size_code == 3);
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
        const unsigned opc = BITS(insn, 23, 22);
        const bool is_load = opc != 0;
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
            value = apply_load_opc(value, opc, bytes, size_code == 3);
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

    // --- SIMD LDR / STR (unscaled / pre-index / post-index) ---
    // The single-register vector forms share the immediate-group layout with the
    // integer ones but set V. libc spills vector state with these around calls.
    if ((insn & 0x3B200000u) == 0x38000000u && BIT(insn, 26) != 0) {
        const bool is_load = BIT(insn, 22) != 0;
        const unsigned size_code = BITS(insn, 31, 30);
        const unsigned mode = BITS(insn, 11, 10);
        const int64_t imm9 = (int64_t)sign_extend(BITS(insn, 20, 12), 9);
        const unsigned bytes = 1u << size_code;
        const unsigned rt_v = BITS(insn, 4, 0);

        uint64_t base = e2_read_sp(cpu, rn);
        uint64_t addr;
        if (mode == 1) {
            addr = base;
        } else {
            addr = (uint64_t)((int64_t)base + imm9);
        }

        const size_t chunk = bytes > 8 ? 8 : bytes;
        if (is_load) {
            if (e2_memory_read(&machine->mem, addr, &cpu->v[rt_v][0], chunk) != 0 ||
                (bytes > 8 &&
                 e2_memory_read(&machine->mem, addr + 8, &cpu->v[rt_v][1], 8) != 0)) {
                machine->ctx.reason = "simd load outside mapped memory";
                return E2_FAULT;
            }
        } else {
            if (e2_memory_write(&machine->mem, addr, &cpu->v[rt_v][0], chunk) != 0 ||
                (bytes > 8 &&
                 e2_memory_write(&machine->mem, addr + 8, &cpu->v[rt_v][1], 8) != 0)) {
                machine->ctx.reason = "simd store outside mapped memory";
                return E2_FAULT;
            }
        }

        if (mode == 1) {
            e2_write_sp(cpu, rn, (uint64_t)((int64_t)base + imm9));
        } else if (mode == 3) {
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
        const unsigned opc = BITS(insn, 23, 22);
        const bool is_load = opc != 0;
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
            // LDRSB / LDRSH / LDRSW are selected by opc, so the sign extension
            // has to go through the same helper as the other load forms. This
            // path used to store the raw bytes, which turned a 0xFF byte into
            // 0xFF instead of -1 - the wrong index every time one was used.
            value = apply_load_opc(value, opc, bytes, size_code == 3);
            const bool dest64 = (opc == 2) || (size_code == 3);
            write_reg(cpu, rd, value, dest64);
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

    // --- Data processing (1 source): RBIT / REV16 / REV32 / REV / CLZ / CLS ---
    // Bit manipulation that has no scalar equivalent in C; compilers emit these
    // for byte-order swaps and bit-scanning loops.
    if ((insn & 0x7FE00000u) == 0x5AC00000u) {
        const unsigned opcode = BITS(insn, 15, 10);
        const unsigned rn2 = BITS(insn, 9, 5);
        const unsigned width = is64 ? 64 : 32;
        const uint64_t mask = is64 ? ~0ULL : 0xFFFFFFFFULL;
        uint64_t v = e2_read_xzr(cpu, rn2) & mask;
        uint64_t result;

        switch (opcode) {
            case 0x00: {  // RBIT
                uint64_t r = 0;
                for (unsigned k = 0; k < width; k++) {
                    if ((v >> k) & 1u) {
                        r |= 1ULL << (width - 1 - k);
                    }
                }
                result = r;
                break;
            }
            case 0x01: {  // REV16: reverse bytes within each halfword
                result = 0;
                for (unsigned k = 0; k < width; k += 16) {
                    const uint64_t hw = (v >> k) & 0xFFFF;
                    result |= ((hw & 0xFF) << 8 | ((hw >> 8) & 0xFF)) << k;
                }
                break;
            }
            case 0x02: {  // REV32 (64-bit) / REV (32-bit): reverse bytes per word
                if (is64) {
                    result = 0;
                    for (unsigned k = 0; k < 64; k += 32) {
                        const uint64_t w = (v >> k) & 0xFFFFFFFFULL;
                        const uint64_t r = ((w & 0xFF) << 24) | ((w & 0xFF00) << 8) |
                                           ((w >> 8) & 0xFF00) | ((w >> 24) & 0xFF);
                        result |= r << k;
                    }
                } else {
                    result = ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) |
                             ((v >> 8) & 0xFF00) | ((v >> 24) & 0xFF);
                }
                break;
            }
            case 0x03: {  // REV (64-bit only): reverse all bytes
                result = 0;
                for (unsigned k = 0; k < 64; k += 8) {
                    result |= ((v >> k) & 0xFF) << (56 - k);
                }
                break;
            }
            case 0x04: {  // CLZ
                unsigned n = 0;
                for (unsigned k = width; k > 0; k--) {
                    if ((v >> (k - 1)) & 1u) {
                        break;
                    }
                    n++;
                }
                result = n;
                break;
            }
            case 0x05: {  // CLS: count leading bits equal to the sign bit
                const unsigned sign = (unsigned)((v >> (width - 1)) & 1u);
                unsigned n = 0;
                for (unsigned k = width; k > 0; k--) {
                    if (((v >> (k - 1)) & 1u) != sign) {
                        break;
                    }
                    n++;
                }
                result = n - 1;
                break;
            }
            default:
                machine->ctx.reason = "unimplemented 1-source data processing";
                return E2_UNIMPL;
        }

        write_reg(cpu, rd, result & mask, is64);
        cpu->pc = next_pc;
        machine->instructions++;
        return E2_OK;
    }

    // --- MRS / MSR (system registers) ---
    // Only the handful a userspace process actually touches. TPIDR_EL0 is the
    // thread pointer, which is how TLS is addressed; libc sets and reads it
    // constantly. Anything else is reported rather than silently ignored,
    // because a wrong value here corrupts every thread-local access.
    if ((insn & 0xFFF00000u) == 0xD5100000u || (insn & 0xFFF00000u) == 0xD5300000u) {
        const bool is_read = BIT(insn, 21) != 0;
        const unsigned sysreg = BITS(insn, 20, 5);
        const unsigned rt = BITS(insn, 4, 0);

        switch (sysreg) {
            case 0xDE82:  // TPIDR_EL0: thread pointer
                if (is_read) {
                    write_reg(cpu, rt, cpu->tpidr_el0, true);
                } else {
                    cpu->tpidr_el0 = e2_read_xzr(cpu, rt);
                }
                break;
            case 0xDE83:  // TPIDRRO_EL0: read-only thread pointer
                if (is_read) {
                    write_reg(cpu, rt, cpu->tpidr_el0, true);
                }
                break;
            case 0x5F01:  // CNTVCT_EL0: virtual counter, monotonic nanoseconds-ish
                if (is_read) {
                    machine->cycle_clock += 1000;
                    write_reg(cpu, rt, machine->cycle_clock, true);
                }
                break;
            case 0xD809:  // CTR_EL0: cache type register, constant
                if (is_read) {
                    write_reg(cpu, rt, 0x8444C004ULL, true);
                }
                break;
            case 0xD807:  // DCZID_EL0: data cache zero id, no DZP
                if (is_read) {
                    write_reg(cpu, rt, 0x4ULL, true);
                }
                break;
            default:
                machine->ctx.reason = "unimplemented system register";
                return E2_UNIMPL;
        }
        cpu->pc = next_pc;
        machine->instructions++;
        return E2_OK;
    }

    // --- LDXR / LDAXR / STXR / STLXR (exclusive access) ---
    // libc's atomics and locks are built on these. With a single interpreter
    // thread there is no way for the reservation to be lost between the load
    // and the store, so the store always reports success; the acquire/release
    // ordering they imply is free because execution is strictly sequential.
    if ((insn & 0x3F000000u) == 0x08000000u) {
        const unsigned size_code = BITS(insn, 31, 30);
        const unsigned rs = BITS(insn, 20, 16);
        const unsigned rn2 = BITS(insn, 9, 5);
        const unsigned rt = BITS(insn, 4, 0);
        const bool is_load = BIT(insn, 22) != 0;
        const unsigned bytes = 1u << size_code;
        const uint64_t addr = e2_read_xzr(cpu, rn2);

        if (is_load) {
            uint64_t value = 0;
            if (e2_memory_read(&machine->mem, addr, &value, bytes) != 0) {
                machine->ctx.reason = "exclusive load outside mapped memory";
                return E2_FAULT;
            }
            write_reg(cpu, rt, value, size_code == 3);
        } else {
            const uint64_t value = e2_read_xzr(cpu, rt);
            if (e2_memory_write(&machine->mem, addr, &value, bytes) != 0) {
                machine->ctx.reason = "exclusive store outside mapped memory";
                return E2_FAULT;
            }
            write_reg(cpu, rs, 0, size_code == 3);  // 0 = succeeded
        }
        cpu->pc = next_pc;
        machine->instructions++;
        return E2_OK;
    }

    // --- DSB / DMB / ISB ---
    // Execution is strictly sequential and the host provides the ordering, so
    // these retire without doing anything. They still have to be recognised:
    // dropping them as unimplemented would stop every atomic sequence.
    if ((insn & 0xFFFFF09Fu) == 0xD503309Fu ||   // DSB / DMB
        insn == 0xD5033FDFu ||                    // ISB
        insn == 0xD5033F9Fu) {                    // ISB (alt encoding)
        cpu->pc = next_pc;
        machine->instructions++;
        return E2_OK;
    }

    // --- DC ZVA (data cache zero by VA) ---
    // Zeroes the cache line containing Xt. memcpy/memset use it to clear whole
    // lines at once. The line size is 64 bytes here, matching the CTR_EL0 value
    // reported above.
    if ((insn & 0xFFFFFFE0u) == 0xD50B7420u) {
        const unsigned rt = BITS(insn, 4, 0);
        const uint64_t addr = e2_read_xzr(cpu, rt) & ~(uint64_t)63;
        static const uint8_t zeros[64] = {0};
        if (e2_memory_write(&machine->mem, addr, zeros, sizeof(zeros)) != 0) {
            machine->ctx.reason = "DC ZVA outside mapped memory";
            return E2_FAULT;
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
