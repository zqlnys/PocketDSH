#ifndef E2_CPU_H
#define E2_CPU_H

#include <stdbool.h>
#include <stdint.h>

/// AArch64 general purpose register indices.
///
/// X31 is not a real register: it reads as zero and writes are discarded in most
/// encodings, but it also means SP in a few of them. The execution layer handles
/// that distinction per instruction.
#define E2_REG_SP 31
#define E2_REG_XZR 31
#define E2_REG_LR 30
#define E2_REG_FP 29

/// NZCV condition flags.
typedef struct {
    bool n;  ///< negative
    bool z;  ///< zero
    bool c;  ///< carry
    bool v;  ///< overflow
} e2_flags;

/// Architectural state of the emulated CPU.
///
/// Deliberately plain data with no pointers into host memory: this struct is what
/// the iOS layer will own, and keeping it copyable makes fork() later a memcpy.
typedef struct {
    uint64_t x[31];      ///< X0-X30
    uint64_t sp;         ///< stack pointer (separate from x31)
    uint64_t pc;         ///< program counter
    uint64_t tpidr_el0;  ///< thread pointer, used for TLS
    /// Floating-point control and status. Nothing here computes with them yet,
    /// but libc's fenv routines read-modify-write FPCR and the interpreter must
    /// keep whatever it is given: rounding-mode plumbing is not ours to fake.
    uint64_t fpcr;
    uint64_t fpsr;
    uint64_t v[32][2];   ///< SIMD/FP registers, 128 bits each as two halves
    e2_flags flags;
} e2_cpu;

// SIMD helpers. Vector registers hold 128 bits; element accessors keep the
// instruction implementations readable.

/// Reads lane `index` of `width` bits (8/16/32/64) from a vector register.
static inline uint64_t e2_v_get(const e2_cpu *cpu, unsigned reg, unsigned width, unsigned index) {
    const unsigned per_half = 64 / width;
    const unsigned half = index / per_half;
    const unsigned slot = index % per_half;
    const uint64_t mask = (width == 64) ? ~0ULL : ((1ULL << width) - 1);
    return (cpu->v[reg][half] >> (slot * width)) & mask;
}

/// Writes lane `index` of `width` bits into a vector register.
static inline void e2_v_set(e2_cpu *cpu, unsigned reg, unsigned width, unsigned index,
                            uint64_t value) {
    const unsigned per_half = 64 / width;
    const unsigned half = index / per_half;
    const unsigned slot = index % per_half;
    const uint64_t mask = (width == 64) ? ~0ULL : ((1ULL << width) - 1);
    const unsigned shift = slot * width;
    cpu->v[reg][half] = (cpu->v[reg][half] & ~(mask << shift)) | ((value & mask) << shift);
}

/// Result of one step of execution.
typedef enum {
    E2_OK = 0,        ///< instruction retired, keep going
    E2_HALTED,        ///< guest asked to exit
    E2_FAULT,         ///< unrecoverable fault, see e2_cpu_fault_reason
    E2_UNIMPL,        ///< instruction not implemented yet
    E2_SYSCALL,       ///< guest issued SVC and wants the host to service it
} e2_status;

void e2_cpu_reset(e2_cpu *cpu, uint64_t entry);

/// Reads Xn, treating register 31 as XZR (zero).
static inline uint64_t e2_read_xzr(const e2_cpu *cpu, unsigned reg) {
    return reg == E2_REG_XZR ? 0 : cpu->x[reg];
}

/// Writes Xn, discarding writes to register 31.
static inline void e2_write_xzr(e2_cpu *cpu, unsigned reg, uint64_t value) {
    if (reg != E2_REG_XZR) {
        cpu->x[reg] = value;
    }
}

/// Reads Xn, treating register 31 as SP.
static inline uint64_t e2_read_sp(const e2_cpu *cpu, unsigned reg) {
    return reg == E2_REG_SP ? cpu->sp : cpu->x[reg];
}

/// Writes Xn, treating register 31 as SP.
static inline void e2_write_sp(e2_cpu *cpu, unsigned reg, uint64_t value) {
    if (reg == E2_REG_SP) {
        cpu->sp = value;
    } else {
        cpu->x[reg] = value;
    }
}

#endif
