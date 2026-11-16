#ifndef ISH_PERSISTENT_SHELL_H
#define ISH_PERSISTENT_SHELL_H

#include <stddef.h>

/// A long-lived emulated shell.
///
/// Instead of running one `xX_main_Xx` + `task_run_current` per command (which
/// forces a fixed capture window and loses the tail of the output), this keeps a
/// single `/bin/sh` alive and pipes commands in/out through host file
/// descriptors. Command boundaries are marked by a sentinel line echoed by the
/// shell itself.
///
/// Only one shell may be active at a time; iSH keeps global mount and pid state.
typedef struct pocket_shell pocket_shell;

/// Returned by pocket_shell_exec when the timeout expired before the sentinel.
/// A timed-out command leaves the shell in an unknown state, so callers should
/// discard the session rather than reuse it.
#define POCKET_SHELL_TIMEOUT (-1000L)

/// Starts a shell. `workspace` may be NULL to skip the /workspace mount.
/// Returns NULL on failure and writes a reason into `error` when provided.
///
/// `workspace_read_only` mounts the workspace through a wrapper that rejects
/// every write (iSH's realfs stores mount flags but never enforces them, so the
/// flag alone would be cosmetic). Pass 0 for the read-write behaviour.
pocket_shell *pocket_shell_start(const char *rootfs, const char *workspace,
                                 int workspace_read_only,
                                 char *error, size_t error_size);

/// Sends one command and collects output until the sentinel arrives or the
/// timeout expires. Returns the number of bytes stored in `out` (always
/// NUL-terminated), or a negative errno-style code.
///
/// `truncated` is set to 1 when the timeout hit before the sentinel.
long pocket_shell_exec(pocket_shell *shell, const char *command,
                       char *out, size_t out_size, int timeout_ms,
                       int *truncated);

/// True while the emulated shell process is still alive.
int pocket_shell_is_running(pocket_shell *shell);

/// Sends `exit`, waits briefly, then releases everything.
void pocket_shell_stop(pocket_shell *shell);

#endif
