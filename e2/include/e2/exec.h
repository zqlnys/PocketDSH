#ifndef E2_EXEC_H
#define E2_EXEC_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "e2/cpu.h"
#include "e2/memory.h"

/// Why the last step stopped, for tests and crash reports.
typedef struct {
    uint64_t pc;
    uint32_t instruction;
    const char *reason;
    uint16_t svc_number;
} e2_exec_context;

typedef struct {
    e2_cpu cpu;
    e2_memory mem;
    e2_exec_context ctx;
    uint64_t instructions;
    int exit_status;
    /// Monotonic counter handed to CNTVCT_EL0 readers; advances per read.
    uint64_t cycle_clock;
} e2_machine;

/// Prepares a machine with a stack of `stack_size` bytes. The stack top is
/// page aligned and stored in SP.
void e2_machine_init(e2_machine *machine, uint64_t entry, size_t stack_size);
void e2_machine_destroy(e2_machine *machine);

/// Executes exactly one instruction.
///
/// On E2_SYSCALL the SVC number is in `machine->ctx.svc_number` and PC has
/// already advanced past the SVC; the caller services it and resumes execution.
e2_status e2_step(e2_machine *machine);

/// Runs until halt, fault, unimplemented instruction, or the instruction budget
/// is exhausted. Returns the status that stopped it.
e2_status e2_run(e2_machine *machine, uint64_t max_instructions);

/// Human-readable name for a status, used by tests and logs.
const char *e2_status_name(e2_status status);

#endif
