// Bridge between Swift and the E2 harness.
//
// The harness writes the guest's output to stdout, which an app cannot hand back to Swift
// directly. Rather than redirecting into a pipe - where output larger than the pipe buffer
// deadlocks, because nothing is draining it while the call runs - this sends stdout to a
// temporary file and returns the path. The caller reads it and deletes it.
//
// Everything here is ordinary file and descriptor work, which iOS allows inside the sandbox.

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void e2_set_root(const char *path);
int e2_rune_main(int argc, char **argv);

/// Runs the harness with `argv` and captures stdout into `out_path`.
///
/// Returns the harness's exit status, or -1 if the redirection itself failed. The caller owns
/// `out_path` and should delete the file.
int e2_bridge_run(const char *rootfs, const char *out_path,
                  int argc, char **argv) {
    if (rootfs != NULL) {
        e2_set_root(rootfs);
    }

    fflush(stdout);
    const int saved = dup(STDOUT_FILENO);
    if (saved < 0) {
        return -1;
    }
    const int sink = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (sink < 0) {
        close(saved);
        return -1;
    }
    if (dup2(sink, STDOUT_FILENO) < 0) {
        close(sink);
        close(saved);
        return -1;
    }
    close(sink);

    const int status = e2_rune_main(argc, argv);

    fflush(stdout);
    dup2(saved, STDOUT_FILENO);
    close(saved);
    return status;
}
