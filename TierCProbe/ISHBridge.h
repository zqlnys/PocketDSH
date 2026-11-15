#ifndef ISH_BRIDGE_H
#define ISH_BRIDGE_H

#include <stddef.h>

/// Runs one Linux command through iSH. The output is written to output_path.
/// Returns the emulated shell exit code, or a negative error code.
int pocket_ish_run(const char *rootfs, const char *command, const char *output_path);

/// Registers an already-copied project tree under /workspace in the iSH fakefs
/// metadata database. Returns 0 on success, negative errno on failure.
int pocket_ish_register_workspace(const char *rootfs, const char *workspace_path);

#endif
