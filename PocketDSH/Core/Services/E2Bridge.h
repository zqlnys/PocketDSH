#ifndef E2_BRIDGE_H
#define E2_BRIDGE_H

/// Runs the E2 harness, capturing stdout into out_path. Returns the exit status, or -1 when
/// the redirection could not be set up.
int e2_bridge_run(const char *rootfs, const char *out_path, int argc, char **argv);

#endif
