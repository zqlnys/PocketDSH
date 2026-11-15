#ifndef ISH_BRIDGE_H
#define ISH_BRIDGE_H

#include <stddef.h>

/// Runs one Linux command through iSH. The output is written to output_path.
/// Returns the emulated shell exit code, or a negative error code.
int pocket_ish_run(const char *rootfs, const char *command, const char *output_path);

#endif
