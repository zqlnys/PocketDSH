#ifndef ISH_BRIDGE_H
#define ISH_BRIDGE_H

#include <stddef.h>

/// Runs one Linux command through iSH. Output is written to output_path.
///
/// - `rootfs`: host path to the Alpine rootfs directory.
/// - `workspace`: optional host path mounted at /workspace inside the guest via
///   iSH's realfs backend. Pass NULL to skip the mount.
/// - `command`: shell command executed by /bin/sh -c.
///
/// Returns the emulated shell exit code, or a negative error code.
int pocket_ish_run(const char *rootfs, const char *workspace, const char *command,
                   const char *output_path);

#endif
