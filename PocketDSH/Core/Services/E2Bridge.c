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

/// Environment the guest needs, set once, without overwriting anything already present.
///
/// They live here rather than in Swift because they are properties of the interpreter's
/// configuration, not of a particular call:
///
///   E2_HEAP_MB   CPython's allocator needs far more than busybox's 16 MB default. 256 MB is
///                what a trimmed Alpine root needs for `import json` and `re`.
///   E2_CAP       importing the standard library runs tens of millions of instructions.
///   PYTHONHOME   without it getpath walks its landmarks and fails before any Python runs.
static void e2_configure_environment(void) {
    static int done = 0;
    if (done) {
        return;
    }
    done = 1;
    if (getenv("E2_HEAP_MB") == NULL) {
        setenv("E2_HEAP_MB", "256", 0);
    }
    if (getenv("E2_CAP") == NULL) {
        setenv("E2_CAP", "400000000", 0);
    }
    if (getenv("PYTHONHOME") == NULL) {
        setenv("PYTHONHOME", "/usr", 0);
    }
}

/// Runs the harness with `argv` and captures stdout into `out_path`.
///
/// Returns the harness's exit status, or -1 if the redirection itself failed. The caller owns
/// `out_path` and should delete the file.
int e2_bridge_run(const char *rootfs, const char *out_path,
                  int argc, char **argv) {
    if (rootfs != NULL) {
        e2_set_root(rootfs);
    }
    e2_configure_environment();

    // Both streams go to the file. The harness reports load and syscall failures on stderr, and
    // an exit status with no output is undiagnosable - which is exactly what the first run on
    // iOS produced: status 1, empty file, no clue.
    fflush(stdout);
    fflush(stderr);
    const int saved_out = dup(STDOUT_FILENO);
    const int saved_err = dup(STDERR_FILENO);
    if (saved_out < 0 || saved_err < 0) {
        if (saved_out >= 0) close(saved_out);
        if (saved_err >= 0) close(saved_err);
        return -1;
    }
    const int sink = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (sink < 0) {
        close(saved_out);
        close(saved_err);
        return -1;
    }
    if (dup2(sink, STDOUT_FILENO) < 0 || dup2(sink, STDERR_FILENO) < 0) {
        close(sink);
        close(saved_out);
        close(saved_err);
        return -1;
    }
    close(sink);

    const int status = e2_rune_main(argc, argv);

    fflush(stdout);
    fflush(stderr);
    dup2(saved_out, STDOUT_FILENO);
    dup2(saved_err, STDERR_FILENO);
    close(saved_out);
    close(saved_err);
    return status;
}
