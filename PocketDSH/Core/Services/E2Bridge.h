#ifndef E2_BRIDGE_H
#define E2_BRIDGE_H

/// Runs the E2 harness, capturing stdout into out_path. Returns the exit status, or -1 when
/// the redirection could not be set up.
int e2_bridge_run(const char *rootfs, const char *out_path, int argc, char **argv);

/// The same, with the guest's /workspace mounted at `workspace`.
///
/// Without it the guest's /workspace is a scratch directory and a command cannot see the
/// project it was asked about - which is most of the point of running one.
int e2_bridge_run_in(const char *rootfs, const char *workspace, const char *out_path,
                     int argc, char **argv);

#endif
