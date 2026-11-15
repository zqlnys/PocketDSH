#ifndef ISH_BRIDGE_H
#define ISH_BRIDGE_H

#include <stddef.h>

/// Starts one persistent Alpine /bin/sh inside iSH.
/// Returns 0 on success, negative errno on failure.
int pocket_ish_start(const char *rootfs);

/// Sends a command line to the persistent shell.
int pocket_ish_send(const char *command);

/// Reads shell output. Returns bytes read, 0 on timeout, negative on error.
int pocket_ish_read(char *buffer, size_t capacity, int timeout_ms);

/// Returns 1 while the shell task is alive.
int pocket_ish_is_running(void);

/// Closes the shell and its pipes.
void pocket_ish_close(void);

#endif
