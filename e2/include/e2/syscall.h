#ifndef E2_SYSCALL_H
#define E2_SYSCALL_H

#include <stddef.h>
#include <stdint.h>

#include "e2/exec.h"

/// Linux AArch64 syscall numbers used so far. The ABI is asm-generic, which is
/// the numbering every aarch64 Linux userspace binary is built against.
#define E2_SYS_write 64
#define E2_SYS_exit 93
#define E2_SYS_exit_group 94

/// Everything the syscall layer needs from its host. The iOS layer will provide
/// file descriptors backed by the app sandbox; the host test harness provides
/// stdio and a memory buffer.
typedef struct {
    /// Writes `len` bytes from `buf` to guest fd `fd`. Returns bytes written, or
    /// a negative errno-style code.
    long (*write)(void *ctx, int fd, const void *buf, size_t len);
    /// Called for exit/exit_group. The interpreter stops afterwards.
    void (*exit)(void *ctx, int status);
    void *ctx;
} e2_syscall_env;

/// Services the syscall the machine stopped on.
///
/// Returns E2_OK to continue executing, or E2_HALTED when the guest exited.
/// The return value of the syscall is placed in X0, as the ABI requires.
e2_status e2_service_syscall(e2_machine *machine, e2_syscall_env *env);

#endif
