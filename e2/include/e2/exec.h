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
    /// Guest address involved in a fault, when the fault has one. Printing it
    /// with the reason saves guessing which load went out of range.
    uint64_t fault_addr;
    /// Register involved in a fault, or 0xFF when not applicable.
    unsigned fault_reg;
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
    /// Program break, backed by a real mapping. libc's allocator grows it.
    uint64_t brk_base;
    uint64_t brk_current;
    uint64_t brk_limit;
    /// Signal handlers installed through rt_sigaction. 0 means "no handler" and 1 means
    /// "ignore", matching the kernel's SIG_DFL and SIG_IGN. Until this existed both
    /// rt_sigaction and kill returned 0 without doing anything, so `trap ... ; kill -INT $$`
    /// silently did nothing - the interface answered and the implementation was a constant.
    uint64_t sig_handler[64];
    /// Signals raised but not yet delivered, as a bitmask over signal numbers.
    uint64_t sig_pending;
    /// Where the trampoline lives: two instructions, `mov x8, #139; svc #0`, which is what
    /// the kernel puts in the return address of a signal frame so the handler's `ret` lands
    /// on rt_sigreturn.
    uint64_t sig_trampoline;
} e2_machine;

/// Prepares a machine with a stack of `stack_size` bytes. The stack top is
/// page aligned and stored in SP.
void e2_machine_init(e2_machine *machine, uint64_t entry, size_t stack_size);
void e2_machine_destroy(e2_machine *machine);

/// The context saved on the guest stack when a signal is delivered, restored by
/// rt_sigreturn. The kernel's own frame carries siginfo and a ucontext; only the registers
/// and the interrupted pc are needed to resume correctly, and busybox's trap handlers read
/// neither of the others.
typedef struct {
    uint64_t x[31];   ///< x0..x30 as they were when the signal was taken.
    uint64_t sp;
    uint64_t pc;
    uint64_t magic;   ///< Guards against an rt_sigreturn with no frame on the stack.
} e2_sigframe;

#define E2_SIGFRAME_MAGIC 0x4532736967467261ULL

/// Raises `signum` for the next instruction boundary, unless it is ignored.
///
/// Returns 0 when the signal was recorded or dropped, negative errno otherwise. Delivery
/// itself happens in e2_deliver_pending_signal.
int e2_machine_raise_signal(e2_machine *machine, unsigned signum);

/// If a signal is pending and has a handler, builds its frame and points the machine at the
/// handler. Returns 1 when it delivered one, 0 when nothing was pending, and -1 when a
/// pending signal has no handler (the default action, which this layer reports rather than
/// simulates).
int e2_deliver_pending_signal(e2_machine *machine);

/// Restores the context saved by e2_deliver_pending_signal. Used by rt_sigreturn.
int e2_machine_pop_sigframe(e2_machine *machine, e2_sigframe *out);

/// Executes exactly one instruction.
///
/// On E2_SYSCALL the SVC number is in `machine->ctx.svc_number` and PC has
/// already advanced past the SVC; the caller services it and resumes execution.
e2_status e2_step(e2_machine *machine);

/// Duplicates a machine's architectural state and address space. Used by fork.
/// Returns 0 on success. The destination must not already own memory.
int e2_machine_clone(const e2_machine *src, e2_machine *dst);

/// Runs until halt, fault, unimplemented instruction, or the instruction budget
/// is exhausted. Returns the status that stopped it.
e2_status e2_run(e2_machine *machine, uint64_t max_instructions);

/// Human-readable name for a status, used by tests and logs.
const char *e2_status_name(e2_status status);

#endif
