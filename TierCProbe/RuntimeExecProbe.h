#ifndef RUNTIME_EXEC_PROBE_H
#define RUNTIME_EXEC_PROBE_H

#include <stddef.h>

/// Result of probing whether this process can execute code that was loaded at
/// runtime. Used to decide the architecture of a future Linux backend: if the
/// process can run aarch64 machine code it loaded itself, a native userland
/// (syscall forwarding only) is viable; if not, the backend needs its own
/// instruction interpreter.
///
/// This probe is original code and does not depend on iSH or any GPL component,
/// so it can ship in every build.
typedef struct {
    /// 0 when an anonymous executable mapping succeeded, otherwise errno.
    int anonymous_exec_errno;

    /// 0 when mapping the code file read-only succeeded, otherwise errno.
    int file_map_errno;

    /// 0 when mprotect(PROT_READ|PROT_EXEC) on the file mapping succeeded,
    /// otherwise errno.
    int file_protect_errno;

    /// -1 when execution was not attempted (mapping or mprotect failed).
    /// 0 when the loaded code ran and returned.
    /// Otherwise the signal number that fired.
    int execute_status;

    /// Return value from the loaded code when execute_status == 0.
    int execute_value;

    /// Human-readable summary; always NUL-terminated.
    char detail[512];
} pocket_exec_probe_result;

/// Runs the probe. Returns 0 when the probe completed (regardless of outcome),
/// negative errno when the probe itself could not run.
int pocket_exec_probe(pocket_exec_probe_result *out);

#endif
