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
        uint64_t stack_base = 0;
        if (e2_memory_map_any(&machine->mem, stack_size, E2_PROT_READ | E2_PROT_WRITE,
                              &stack_base) == 0) {
            // SP points at the top (grows down).
            machine->cpu.sp = stack_base + ((stack_size + E2_PAGE_SIZE - 1) & ~(size_t)(E2_PAGE_SIZE - 1));
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
            uint64_t current = is64 ? cpu->x[rd] : (cpu->x[rd] & 0xFFFFFFFFu);
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
        e2_write_sp(cpu, rd, result);
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

        uint64_t operand = cpu->x[rm] & mask;
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
        uint64_t a = cpu->x[rn] & mask;
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
        const uint64_t value = is64 ? cpu->x[rd] : (cpu->x[rd] & 0xFFFFFFFFu);
        const bool take = nonzero ? (value != 0) : (value == 0);
        if (take) {
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
            uint64_t value = cpu->x[rd];
            if (e2_memory_write(&machine->mem, addr, &value, bytes) != 0) {
                machine->ctx.reason = "store outside mapped memory";
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
