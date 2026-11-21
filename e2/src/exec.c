#include "e2/exec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

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
    // The trampoline the kernel places in a signal frame's return address. Kept in its own
    // page below the stack so nothing the program does to its stack can overwrite it.
    {
        const uint64_t where = 0x7EFFF000ULL;
        if (e2_memory_map_at(&machine->mem, where, E2_PAGE_SIZE,
                             E2_PROT_READ | E2_PROT_WRITE | 4 /* exec */) == 0) {
            const uint32_t code[2] = {0xD2801168u /* mov x8, #139 */, 0xD4000001u /* svc #0 */};
            e2_memory_write(&machine->mem, where, code, sizeof(code));
            machine->sig_trampoline = where;
        }
    }

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

int e2_machine_raise_signal(e2_machine *machine, unsigned signum) {
    if (signum == 0 || signum >= 64) return -22;         // EINVAL
    if (machine->sig_handler[signum] == 1) return 0;     // SIG_IGN
    machine->sig_pending |= (1ULL << signum);
    return 0;
}

int e2_machine_pop_sigframe(e2_machine *machine, e2_sigframe *out) {
    e2_sigframe frame;
    if (e2_memory_read(&machine->mem, machine->cpu.sp, &frame, sizeof(frame)) != 0) {
        return -14;                                      // EFAULT
    }
    if (frame.magic != E2_SIGFRAME_MAGIC) {
        // rt_sigreturn with no frame means the program called it directly. Restoring
        // whatever happened to be on the stack would be worse than failing.
        return -22;
    }
    for (unsigned i = 0; i < 31; i++) machine->cpu.x[i] = frame.x[i];
    machine->cpu.sp = frame.sp;
    machine->cpu.pc = frame.pc;
    if (out != NULL) *out = frame;
    return 0;
}

int e2_deliver_pending_signal(e2_machine *machine) {
    if (machine->sig_pending == 0) return 0;
    for (unsigned sig = 1; sig < 64; sig++) {
        const uint64_t bit = 1ULL << sig;
        if ((machine->sig_pending & bit) == 0) continue;
        machine->sig_pending &= ~bit;
        const uint64_t handler = machine->sig_handler[sig];
        if (handler == 1) continue;                      // ignored
        if (handler == 0) {
            // The default action. Reporting it rather than pretending the signal was
            // handled is what makes a missing handler visible instead of silent. The number
            // is kept so the caller can report 128 + sig, which is what a shell expects -
            // a constant 137 said SIGKILL for every signal.
            machine->ctx.reason = "signal with no handler";
            machine->ctx.svc_number = (uint16_t)sig;
            return -1;
        }
        if (machine->sig_trampoline == 0) return -1;

        e2_sigframe frame;
        memset(&frame, 0, sizeof(frame));
        for (unsigned i = 0; i < 31; i++) frame.x[i] = machine->cpu.x[i];
        frame.sp = machine->cpu.sp;
        frame.pc = machine->cpu.pc;
        frame.magic = E2_SIGFRAME_MAGIC;

        const uint64_t sp = (machine->cpu.sp - sizeof(frame)) & ~(uint64_t)15;
        if (e2_memory_write(&machine->mem, sp, &frame, sizeof(frame)) != 0) return -1;
        machine->cpu.sp = sp;
        machine->cpu.x[0] = sig;        // the handler takes the signal number
        machine->cpu.x[1] = 0;          // no siginfo
        machine->cpu.x[2] = 0;          // no ucontext
        machine->cpu.x[30] = machine->sig_trampoline;
        machine->cpu.pc = handler;
        return 1;
    }
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


    // --- SIMD: DUP (broadcast one vector element across a vector) ---
    // `dup v1.8b, v0.b[5]` copies lane 5 into all eight bytes. It shares the imm5 element
    // encoding with UMOV and SMOV, and was missing entirely - a broadcast that silently
    // produced zeros rather than failing.
    if ((insn & 0x9FE0FC00u) == 0x0E000400u) {
        const bool q = BIT(insn, 30) != 0;
        const unsigned imm5 = BITS(insn, 20, 16);
        const unsigned rn2 = BITS(insn, 9, 5);
        const unsigned rd = BITS(insn, 4, 0);
        if (imm5 != 0) {
            unsigned esize, index;
            if (imm5 & 1) {
                esize = 8;  index = (imm5 >> 1) & 0xF;
            } else if (imm5 & 2) {
                esize = 16; index = (imm5 >> 2) & 0x7;
            } else if (imm5 & 4) {
                esize = 32; index = (imm5 >> 3) & 0x3;
            } else {
                esize = 64; index = (imm5 >> 4) & 0x1;
            }
            const uint64_t elem = e2_v_get(cpu, rn2, esize, index);
            const unsigned lanes = (q ? 128u : 64u) / esize;
            uint64_t lo = 0, hi = 0;
            for (unsigned lane = 0; lane < lanes; lane++) {
                const unsigned bit = (lane * esize) % 64;
                if (lane * esize < 64) {
                    lo |= elem << bit;
                } else {
                    hi |= elem << bit;
                }
            }
            cpu->v[rd][0] = lo;
            cpu->v[rd][1] = q ? hi : 0;
            cpu->pc = next_pc;
            machine->instructions++;
            return E2_OK;
        }
    }

    // --- SIMD: SMOV (vector element to general register, sign extended) ---
    // The same family as UMOV with bit 12 clear. A byte holds -1 as 0xFF; UMOV moves it as
    // 0x000000FF and SMOV as 0xFFFFFFFF, so a program that extracts a signed byte with the
    // wrong one gets a plausible but wrong number rather than a failure.
    if ((insn & 0x9FE0FC00u) == 0x0E002C00u) {
        const unsigned imm5 = BITS(insn, 20, 16);
        const unsigned rn2 = BITS(insn, 9, 5);
        const unsigned rd = BITS(insn, 4, 0);
        unsigned esize, index;
        if (imm5 & 1) {
            esize = 8;  index = (imm5 >> 1) & 0xF;
        } else if (imm5 & 2) {
            esize = 16; index = (imm5 >> 2) & 0x7;
        } else if (imm5 & 4) {
            esize = 32; index = (imm5 >> 3) & 0x3;
        } else {
            machine->ctx.reason = "invalid SMOV imm5";
            return E2_UNIMPL;
        }
        const uint64_t raw = e2_v_get(cpu, rn2, esize, index);
        // Sign-extend from esize to the destination width, which Q selects: 0 for a 32-bit
        // destination, 1 for a 64-bit one.
        int64_t value = (int64_t)raw;
        const int shift = 64 - (int)esize;
        value = (value << shift) >> shift;
        write_reg(cpu, rd, (uint64_t)value, true);
        cpu->pc = next_pc;
        machine->instructions++;
        return E2_OK;
    }

    // --- SIMD: UMOV (vector element to general register) ---
    // musl's memset does `mov x1, v0.d[0]` to materialise the replicated byte
    // pattern, so without this every memset wrote zeros. It used to be decoded
    // as DUP because that rule only tested bits 11-10 of the opcode.
    if ((insn & 0x9FE0FC00u) == 0x0E003C00u) {
        const unsigned imm5 = BITS(insn, 20, 16);
        const unsigned rn2 = BITS(insn, 9, 5);
        unsigned esize, index;
        if (imm5 & 1) {
            esize = 8;  index = (imm5 >> 1) & 0xF;
        } else if (imm5 & 2) {
            esize = 16; index = (imm5 >> 2) & 0x7;
        } else if (imm5 & 4) {
            esize = 32; index = (imm5 >> 3) & 0x3;
        } else if (imm5 & 8) {
            esize = 64; index = (imm5 >> 4) & 0x1;
        } else {
            machine->ctx.reason = "invalid UMOV imm5";
            return E2_UNIMPL;
        }
        // Read through the lane accessor: indexing v[rn][0] directly only ever sees the low
        // 64 bits, so `mov x2, v1.d[1]` returned lane 0 and any element above the first half
        // came back wrong. musl's memset and memcpy reach this path.
        const uint64_t elem = e2_v_get(cpu, rn2, esize, index);
        write_reg(cpu, rd, elem, true);
        cpu->pc = next_pc;
        machine->instructions++;
        return E2_OK;
    }

    // --- SIMD: INS (general register into a vector element) ---
    // The inverse of UMOV, and the same family: bits 15-10 select it. musl's
    // memcpy and every vectorised loop reach for both.
    if ((insn & 0x9FE0FC00u) == 0x0E001C00u) {
        const unsigned imm5 = BITS(insn, 20, 16);
        const unsigned rn2 = BITS(insn, 9, 5);
        unsigned esize, index;
        if (imm5 & 1) {
            esize = 8;  index = (imm5 >> 1) & 0xF;
        } else if (imm5 & 2) {
            esize = 16; index = (imm5 >> 2) & 0x7;
        } else if (imm5 & 4) {
            esize = 32; index = (imm5 >> 3) & 0x3;
        } else if (imm5 & 8) {
            esize = 64; index = (imm5 >> 4) & 0x1;
        } else {
            machine->ctx.reason = "invalid INS imm5";
            return E2_UNIMPL;
        }
        const unsigned bit = index * esize;
        uint64_t *half = (bit >= 64) ? &cpu->v[rd][1] : &cpu->v[rd][0];
        const unsigned shift = bit & 63;
        const uint64_t mask = (esize >= 64) ? ~0ULL : ((1ULL << esize) - 1);
        const uint64_t src = e2_read_xzr(cpu, rn2) & mask;
        *half = (*half & ~(mask << shift)) | (src << shift);
        cpu->pc = next_pc;
        machine->instructions++;
        return E2_OK;
    }

    // --- SIMD: XTN / SQXTN / SQXTUN / UQXTN (narrow by half) ---
    // size encodes the destination element width, not the source: xtn v0.2s
    // (32-bit lanes) is size 2 and xtn v0.8b (8-bit lanes) is size 0.
    if ((insn & 0x9F3FFC00u) == 0x0E212800u || (insn & 0x9F3FFC00u) == 0x0E214800u) {
        const bool q = BIT(insn, 30) != 0;
        const bool unsigned_result = BIT(insn, 29) != 0;
        const bool saturating = (insn & 0x9F3FFC00u) == 0x0E214800u;
        const unsigned size = BITS(insn, 23, 22);
        const unsigned rn_v = BITS(insn, 9, 5);
        const unsigned rd_v = BITS(insn, 4, 0);
        const unsigned dst_esize = 8u << size;
        const unsigned src_esize = dst_esize * 2;
        const unsigned lanes = 64u / dst_esize;
        const uint64_t dst_mask = (dst_esize >= 64) ? ~0ULL : ((1ULL << dst_esize) - 1);
        for (unsigned l = 0; l < lanes; l++) {
            const uint64_t raw = e2_v_get(cpu, rn_v, src_esize, l);
            uint64_t out;
            if (!saturating) {
                out = raw & dst_mask;
            } else {
                // Saturate into the destination range: signed clamps to
                // [-2^(n-1), 2^(n-1)-1], unsigned to [0, 2^n-1].
                //
                // The value has to be interpreted at the SOURCE width first. Taking the
                // zero-extended lane and clamping it as signed turned every negative
                // input into a large positive one, so SQXTN answered 32767 where
                // -32768 was correct. Sign extension is the difference between the two.
                const int64_t lo_uns = 0;
                const int64_t hi_uns = (int64_t)dst_mask;
                const int64_t lo_sgn = -(int64_t)(1ULL << (dst_esize - 1));
                const int64_t hi_sgn = (int64_t)(dst_mask >> 1);
                int64_t value;
                if (unsigned_result) {
                    value = (int64_t)raw;   // already zero-extended at the source width
                } else {
                    // Sign-extend from src_esize.
                    const unsigned shift = 64u - src_esize;
                    value = (int64_t)(raw << shift) >> shift;
                }
                const int64_t lo = unsigned_result ? lo_uns : lo_sgn;
                const int64_t hi = unsigned_result ? hi_uns : hi_sgn;
                const int64_t clamped = value < lo ? lo : (value > hi ? hi : value);
                out = (uint64_t)clamped & dst_mask;
            }
            const unsigned index = (q ? lanes : 0u) + l;
            e2_v_set(cpu, rd_v, dst_esize, index, out);
        }
        cpu->pc = next_pc;
        machine->instructions++;
        return E2_OK;
    }

    // --- SIMD: UZP / TRN / ZIP (three-register permutes) ---
    // One encoding family; bits 15-12 pick the permutation. CPython's string and
    // hash helpers interleave lanes with these, so a silent gap here stops the
    // interpreter a few million instructions in.
    if ((insn & 0xBF200C00u) == 0x0E000800u) {
        const bool q = BIT(insn, 30) != 0;
        const unsigned size = BITS(insn, 23, 22);
        const unsigned esize = 8u << size;
        const unsigned opcode = BITS(insn, 15, 12);
        const unsigned rm_v = BITS(insn, 20, 16);
        const unsigned rn_v = BITS(insn, 9, 5);
        const unsigned rd_v = BITS(insn, 4, 0);
        const unsigned lanes = (q ? 128u : 64u) / esize;
        const unsigned half = lanes / 2;
        uint64_t a[16], b[16], out[16];
        for (unsigned l = 0; l < lanes; l++) {
            a[l] = e2_v_get(cpu, rn_v, esize, l);
            b[l] = e2_v_get(cpu, rm_v, esize, l);
        }
        switch (opcode) {
        case 1:  // UZP1: the even-numbered elements of A, then of B. Concatenated, not
            // interleaved - interleaving here is what TRN1 does, and getting the two
            // confused scrambles byte buffers while leaving most tools working.
            for (unsigned i = 0; i < half; i++) {
                out[i] = a[2 * i];
                out[half + i] = b[2 * i];
            }
            break;
        case 5:  // UZP2: the odd-numbered elements of A, then of B.
            for (unsigned i = 0; i < half; i++) {
                out[i] = a[2 * i + 1];
                out[half + i] = b[2 * i + 1];
            }
            break;
        case 2:  // TRN1
            for (unsigned i = 0; i < half; i++) { out[2 * i] = a[2 * i]; out[2 * i + 1] = b[2 * i]; }
            break;
        case 6:  // TRN2
            for (unsigned i = 0; i < half; i++) { out[2 * i] = a[2 * i + 1]; out[2 * i + 1] = b[2 * i + 1]; }
            break;
        case 3:  // ZIP1: interleave the LOW half of each source. ZIP interleaves and UZP
            // concatenates; the two had been swapped here, which is invisible on a buffer
            // whose halves happen to agree and scrambles every other one.
            for (unsigned i = 0; i < half; i++) { out[2 * i] = a[i]; out[2 * i + 1] = b[i]; }
            break;
        case 7:  // ZIP2: interleave the HIGH half of each source.
            for (unsigned i = 0; i < half; i++) {
                out[2 * i] = a[i + half];
                out[2 * i + 1] = b[i + half];
            }
            break;
        default:
            machine->ctx.reason = "invalid permute opcode";
            return E2_UNIMPL;
        }
        for (unsigned l = 0; l < lanes; l++) {
            e2_v_set(cpu, rd_v, esize, l, out[l]);
        }
        cpu->pc = next_pc;
        machine->instructions++;
        return E2_OK;
    }

    // --- SIMD: INS (element into element) ---
    // Distinct from INS (general) above: the source is another vector's lane,
    // not a general register. Both imm5 and imm4 carry element size and index,
    // so they have to stay outside the mask - testing them fixed the family
    // against one encoding only.
    if ((insn & 0xFF008C00u) == 0x6E000400u) {
        const unsigned imm5 = BITS(insn, 20, 16);
        const unsigned imm4 = BITS(insn, 14, 11);
        const unsigned rn_v = BITS(insn, 9, 5);
        const unsigned rd_v = BITS(insn, 4, 0);
        unsigned esize;
        unsigned dst_index, src_index;
        if (imm5 & 1) {
            esize = 8;  dst_index = (imm5 >> 1) & 0xF; src_index = imm4 & 0xF;
        } else if (imm5 & 2) {
            esize = 16; dst_index = (imm5 >> 2) & 0x7; src_index = (imm4 >> 1) & 0x7;
        } else if (imm5 & 4) {
            esize = 32; dst_index = (imm5 >> 3) & 0x3; src_index = (imm4 >> 2) & 0x3;
        } else if (imm5 & 8) {
            esize = 64; dst_index = (imm5 >> 4) & 0x1; src_index = (imm4 >> 3) & 0x1;
        } else {
            machine->ctx.reason = "invalid INS element imm5";
            return E2_UNIMPL;
        }
        const uint64_t value = e2_v_get(cpu, rn_v, esize, src_index);
        e2_v_set(cpu, rd_v, esize, dst_index, value);
        cpu->pc = next_pc;
        machine->instructions++;
        return E2_OK;
    }

    // --- SIMD: DUP (general) ---
    // `dup v0.16b, w1` is the first instruction of musl's memset, so this is the
    // entry point for vector support.
    if ((insn & 0x9FF0FC00u) == 0x0E000C00u) {
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

    // --- SIMD scalar FCVTZS / FCVTZU (FP register destination) ---
    // Distinct from the general-register form below: here the integer result
    // lands in an FP register. awk's int() is exactly this pair -
    // `fcvtzs d8, d9` then `scvtf d8, d8` - so without it the raw double bits
    // survive and every integral value prints as its IEEE bit pattern.
    if ((insn & 0xFFBFFC00u) == 0x5EA1B800u || (insn & 0xFFBFFC00u) == 0x7EA1B800u) {
        const bool is_unsigned = BIT(insn, 31) != 0;
        const bool dbl = BIT(insn, 22) != 0;
        const unsigned rn2 = BITS(insn, 9, 5);
        const unsigned rd2 = BITS(insn, 4, 0);
        double v;
        if (dbl) {
            uint64_t b = cpu->v[rn2][0];
            memcpy(&v, &b, 8);
        } else {
            uint32_t b = (uint32_t)cpu->v[rn2][0];
            float f;
            memcpy(&f, &b, 4);
            v = (double)f;
        }
        uint64_t result;
        if (v != v) {
            result = 0;
        } else if (is_unsigned) {
            if (v <= 0.0) result = 0;
            else if (dbl && v >= 18446744073709551615.0) result = ~0ULL;
            else if (!dbl && v >= 4294967295.0) result = 0xFFFFFFFFULL;
            else result = (uint64_t)v;
        } else {
            if (dbl) {
                if (v <= -9223372036854775808.0) result = 0x8000000000000000ULL;
                else if (v >= 9223372036854775807.0) result = 0x7FFFFFFFFFFFFFFFULL;
                else result = (uint64_t)(int64_t)v;
            } else {
                if (v <= -2147483648.0) result = (uint64_t)(int64_t)(int32_t)0x80000000;
                else if (v >= 2147483647.0) result = 0x7FFFFFFFULL;
                else result = (uint64_t)(int64_t)(int32_t)v;
            }
        }
        cpu->v[rd2][0] = result;
        cpu->v[rd2][1] = 0;
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
        if (key == 35 && size != 0) {         // BSL / BIT / BIF: same bits as the logical
            // group but with U set. `size` picks which of the three.
            switch (size) {
                case 1:                        // BSL: the old destination selects
                    r0 = (cpu->v[rd][0] & a0) | (~cpu->v[rd][0] & b0);
                    r1 = (cpu->v[rd][1] & a1) | (~cpu->v[rd][1] & b1);
                    break;
                case 2:                        // BIT: where Vn is 1, take Vm
                    r0 = (a0 & b0) | (~a0 & cpu->v[rd][0]);
                    r1 = (a1 & b1) | (~a1 & cpu->v[rd][1]);
                    break;
                default:                       // BIF: where Vn is 0, take Vm
                    r0 = (a0 & cpu->v[rd][0]) | (~a0 & b0);
                    r1 = (a1 & cpu->v[rd][1]) | (~a1 & b1);
                    break;
            }
        } else if (key == 3) {                // logical subgroup
            switch (size) {
                case 0: r0 = a0 & b0;  r1 = a1 & b1;  break;   // and
                case 1: r0 = a0 & ~b0; r1 = a1 & ~b1; break;   // bic
                case 2: r0 = a0 | b0;  r1 = a1 | b1;  break;   // orr
                default: r0 = a0 | ~b0; r1 = a1 | ~b1; break;  // orn
            }
        } else if (key == 35 && size == 0) {  // eor
            r0 = a0 ^ b0; r1 = a1 ^ b1;
        } else if (key == 49) {              // cmeq: per-element equality
            // Marks each equal element with all ones. A regex engine tests a
            // byte against a set this way, so without it a pattern can never
            // match a literal character - it simply reports no match, silently.
            const unsigned width = 8u << size;
            const unsigned lanes = (q ? 128u : 64u) / width;
            const uint64_t mask = (width >= 64) ? ~0ULL : ((1ULL << width) - 1);
            for (unsigned i = 0; i < lanes; i++) {
                const unsigned bit_off = i * width;
                const int half = bit_off / 64;
                const unsigned off = bit_off % 64;
                const uint64_t av = ((half ? a1 : a0) >> off) & mask;
                const uint64_t bv = ((half ? b1 : b0) >> off) & mask;
                const uint64_t rv = (av == bv) ? mask : 0;
                if (half) r1 = (r1 & ~(mask << off)) | (rv << off);
                else      r0 = (r0 & ~(mask << off)) | (rv << off);
            }
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
        if (getenv("E2_DBG_3SAME"))
            fprintf(stderr, "[3same] insn=0x%08x key=%u size=%u rd=%u rn=%u rm=%u\n",
                    insn, key, size, rd, rn2, rm);
        cpu->v[rd][0] = r0;
        cpu->v[rd][1] = q ? r1 : 0;
        cpu->pc = next_pc;
        machine->instructions++;
        return E2_OK;
    }

    // --- Scalar floating point conditional compare: FCCMP / FCCMPE ---
    // Compare when the condition holds, otherwise load NZCV from the instruction's own
    // immediate. Compilers emit these to chain float comparisons without a branch between them,
    // and it showed up as an unimplemented instruction the first time a program did.
    if ((insn & 0xFF200C00u) == 0x1E200400u) {
        const unsigned ftype = BITS(insn, 23, 22);
        const unsigned rm = BITS(insn, 20, 16);
        const unsigned cond = BITS(insn, 15, 12);
        const unsigned rn = BITS(insn, 9, 5);
        const unsigned nzcv = BITS(insn, 3, 0);
        const bool signalling = BIT(insn, 4) != 0;
        if (ftype == 3) {
            machine->ctx.reason = "invalid fccmp ftype";
            return E2_UNIMPL;
        }
        if (!condition_holds(cpu, cond)) {
            // The condition failed: NZCV comes from the instruction's own immediate, which is
            // how a chained comparison carries the previous result forward.
            cpu->flags.n = (nzcv >> 3) & 1;
            cpu->flags.z = (nzcv >> 2) & 1;
            cpu->flags.c = (nzcv >> 1) & 1;
            cpu->flags.v = nzcv & 1;
            cpu->pc = next_pc;
            machine->instructions++;
            return E2_OK;
        }
        // The condition held: compare, with the same NaN and equality rules FCMP uses.
        double a, b;
        if (ftype == 0) {
            uint32_t ba = (uint32_t)cpu->v[rn][0];
            uint32_t bb = (uint32_t)cpu->v[rm][0];
            float fa, fb;
            memcpy(&fa, &ba, 4);
            memcpy(&fb, &bb, 4);
            a = (double)fa;
            b = (double)fb;
        } else {
            memcpy(&a, &cpu->v[rn][0], 8);
            memcpy(&b, &cpu->v[rm][0], 8);
        }
        (void)signalling;
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

    // --- Scalar floating point compare: FCMP / FCMPE ---
    // AArch64 sets NZCV in a specific way for unordered comparisons, which is
    // how C's <, <=, == on floats end up branching.
    if ((insn & 0xFF207C00u) == 0x1E202000u) {
        const unsigned ftype = BITS(insn, 23, 22);
        if (ftype == 0 || ftype == 1) {
            const bool dbl = ftype == 1;
            const unsigned rm = BITS(insn, 20, 16);
            const unsigned rn2 = BITS(insn, 9, 5);
            // Bit 3 selects the zero form: `fcmp d8, #0.0` is 0x...2118 while
            // `fcmp d8, d0` is 0x...2110. Testing Rm == 0 instead looked right
            // and was wrong - d0 is a real register whose number is zero - so
            // every comparison against d0 silently became a comparison against
            // zero and float branches went the opposite way.
            const bool zero_form = BIT(insn, 3) != 0;
            double a, b;
            if (dbl) {
                memcpy(&a, &cpu->v[rn2][0], 8);
                if (zero_form) { b = 0.0; } else { memcpy(&b, &cpu->v[rm][0], 8); }
            } else {
                uint32_t ba = (uint32_t)cpu->v[rn2][0];
                float fa; memcpy(&fa, &ba, 4);
                a = (double)fa;
                if (zero_form) { b = 0.0; }
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
    // --- Integer to floating point, fixed point (SCVTF/UCVTF with #fbits) ---
    // `%g` on a small number reaches `scvtf d0, w0, #2`: the integer is scaled
    // down by 2^fbits, and fbits is encoded as (64 - fbits) in bits 15-10.
    if ((insn & 0xFF3F0000u) == 0x1E020000u || (insn & 0xFF3F0000u) == 0x1E030000u) {
        const bool is_unsigned = BIT(insn, 16) != 0;
        const bool from_64 = BIT(insn, 31) != 0;
        const bool single = (BIT(insn, 22) == 0);
        const unsigned rn2 = BITS(insn, 9, 5);
        const unsigned rd2 = BITS(insn, 4, 0);
        const unsigned fbits = 64u - BITS(insn, 15, 10);
        const double scale = (fbits == 0) ? 1.0 : (double)(1ULL << fbits);
        double v;
        if (is_unsigned) {
            const uint64_t raw = e2_read_xzr(cpu, rn2);
            v = from_64 ? (double)raw : (double)(uint32_t)raw;
        } else {
            const uint64_t raw = e2_read_xzr(cpu, rn2);
            v = from_64 ? (double)(int64_t)raw
                        : (double)(int32_t)(uint32_t)raw;
        }
        v /= scale;
        if (single) {
            const float f = (float)v;
            uint32_t raw;
            memcpy(&raw, &f, 4);
            cpu->v[rd2][0] = (cpu->v[rd2][0] & ~0xFFFFFFFFULL) | raw;
        } else {
            memcpy(&cpu->v[rd2][0], &v, 8);
        }
        cpu->pc = next_pc;
        machine->instructions++;
        return E2_OK;
    }

    // --- SIMD shift by immediate: SHL / SSHR / USHR ---
    // printf's float formatting scales mantissas with `shl d1, d1, #52`. The
    // element size lives in immh, so the shift amount is (imm - element size)
    // rather than the raw immediate.
    // immh == 0000 is not a shift by immediate at all - that encoding belongs to
    // movi/mvni, which share this mask. Requiring it non-zero is the distinction
    // the architecture itself uses.
    // Bit 30 is set in every form of this family and clear in the FMADD
    // encodings that share the mask; immh == 0000 is movi/mvni.
    // Q selects 16 bytes versus 8, and both are ordinary forms of this family -
    // `sshr v1.2s, v8.2s, #8` has Q clear. Requiring Q had excluded the 64-bit forms
    // entirely, which surfaced only when a workload used one.
    if ((insn & 0x8F000400u) == 0x0F000400u && BITS(insn, 22, 19) != 0) {
        const bool u = BIT(insn, 29) != 0;
        const bool scalar = BIT(insn, 28) != 0;
        const unsigned opcode = BITS(insn, 15, 11);
        const unsigned imm = (BITS(insn, 22, 16));
        // The mask is a family marker, not an opcode test: FMADD and the
        // widening shifts share it. Claim the instruction only when the opcode
        // is one of ours, and otherwise fall through so their own rules run -
        // claiming them here turned a working fmadd into an error.
        const bool is_shl = (!u && opcode == 0x0A);
        const bool is_sshr = (!u && opcode == 0x00);
        const bool is_ushr = (u && opcode == 0x00);
        if (!is_shl && !is_sshr && !is_ushr) {
            goto not_a_shift;
        }
        unsigned esize;
        if (BIT(insn, 22)) esize = 64;
        else if (BIT(insn, 21)) esize = 32;
        else if (BIT(insn, 20)) esize = 16;
        else esize = 8;
        // Left and right shifts pack the amount differently: SHL stores
        // (esize + shift), the right shifts store (2*esize - shift). Treating
        // both as (imm - esize) turned `ushr d0, d0, #4` into a shift by 60.
        const unsigned shift = is_shl ? (imm - esize) : (2u * esize - imm);
        const unsigned rd2 = BITS(insn, 4, 0);
        const unsigned rn2 = BITS(insn, 9, 5);
        // Shifting everything out is legal and yields zero; it is not an error.
        const unsigned eff = (shift >= esize) ? esize : shift;
        const uint64_t mask = (esize == 64) ? ~0ULL : ((1ULL << esize) - 1);
        const uint64_t src_lo = cpu->v[rn2][0];
        const uint64_t src_hi = cpu->v[rn2][1];
        if (scalar) {
            const uint64_t lane = src_lo & mask;
            uint64_t out;
            if (!u && opcode == 0x0A) {          // SHL
                out = (eff >= esize) ? 0 : ((lane << eff) & mask);
            } else if (!u && opcode == 0x00) {   // SSHR
                const int64_t signed_lane =
                    (esize == 64) ? (int64_t)lane
                                  : (int64_t)((lane ^ (1ULL << (esize - 1))) - (1ULL << (esize - 1)));
                out = (uint64_t)(signed_lane >> eff) & mask;
            } else if (u && opcode == 0x00) {    // USHR
                out = (lane >> eff) & mask;
            } else {
                goto not_a_shift;
            }
            cpu->v[rd2][0] = out;
            cpu->v[rd2][1] = 0;
        } else {
            uint64_t out_lo = 0, out_hi = 0;
            const unsigned per_half = (esize == 64) ? 1u : (64u / esize);
            for (unsigned h = 0; h < 2; h++) {
                const uint64_t src = (h == 0) ? src_lo : src_hi;
                uint64_t acc = 0;
                for (unsigned l = 0; l < per_half; l++) {
                    const unsigned bit = l * esize;
                    const uint64_t lane = (src >> bit) & mask;
                    uint64_t res;
                    if (!u && opcode == 0x0A) {
                        res = (eff >= esize) ? 0 : ((lane << eff) & mask);
                    } else if (!u && opcode == 0x00) {
                        const int64_t signed_lane =
                            (esize == 64) ? (int64_t)lane
                                          : (int64_t)((lane ^ (1ULL << (esize - 1))) - (1ULL << (esize - 1)));
                        res = (uint64_t)(signed_lane >> eff) & mask;
                    } else if (u && opcode == 0x00) {
                        res = (lane >> eff) & mask;
                    } else {
                        goto not_a_shift;
                    }
                    acc |= res << bit;
                }
                if (h == 0) out_lo = acc; else out_hi = acc;
            }
            cpu->v[rd2][0] = out_lo;
            cpu->v[rd2][1] = out_hi;
        }
        cpu->pc = next_pc;
        machine->instructions++;
        return E2_OK;
    }
not_a_shift:;

    // --- FP to integer, written back into an FP register ---
    // Distinct from `fcvtzs x0, d0` (which targets a general register): these
    // convert to an integer and store the result as a floating-point value.
    // `sleep` reaches them turning a double into whole seconds. Rounding mode
    // comes from the opcode, so they are matched by complete key again.
    {
        const uint32_t fkey = insn & 0xFFBFFC00u;
        enum { FCVTZ_S, FCVTZ_U, FCVTM_S, FCVTP_S, FCVTN_S, FCVTA_S };
        static const struct { uint32_t key; int op; } conv[] = {
            {0x5EA1B800u, FCVTZ_S},   // toward zero, signed
            {0x7EA1B800u, FCVTZ_U},   // toward zero, unsigned
            {0x5E21B800u, FCVTM_S},   // toward -inf
            {0x5EA1A800u, FCVTP_S},   // toward +inf
            {0x5E21A800u, FCVTN_S},   // to nearest, ties to even
            {0x5E21C800u, FCVTA_S},   // to nearest, ties away from zero
        };
        for (unsigned ci = 0; ci < sizeof(conv) / sizeof(conv[0]); ci++) {
            if (conv[ci].key != fkey) continue;
            // Bit 22 is the size here; bit 23 is part of the opcode, unlike
            // the one-source group where 23-22 is the type field.
            const bool single = (BIT(insn, 22) == 0);
            const unsigned rn2 = BITS(insn, 9, 5);
            const unsigned rd2 = BITS(insn, 4, 0);
            double v;
            if (single) {
                const uint32_t raw = (uint32_t)cpu->v[rn2][0];
                float f;
                memcpy(&f, &raw, 4);
                v = (double)f;
            } else {
                memcpy(&v, &cpu->v[rn2][0], 8);
            }
            // NaN and overflow saturate rather than trapping; that is what the
            // architecture says and what formatting code expects.
            double out;
            if (v != v) {
                out = 0.0;
            } else if (conv[ci].op == FCVTZ_U) {
                if (v <= 0.0) out = 0.0;
                else if (v >= 18446744073709551615.0) out = (double)(int64_t)-1;
                else out = (double)(uint64_t)trunc(v);
            } else {
                double r;
                switch (conv[ci].op) {
                    case FCVTZ_S: r = trunc(v); break;
                    case FCVTM_S: r = floor(v); break;
                    case FCVTP_S: r = ceil(v); break;
                    case FCVTN_S: r = rint(v); break;
                    default: r = round(v); break;
                }
                if (r > 9.2233720368547758e18) r = 9.2233720368547758e18;
                if (r < -9.2233720368547758e18) r = -9.2233720368547758e18;
                out = (double)(int64_t)r;
            }
            if (single) {
                const float f2 = (float)out;
                uint32_t raw;
                memcpy(&raw, &f2, 4);
                cpu->v[rd2][0] = (cpu->v[rd2][0] & ~0xFFFFFFFFULL) | raw;
            } else {
                memcpy(&cpu->v[rd2][0], &out, 8);
            }
            cpu->pc = next_pc;
            machine->instructions++;
            return E2_OK;
        }
    }

    // --- Floating-point, three sources: FMADD / FMSUB / FNMADD / FNMSUB ---
    // One group, selected by two bits: bit 21 subtracts the addend and bit 15
    // negates the product. The multiply and the add are fused, so the product is
    // not rounded before the addition - that is the whole point of using these.
    if ((insn & 0xFF800000u) == 0x1F000000u) {
        const bool single = (BITS(insn, 23, 22) == 0);
        const bool negate_product = BIT(insn, 15) != 0;
        const bool subtract = BIT(insn, 21) != 0;
        const unsigned rm = BITS(insn, 20, 16);
        const unsigned ra = BITS(insn, 14, 10);
        const unsigned rn2 = BITS(insn, 9, 5);
        const unsigned rd2 = BITS(insn, 4, 0);
        if (single) {
            float a, b, add;
            const uint32_t araw = (uint32_t)cpu->v[rn2][0];
            const uint32_t braw = (uint32_t)cpu->v[rm][0];
            const uint32_t craw = (uint32_t)cpu->v[ra][0];
            memcpy(&a, &araw, 4);
            memcpy(&b, &braw, 4);
            memcpy(&add, &craw, 4);
            if (negate_product) a = -a;
            if (subtract) add = -add;
            const float result = fmaf(a, b, add);
            uint32_t bits;
            memcpy(&bits, &result, 4);
            cpu->v[rd2][0] = (cpu->v[rd2][0] & ~0xFFFFFFFFULL) | bits;
        } else {
            double a, b, add;
            memcpy(&a, &cpu->v[rn2][0], 8);
            memcpy(&b, &cpu->v[rm][0], 8);
            memcpy(&add, &cpu->v[ra][0], 8);
            if (negate_product) a = -a;
            if (subtract) add = -add;
            const double result = fma(a, b, add);
            memcpy(&cpu->v[rd2][0], &result, 8);
        }
        cpu->pc = next_pc;
        machine->instructions++;
        return E2_OK;
    }

    // --- Floating-point, one source ---
    // Move, negate, absolute value, square root, format conversion and the
    // seven rounding modes all share one encoding. They are matched against a
    // table of complete keys rather than by masking out an opcode field: doing
    // that wrong once made `ldr q0, [x21, x2]` run as an integer load.
    {
        const uint32_t key = insn & 0xFF3FFC00u;
        enum { OP_FRINTN, OP_FRINTP, OP_FRINTM, OP_FRINTZ, OP_FRINTA,
               OP_FRINTX, OP_FRINTI, OP_FMOV, OP_FABS, OP_FNEG, OP_FSQRT,
               OP_FCVT_NARROW, OP_FCVT_WIDEN };
        static const struct { uint32_t key; int op; } fops[] = {
            {0x1E244000u, OP_FRINTN},   // to nearest, ties to even
            {0x1E24C000u, OP_FRINTP},   // toward +inf
            {0x1E254000u, OP_FRINTM},   // toward -inf
            {0x1E25C000u, OP_FRINTZ},   // toward zero
            {0x1E264000u, OP_FRINTA},   // to nearest, ties away from zero
            {0x1E274000u, OP_FRINTX},   // like frintn, reports inexact
            {0x1E27C000u, OP_FRINTI},   // like frintz, reports inexact
            {0x1E204000u, OP_FMOV},     // register to register
            {0x1E20C000u, OP_FABS},
            {0x1E214000u, OP_FNEG},
            {0x1E21C000u, OP_FSQRT},
            {0x1E224000u, OP_FCVT_NARROW},  // s <- d
            {0x1E22C000u, OP_FCVT_WIDEN},   // d <- s
        };
        for (unsigned fi = 0; fi < sizeof(fops) / sizeof(fops[0]); fi++) {
            if (fops[fi].key != key) {
                continue;
            }
            const unsigned type = BITS(insn, 23, 22);
            const unsigned rn2 = BITS(insn, 9, 5);
            const unsigned rd2 = BITS(insn, 4, 0);
            const bool single = (type == 0);
            double d;
            if (single) {
                const uint32_t raw = (uint32_t)cpu->v[rn2][0];
                float f;
                memcpy(&f, &raw, 4);
                d = (double)f;
            } else {
                memcpy(&d, &cpu->v[rn2][0], 8);
            }
            uint64_t raw_src = cpu->v[rn2][0];
            switch (fops[fi].op) {
                case OP_FRINTN: d = rint(d); break;
                case OP_FRINTP: d = ceil(d); break;
                case OP_FRINTM: d = floor(d); break;
                case OP_FRINTZ: d = trunc(d); break;
                case OP_FRINTA: d = round(d); break;
                case OP_FRINTX: d = rint(d); break;
                case OP_FRINTI: d = trunc(d); break;
                // Move, negate and absolute value are exact bit operations on
                // the sign, so they never round and never touch the payload.
                case OP_FMOV: d = single ? (double)(uint32_t)raw_src
                                        : ({ double t; memcpy(&t, &raw_src, 8); t; });
                              break;
                case OP_FABS:
                    if (single) {
                        d = (double)(uint32_t)(raw_src & 0x7FFFFFFFu);
                    } else {
                        uint64_t t = raw_src & 0x7FFFFFFFFFFFFFFFULL;
                        memcpy(&d, &t, 8);
                    }
                    break;
                case OP_FNEG:
                    if (single) {
                        d = (double)(uint32_t)(raw_src ^ 0x80000000u);
                    } else {
                        uint64_t t = raw_src ^ 0x8000000000000000ULL;
                        memcpy(&d, &t, 8);
                    }
                    break;
                case OP_FSQRT: d = sqrt(single ? fabs(d) : d); break;
                case OP_FCVT_NARROW: d = (double)(float)d; break;   // s <- d
                case OP_FCVT_WIDEN: d = d; break;                   // d <- s
                default: break;
            }
            if (single) {
                const float f = (float)d;
                uint32_t raw;
                memcpy(&raw, &f, 4);
                cpu->v[rd2][0] = (cpu->v[rd2][0] & ~0xFFFFFFFFULL) | raw;
            } else {
                memcpy(&cpu->v[rd2][0], &d, 8);
            }
            cpu->pc = next_pc;
            machine->instructions++;
            return E2_OK;
        }
    }

    // --- FCSEL: floating-point conditional select ---
    // Picks one of two FP registers by condition code. `fcsel d0, d0, d1, mi`
    // shows up in numeric code that branches on a comparison it just made.
    if ((insn & 0xFF200C00u) == 0x1E200C00u) {
        const unsigned type = BITS(insn, 23, 22);
        const unsigned rm = BITS(insn, 20, 16);
        const unsigned cond = BITS(insn, 15, 12);
        const unsigned rn2 = BITS(insn, 9, 5);
        const unsigned rd2 = BITS(insn, 4, 0);
        const bool single = (type == 0);
        const uint64_t a = single ? (cpu->v[rn2][0] & 0xFFFFFFFFu) : cpu->v[rn2][0];
        const uint64_t b = single ? (cpu->v[rm][0] & 0xFFFFFFFFu) : cpu->v[rm][0];
        const uint64_t pick = condition_holds(cpu, cond) ? a : b;
        if (single) {
            cpu->v[rd2][0] = (cpu->v[rd2][0] & ~0xFFFFFFFFULL) | pick;
        } else {
            cpu->v[rd2][0] = pick;
        }
        cpu->pc = next_pc;
        machine->instructions++;
        return E2_OK;
    }

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
                // FMAX/FMIN propagate a NaN; the "number" variants FMAXNM/FMINNM
                // return the other operand instead. Treating both as the *NM form
                // is wrong whenever an input is NaN.
                case 4: case 5: {
                    const bool is_max = (opcode == 4);
                    if (a != a || b != b) {
                        r = a + b;   // quiet NaN
                    } else {
                        r = is_max ? (a > b ? a : b) : (a < b ? a : b);
                    }
                    break;
                }
                case 6: case 7: {
                    const bool is_max = (opcode == 6);
                    if (a != a) r = b;
                    else if (b != b) r = a;
                    else r = is_max ? (a > b ? a : b) : (a < b ? a : b);
                    break;
                }
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
        // scvtf and ucvtf differ in bit 16: 0x9E62_0000 against 0x9E63_0000.
        // Testing bit 20 instead made every ucvtf run as signed, so an unsigned
        // value above INT64_MAX converted as if it were negative - and INT64_MIN
        // turning up in results is exactly that signature.
        const bool is_unsigned = BIT(insn, 16) != 0;
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
    // The upper half of a 128-bit register has its own pair of encodings; the
    // lower half is the plain D form above. `seq` reaches these while laying out
    // its output buffer.
    if ((insn & 0xFFFFFC00u) == 0x9EAF0000u) {      // fmov Vd.D[1], Xn
        const unsigned rn2 = BITS(insn, 9, 5);
        cpu->v[rd][1] = e2_read_xzr(cpu, rn2);
        cpu->pc = next_pc; machine->instructions++; return E2_OK;
    }
    if ((insn & 0xFFFFFC00u) == 0x9EAE0000u) {      // fmov Xd, Vn.D[1]
        const unsigned rn2 = BITS(insn, 9, 5);
        write_reg(cpu, rd, cpu->v[rn2][1], true);
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
        // Read the whole source before writing anything: the destination is
        // frequently the same register as the source (`sshll v0.2d, v0.2s` is
        // the common form), and the first iteration would otherwise clobber the
        // value the second one still needs. That cost every widened high half,
        // which is how the regex engine lost half of each character class.
        const uint64_t src_lo = cpu->v[rn2][0];
        const uint64_t src_hi = cpu->v[rn2][1];
        // Q selects the upper half of the source; every destination lane is
        // twice as wide as its source lane.
        for (unsigned i = 0; i < lanes; i++) {
            const unsigned src_idx = (q ? lanes : 0u) + i;
            const unsigned src_bit = src_idx * esize;
            const uint64_t src_word = (src_bit >= 64) ? src_hi : src_lo;
            uint64_t v = (src_word >> (src_bit % 64)) & emask;
            if (!is_unsigned && (v & (1ULL << (esize - 1))) != 0) {
                v |= ~emask;   // sign-extend to 64 bits before shifting
            }
            const uint64_t wide = v << shift;   // v is already 64-bit signed here
            // The destination index is the lane number, not the source index: with Q set
            // the sources come from the upper half but the results always start at lane 0.
            // Using src_idx made the first lane land at half=2, past the end of the
            // register, so the widened values were never written and callers read whatever
            // had been there - which is how CPython's ASCII-to-wchar widening produced the
            // four bytes "/sur" in a single wchar_t.
            const int half = (i * dsize) / 64;
            const unsigned off = (i * dsize) % 64;
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

    // --- SIMD: TBL / TBX (table lookup on bytes) ---
    // `tbl v1.16b, {v0.16b}, v0.16b` gathers bytes through an index vector; TBX keeps the
    // destination byte where the index is out of range instead of zeroing it. Together with
    // UZP, ZIP, REV and EXT this is the byte-shuffling set, and every member of it that was
    // missing showed up as a corrupted string rather than as a failed instruction.
    if ((insn & 0xBFE08800u) == 0x0E000000u) {
        const bool q = BIT(insn, 30) != 0;
        const unsigned len = BITS(insn, 14, 13);        // one fewer than the register count
        const bool tbx = BIT(insn, 12) != 0;
        const unsigned rm = BITS(insn, 20, 16);
        const unsigned rn = BITS(insn, 9, 5);
        const unsigned rd = BITS(insn, 4, 0);
        const unsigned per = q ? 16u : 8u;
        const unsigned table_bytes = per * (len + 1);
        uint8_t table[64];
        for (unsigned r = 0; r <= len && r < 4; r++) {
            for (unsigned i = 0; i < per; i++) {
                table[r * per + i] = (uint8_t)(cpu->v[(rn + r) % 32][i / 8] >> ((i % 8) * 8));
            }
        }
        uint64_t lo = 0, hi = 0;
        for (unsigned i = 0; i < per; i++) {
            const unsigned idx = (unsigned)((cpu->v[rm][i / 8] >> ((i % 8) * 8)) & 0xFF);
            uint8_t out;
            if (idx < table_bytes) {
                out = table[idx];
            } else if (tbx) {
                out = (uint8_t)(cpu->v[rd][i / 8] >> ((i % 8) * 8));
            } else {
                out = 0;
            }
            if (i < 8) lo |= (uint64_t)out << (i * 8);
            else hi |= (uint64_t)out << ((i - 8) * 8);
        }
        cpu->v[rd][0] = lo;
        cpu->v[rd][1] = q ? hi : 0;
        cpu->pc = next_pc;
        machine->instructions++;
        return E2_OK;
    }

    // --- SIMD: EXT (extract from the concatenation of two vectors) ---
    // `ext v1.16b, v0.16b, v0.16b, #1` takes 16 bytes starting one byte into vn:vm. It is the
    // other half of the byte-shuffling family that path handling leans on, and like them a
    // wrong offset moves bytes without failing loudly.
    if ((insn & 0xBFE08400u) == 0x2E000000u) {
        const bool q = BIT(insn, 30) != 0;
        const unsigned bytes = q ? 16u : 8u;
        const unsigned imm4 = BITS(insn, 14, 11);
        const unsigned rm = BITS(insn, 20, 16);
        const unsigned rn = BITS(insn, 9, 5);
        const unsigned rd = BITS(insn, 4, 0);
        uint8_t joined[32];
        for (unsigned i = 0; i < bytes; i++) {
            joined[i] = (uint8_t)(cpu->v[rn][i / 8] >> ((i % 8) * 8));
            joined[bytes + i] = (uint8_t)(cpu->v[rm][i / 8] >> ((i % 8) * 8));
        }
        uint64_t lo = 0, hi = 0;
        for (unsigned i = 0; i < bytes; i++) {
            const uint8_t b = joined[(imm4 + i) % (bytes * 2)];
            if (i < 8) lo |= (uint64_t)b << (i * 8);
            else hi |= (uint64_t)b << ((i - 8) * 8);
        }
        cpu->v[rd][0] = lo;
        cpu->v[rd][1] = q ? hi : 0;
        cpu->pc = next_pc;
        machine->instructions++;
        return E2_OK;
    }

    // --- SIMD: REV64 / REV32 / REV16 (reverse bytes inside each element) ---
    // `rev64 v1.16b, v0.16b` reverses the bytes of each 64-bit half. Absent, it stopped the
    // interpreter outright; present-but-wrong it would scramble byte buffers silently, which
    // is the shape of the corrupted path strings CPython was printing.
    if ((insn & 0x9FFFF800u) == 0x0E200800u || (insn & 0x9FFFF800u) == 0x0E201800u) {
        const bool q = BIT(insn, 30) != 0;
        const bool is_rev32 = BIT(insn, 29) != 0;
        const bool is_rev16 = (insn & 0x00001000u) != 0;
        const unsigned size = BITS(insn, 23, 22);
        const unsigned rn = BITS(insn, 9, 5);
        const unsigned rd = BITS(insn, 4, 0);
        // REV64 reverses inside each 64-bit container, REV32 inside 32-bit, REV16 inside
        // 16-bit; the element size selects how wide one group of bytes is.
        unsigned group_bits;
        if (is_rev16) group_bits = 16;
        else if (is_rev32) group_bits = 32;
        else group_bits = 64;
        (void)size;
        const unsigned group_bytes = group_bits / 8;
        const unsigned halves = q ? 2u : 1u;
        for (unsigned h = 0; h < halves; h++) {
            const uint64_t src = cpu->v[rn][h];
            uint64_t out = 0;
            for (unsigned g = 0; g < 8 / group_bytes; g++) {
                const uint64_t chunk = (src >> (g * group_bits)) & (group_bits == 64 ? ~0ULL : ((1ULL << group_bits) - 1));
                uint64_t rev = 0;
                for (unsigned b = 0; b < group_bytes; b++) {
                    rev = (rev << 8) | ((chunk >> (b * 8)) & 0xFF);
                }
                out |= rev << (g * group_bits);
            }
            cpu->v[rd][h] = out;
        }
        cpu->pc = next_pc;
        machine->instructions++;
        return E2_OK;
    }

    // --- SIMD: LD1R / LD2R / LD3R / LD4R (load one element, replicate) ---
    // `ld1r {v1.2d}, [x0]` loads a single element and copies it into every lane. CPython
    // emits this while maintaining reference counts in bulk, and its absence stopped the
    // interpreter with "unimplemented instruction" the first time start-up got that far.
    // LD2R..LD4R differ only in how many registers they fill, so they share this decode.
    if ((insn & 0xBFFFF000u) == 0x0D40C000u) {
        const unsigned q = BIT(insn, 30);
        const unsigned size = BITS(insn, 11, 10);
        const unsigned rn = BITS(insn, 9, 5);
        const unsigned rt = BITS(insn, 4, 0);
        const unsigned esize = 8u << size;
        const unsigned bytes = esize / 8;
        const unsigned regs = 1u << BITS(insn, 14, 12);   // opcode selects LD1R..LD4R
        const uint64_t addr = e2_read_sp(cpu, rn);
        for (unsigned r = 0; r < regs; r++) {
            uint64_t v = 0;
            if (e2_memory_read(&machine->mem, addr + r * bytes, &v, bytes) != 0) {
                machine->ctx.reason = "ld1r outside mapped memory";
                machine->ctx.fault_addr = addr + r * bytes;
                return E2_FAULT;
            }
            uint64_t lane_mask = (esize >= 64) ? ~0ULL : ((1ULL << esize) - 1);
            v &= lane_mask;
            uint64_t fill = 0;
            for (unsigned bit = 0; bit < 64; bit += esize) {
                fill |= v << bit;
            }
            // The 64-bit form writes only the low half; the upper 64 bits are cleared, as for
            // every other 64-bit Advanced SIMD write. Leaving them alone instead made a reply
            // that used `ld1r {v0.8b}` read whatever was in the register before it.
            cpu->v[(rt + r) % 32][0] = fill;
            cpu->v[(rt + r) % 32][1] = q ? fill : 0;
        }
        cpu->pc = next_pc;
        machine->instructions++;
        return E2_OK;
    }

    // --- SIMD: LD1 / ST1, one element into one lane ---
    // `ld1 {v0.d}[1], [x0]` loads a single element into one lane of a register.
    // Index comes from Q and S; the element width is size=00 (32-bit) or
    // 01 (64-bit) in the forms the compiler emits here.
    if ((insn & 0xBF9FF400u) == 0x0D008400u || (insn & 0xBF9FF400u) == 0x0D008000u) {
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
            // The 64-bit form: each bit of imm8 fills one byte of the register with 0xff,
            // so imm8=0x01 gives 0x00000000000000ff and imm8=0x0f gives 0x00000000ffffffff.
            // Checked against all 255 encodings with llvm. The previous version replicated
            // the whole byte, which made `movi d0, #0x000000ffffffff` come out as
            // 0x0f0f0f0f0f0f0f0f - the register PyCode_Addr2Line stores over its
            // address-range struct, and where CPython's start-up went wrong.
            uint64_t pattern = 0;
            for (unsigned i = 0; i < 8; i++) {
                if ((imm8 >> i) & 1u) {
                    pattern |= 0xFFULL << (8 * i);
                }
            }
            cpu->v[rd_v][0] = pattern;
            cpu->v[rd_v][1] = pattern;
        } else if (cmode == 0x8 || cmode == 0xA) {
            const unsigned shift = (cmode == 0x8) ? 0u : 8u;
            const unsigned lanes = q ? 8u : 4u;
            uint64_t val = (uint64_t)imm8 << shift;
            // op set turns MOVI into MVNI: the value is complemented, not just
            // shifted. Ignoring it here made `mvni v0.2s, #0` produce zero
            // instead of all ones, and the regex engine relies on exactly that
            // to mark an unset field - which is why grep and awk failed.
            if (op) val = ~val & 0xFFFFu;
            for (unsigned i = 0; i < lanes; i++) {
                e2_v_set(cpu, rd_v, 16, i, val);
            }
        } else if (cmode == 0x0 || cmode == 0x2 || cmode == 0x4 || cmode == 0x6) {
            const unsigned shift = (cmode >> 1) * 8u;
            const unsigned lanes = q ? 4u : 2u;
            uint64_t val = (uint64_t)imm8 << shift;
            if (op) val = ~val & 0xFFFFFFFFu;
            for (unsigned i = 0; i < lanes; i++) {
                e2_v_set(cpu, rd_v, 32, i, val);
            }
        } else if (cmode == 0xC || cmode == 0xD) {
            // movi Vd.4s, #imm8, msl #8/#16 - the low bits become ones.
            const unsigned shift = (cmode == 0xC) ? 8u : 16u;
            const uint64_t ones = (1ULL << shift) - 1;
            const unsigned lanes = q ? 4u : 2u;
            uint64_t val = (((uint64_t)imm8 << shift) | ones) & 0xFFFFFFFFu;
            if (op) val = ~val & 0xFFFFFFFFu;
            for (unsigned i = 0; i < lanes; i++) {
                e2_v_set(cpu, rd_v, 32, i, val);
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
    // --- SIMD&FP load/store (register offset) ---
    // Same family as the unsigned-offset form below, but the address is
    // Rn + (Rm << scale). Compilers use it for vectorised copies, and without
    // it those instructions fell into the integer path: `ldr q0, [x21, x2]`
    // was being executed as an integer register-offset load.
    // Bit 21 separates this form from the unscaled one (`stur d3, [sp, #228]`),
    // which has an imm9 there instead of an Rm.
    if ((insn & 0x3F200000u) == 0x3C200000u) {
        const unsigned size = BITS(insn, 31, 30);
        const bool is_load = BIT(insn, 22) != 0;
        const unsigned rm = BITS(insn, 20, 16);
        const bool scale = BIT(insn, 12) != 0;
        const unsigned rn2 = BITS(insn, 9, 5);
        const unsigned rt2 = BITS(insn, 4, 0);
        // Encodings, from an assembler:
        //   0x3C.. -> ldr q0  (16)    0xFC.. -> ldr d0  (8)
        //   0xBC.. -> ldr s0  (4)     0x7C.. -> ldr h0  (2)
        // so size 0 is the 16-byte vector form and the rest are 1 << size.
        const unsigned bytes = (size == 0) ? 16u : (1u << size);
        uint64_t index = e2_read_xzr(cpu, rm);
        if (scale) {
            index <<= size;
        }
        const uint64_t addr = e2_read_sp(cpu, rn2) + index;
        uint64_t lo = 0, hi = 0;
        if (is_load) {
            if (e2_memory_read(&machine->mem, addr, &lo, bytes > 8 ? 8 : bytes) != 0) {
                machine->ctx.reason = "SIMD load outside mapped memory";
                machine->ctx.fault_addr = addr;
                return E2_FAULT;
            }
            if (bytes > 8 &&
                e2_memory_read(&machine->mem, addr + 8, &hi, bytes - 8) != 0) {
                machine->ctx.reason = "SIMD load outside mapped memory";
                machine->ctx.fault_addr = addr;
                return E2_FAULT;
            }
            if (bytes >= 16) {
                cpu->v[rt2][0] = lo;
                cpu->v[rt2][1] = hi;
            } else if (bytes == 8) {
                cpu->v[rt2][0] = lo;
            } else {
                const uint64_t mask = (1ULL << (bytes * 8)) - 1;
                cpu->v[rt2][0] = (cpu->v[rt2][0] & ~mask) | (lo & mask);
            }
        } else {
            if (bytes >= 16) {
                lo = cpu->v[rt2][0];
                hi = cpu->v[rt2][1];
                if (e2_memory_write(&machine->mem, addr, &lo, 8) != 0 ||
                    e2_memory_write(&machine->mem, addr + 8, &hi, 8) != 0) {
                    machine->ctx.reason = "SIMD store outside mapped memory";
                    machine->ctx.fault_addr = addr;
                    return E2_FAULT;
                }
            } else {
                lo = cpu->v[rt2][0];
                if (e2_memory_write(&machine->mem, addr, &lo, bytes) != 0) {
                    machine->ctx.reason = "SIMD store outside mapped memory";
                    machine->ctx.fault_addr = addr;
                    return E2_FAULT;
                }
            }
        }
        cpu->pc = next_pc;
        machine->instructions++;
        return E2_OK;
    }

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

        // Each register occupies `scale` bytes, so a 128-bit pair spans four
        // 8-byte halves but a 64-bit pair spans only two. Writing four halves
        // unconditionally made `stp d8, d9, [sp, #208]` also write 16 bytes past
        // the pair - which landed on the caller's stack slot and zeroed it.
        const unsigned parts = (scale > 8) ? 2u : 1u;
        const size_t half = (scale > 8) ? 8u : (size_t)scale;
        for (unsigned k = 0; k < parts; k++) {
            const uint64_t off = (uint64_t)k * 8u;
            if (is_load) {
                if (e2_memory_read(&machine->mem, addr + off, &cpu->v[rt_v][k], half) != 0 ||
                    e2_memory_read(&machine->mem, addr + scale + off, &cpu->v[rt2_v][k], half) != 0) {
                    machine->ctx.reason = "simd ldp outside mapped memory";
                    return E2_FAULT;
                }
            } else {
                if (e2_memory_write(&machine->mem, addr + off, &cpu->v[rt_v][k], half) != 0 ||
                    e2_memory_write(&machine->mem, addr + scale + off, &cpu->v[rt2_v][k], half) != 0) {
                    machine->ctx.reason = "simd stp outside mapped memory";
                    return E2_FAULT;
                }
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

    // --- Add/subtract with carry ---
    // A separate encoding group from the ordinary shifted forms: bits 28-24 are
    // 11010 and the opcode field is zero. Compilers emit these for multi-word
    // arithmetic, and `seq` reaches them after the vector paths are in place.
    if ((insn & 0x1FE0FC00u) == 0x1A000000u) {
        const bool is64 = BIT(insn, 31) != 0;
        const bool is_sub = BIT(insn, 30) != 0;
        const bool set_flags = BIT(insn, 29) != 0;
        const unsigned rm = BITS(insn, 20, 16);
        const unsigned rn2 = BITS(insn, 9, 5);
        const uint64_t mask = is64 ? ~0ULL : 0xFFFFFFFFULL;
        const uint64_t a = e2_read_xzr(cpu, rn2) & mask;
        const uint64_t b = e2_read_xzr(cpu, rm) & mask;
        const uint64_t carry = cpu->flags.c ? 1u : 0u;
        uint64_t result;
        if (is_sub) {
            const uint64_t borrow = carry ^ 1u;
            result = (a - b - borrow) & mask;
        } else {
            result = (a + b + carry) & mask;
        }
        write_reg(cpu, rd, result, is64);
        if (set_flags) {
            // The flag rules need the true operands including the carry, so the
            // addition is recomputed against b+carry and the subtraction
            // against a borrow, rather than reusing a and b unchanged.
            const uint64_t b_eff = is_sub ? ((b + (carry ^ 1u)) & mask)
                                          : ((b + carry) & mask);
            set_flags_addsub(cpu, a, b_eff, result, is_sub, is64 ? 64 : 32);
        }
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
                machine->ctx.fault_addr = addr;
                return E2_FAULT;
            }
            if (opc == 3 && size_code == 3) {   // PRFM: no destination register
                cpu->pc = next_pc;
                machine->instructions++;
                return E2_OK;
            }
            value = apply_load_opc(value, opc, bytes, size_code == 3);
            // The destination width follows the instruction, not the access
            // size: LDRSB/LDRSH/LDRSW always write a 64-bit register, while
            // their 32-bit forms and LDRB/LDRH/LDR(W) write 32.
            write_reg(cpu, rd, value, (opc == 2) || (size_code == 3));
        } else {
            // Storing XZR is legal and common (zero-init loops); reading x[31]
            // would take a byte from the neighbouring stack slot instead.
            uint64_t value = e2_read_xzr(cpu, rd);
            if (e2_memory_write(&machine->mem, addr, &value, bytes) != 0) {
                machine->ctx.reason = "store outside mapped memory";
                machine->ctx.fault_addr = addr;
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
                machine->ctx.fault_addr = addr;
                return E2_FAULT;
            }
            if (opc == 3 && size_code == 3) {   // PRFM: no destination register
                cpu->pc = next_pc;
                machine->instructions++;
                return E2_OK;
            }
            value = apply_load_opc(value, opc, bytes, size_code == 3);
            // The destination width follows the instruction, not the access
            // size: LDRSB/LDRSH/LDRSW always write a 64-bit register, while
            // their 32-bit forms and LDRB/LDRH/LDR(W) write 32.
            write_reg(cpu, rd, value, (opc == 2) || (size_code == 3));
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
        // Same width rule as the register-offset form: size 0 is the 16-byte
        // vector encoding, not one byte. `stur q0, [x19, #24]` was writing a
        // single zero byte over two pointer-sized fields.
        const unsigned bytes = (size_code == 0) ? 16u : (1u << size_code);
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
    // Bit 26 selects between the general and the Advanced SIMD form. Masking it
    // out made `ldr q0, [x21, x2]` look like an integer register-offset load,
    // which wrote the vector's first lane into a general register instead.
    if ((insn & 0x3F200C00u) == 0x38200800u) {
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
                machine->ctx.fault_addr = addr;
                machine->ctx.svc_number = (uint16_t)(addr & 0xFFFF);
                return E2_FAULT;
            }
            // LDRSB / LDRSH / LDRSW are selected by opc, so the sign extension
            // has to go through the same helper as the other load forms. This
            // path used to store the raw bytes, which turned a 0xFF byte into
            // 0xFF instead of -1 - the wrong index every time one was used.
            if (opc == 3 && size_code == 3) {   // PRFM: prefetch, no register write
                cpu->pc = next_pc;
                machine->instructions++;
                return E2_OK;
            }
            value = apply_load_opc(value, opc, bytes, size_code == 3);
            const bool dest64 = (opc == 2) || (size_code == 3);
            write_reg(cpu, rd, value, dest64);
        } else {
            uint64_t value = e2_read_xzr(cpu, rd);
            if (e2_memory_write(&machine->mem, addr, &value, bytes) != 0) {
                machine->ctx.reason = "store outside mapped memory";
                machine->ctx.fault_addr = addr;
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
            case 0xDA20:  // FPCR: floating-point control register
                if (is_read) {
                    write_reg(cpu, rt, cpu->fpcr, true);
                } else {
                    cpu->fpcr = e2_read_xzr(cpu, rt);
                }
                break;
            case 0xDA21:  // FPSR: floating-point status register
                if (is_read) {
                    write_reg(cpu, rt, cpu->fpsr, true);
                } else {
                    cpu->fpsr = e2_read_xzr(cpu, rt);
                }
                break;
            case 0xD809:  // CTR_EL0: cache type register, constant
                if (is_read) {
                    write_reg(cpu, rt, 0x8444C004ULL, true);
                }
                break;
            case 0xD807:  // DCZID_EL0: data cache zero id
                // DZP is bit 4: set means "DC ZVA is prohibited". This core does
                // not implement DC ZVA, so it must say so. Returning 4 (DZP
                // clear) claimed the instruction worked, and memset then took
                // the `dc zva` fast path and wrote nothing at all - which is
                // what made the regex engine operate on uninitialised memory.
                if (is_read) {
                    write_reg(cpu, rt, 0x10ULL, true);
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

    // --- Hints: NOP, BTI, PAC/AUT and the rest of the space ---
    // `1101 0101 0000 0011 0010 CRm(4) op2(3) 11111`. Only NOP was recognised, so a
    // binary built with -mbranch-protection faulted on its first BTI (0xD503245F) -
    // apk did exactly that. Every member of this space is architecturally a hint and
    // may be treated as a no-op; BTI in particular only marks a valid jump target,
    // and nothing here enforces branch targets.
    if ((insn & 0xFFFFF01Fu) == 0xD503201Fu) {
        cpu->pc = next_pc;
        machine->instructions++;
        return E2_OK;
    }

    machine->ctx.reason = "unimplemented instruction";
    return E2_UNIMPL;
}

e2_status e2_run(e2_machine *machine, uint64_t max_instructions) {
    // Deliberately unchanged: delivery is driven by the caller, which knows whether a
    // pending signal belongs to the process about to run.
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
