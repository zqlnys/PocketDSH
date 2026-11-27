// Bridge between Swift and the E2 harness.
//
// The harness writes the guest's output to stdout, which an app cannot hand back to Swift
// directly. Rather than redirecting into a pipe - where output larger than the pipe buffer
// deadlocks, because nothing is draining it while the call runs - this sends stdout to a
// temporary file and returns the path. The caller reads it and deletes it.
//
// Everything here is ordinary file and descriptor work, which iOS allows inside the sandbox.

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "E2Bridge.h"

int e2_bridge_session_alive(void);
void e2_bridge_session_close(void);

// Declared by the harness under E2_AS_LIBRARY.
void e2_request_cancel(void);
void e2_clear_cancel(void);
#include <sys/stat.h>
#include <sys/resource.h>
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
    // A cap, not a reservation: the guest heap grows in chunks on demand, so a large ceiling
    // costs nothing until something asks for it. It was 256 and CPython importing re through
    // subprocess reported MemoryError on the device at that figure.
    if (getenv("E2_HEAP_MB") == NULL) {
        setenv("E2_HEAP_MB", "1024", 0);
    }
    if (getenv("E2_CAP") == NULL) {
        setenv("E2_CAP", "400000000", 0);
    }
    if (getenv("PYTHONHOME") == NULL) {
        setenv("PYTHONHOME", "/usr", 0);
    }
    // A UTF-8 locale, deliberately. Under LANG=C CPython converts argv through musl's
    // single-byte locale path, and that path returns the first four characters of any argument
    // twelve or more characters long - which silently truncates a script's path. Under a UTF-8
    // locale it takes the other branch and is correct. The underlying bug in that branch is
    // real and still open; this is the configuration the interpreter needs to work.
    if (getenv("LANG") == NULL) {
        setenv("LANG", "C.UTF-8", 0);
    }
    // LC_ALL overrides LANG for every category, so a program that sets only LANG in its own
    // environment cannot fall into the C locale by accident - which is the configuration where
    // CPython truncates its own argv (see docs/CAPABILITIES.md).
    if (getenv("LC_ALL") == NULL) {
        setenv("LC_ALL", "C.UTF-8", 0);
    }
    // Where CPython may put __pycache__. The bundle is read-only, so without this every import
    // is compiled from source: 239 million instructions for `import asyncio` with the cache
    // present, past 600 million without. The bundle now ships precompiled bytecode as well;
    // this is what lets a module compiled at run time be reused on the next one.
    // A writable directory at guest /workspace. The bundle is read-only, so a program writing a
    // scratch file failed with "Operation not permitted" and the smoke test still called it a
    // pass. This is the app's own temporary directory, which is writable and is cleared by the
    // system.
    if (getenv("E2_WORKSPACE") == NULL) {
        const char *tmp = getenv("TMPDIR");
        static char scratch[1024];
        snprintf(scratch, sizeof(scratch), "%s/e2-workspace", tmp != NULL ? tmp : "/tmp");
        mkdir(scratch, 0700);
        setenv("E2_WORKSPACE", scratch, 0);
    }
    if (getenv("PYTHONPYCACHEPREFIX") == NULL) {
        // Set, but deliberately not passed to the guest by the harness: pointing Python at a
        // writable prefix makes it recompile the standard library instead of using the bytecode the
        // bundle ships, and that measured 155 seconds against 24 for `import asyncio`. The variable
        // is kept for a root without precompiled bytecode, where it would help.
    }
}

/// Runs the harness with `argv` and captures stdout into `out_path`.
///
/// Returns the harness's exit status, or -1 if the redirection itself failed. The caller owns
/// `out_path` and should delete the file.
int e2_bridge_run(const char *rootfs, const char *out_path,
                  int argc, char **argv) {
    return e2_bridge_run_in(rootfs, NULL, out_path, argc, argv);
}

// A session: one guest shell kept alive across commands.
//
// The bridge redirects this process's own stdout and stderr to files, so a session owns them for as
// long as it lives. Only one session at a time, and nothing else may print while it runs - which is
// true of the app, where a run happens on a background thread and the UI does not write to stdout.
//
// A thread rather than turn-taking inside one call: the guest blocks in read() until the writer
// thread feeds it the next command, which is what a shell does. Three earlier designs tried to
// arrange the turns from one thread and each stalled for a different reason, all of them recorded in
// docs/persistent-session.md.

static pthread_t g_session_thread;
static int g_session_stdin = -1;
static volatile int g_session_running = 0;
static char g_session_out[1024];

static char g_session_busybox[1024];

static void *session_thread_main(void *arg) {
    (void)arg;
    // The caller reads the guest's output from this file, so the session's stdout has to go there.
    // e2_bridge_run_in does this per call; a session outlives one call and does it once.
    // Nothing to redirect here: the harness points the guest's own descriptor table at the files
    // named by E2_SESSION_OUT and E2_SESSION_MARK. Redirecting this process's stdout used to do it
    // and swallowed every other writer in the process - including a test runner's - which is what
    // hung the iOS suite twice.
    // argv[1] is the host path to the binary: the harness loads it. The guest path is a different
    // thing and is what a program inside sees as its own argv[0].
    int argc = 3;
    char *argv[3] = { (char *)"e2", g_session_busybox, (char *)"sh" };
    e2_rune_main(argc, argv);
    g_session_running = 0;
    return NULL;
}

/// Starts a guest shell that stays alive. Returns 0, or -1 with nothing left behind.
// One session at a time, enforced rather than assumed: two threads inside e2_rune_main wreck each
// other, and a tool layer that calls this from more than one task is exactly how that happens.
static pthread_mutex_t g_session_lock = PTHREAD_MUTEX_INITIALIZER;

int e2_bridge_session_start(const char *rootfs, const char *workspace,
                            const char *out_path, const char *mark_path,
                            const char *busybox) {
    pthread_mutex_lock(&g_session_lock);
    if (g_session_running) {
        pthread_mutex_unlock(&g_session_lock);
        return -1;
    }
    if (workspace != NULL && workspace[0] != '\0') setenv("E2_WORKSPACE", workspace, 1);
    else unsetenv("E2_WORKSPACE");
    if (rootfs != NULL) e2_set_root(rootfs);
    e2_configure_environment();
    setenv("E2_SESSION", "1", 1);
    if (mark_path != NULL) setenv("E2_SESSION_MARK", mark_path, 1);
    else unsetenv("E2_SESSION_MARK");
    if (out_path != NULL) setenv("E2_SESSION_OUT", out_path, 1);
    else unsetenv("E2_SESSION_OUT");
    snprintf(g_session_out, sizeof(g_session_out), "%s", out_path != NULL ? out_path : "");

    int fds[2];
    if (pipe(fds) != 0) return -1;
    // The thread's stdin is the pipe; the write end stays here for e2_bridge_session_write.
    if (dup2(fds[0], STDIN_FILENO) < 0) {
        close(fds[0]); close(fds[1]);
        return -1;
    }
    close(fds[0]);
    g_session_stdin = fds[1];

    snprintf(g_session_busybox, sizeof(g_session_busybox), "%s",
             busybox != NULL ? busybox : "/bin/busybox");

    g_session_running = 1;
    if (pthread_create(&g_session_thread, NULL, session_thread_main, NULL) != 0) {
        g_session_running = 0;
        close(g_session_stdin);
        g_session_stdin = -1;
        pthread_mutex_unlock(&g_session_lock);
        return -1;
    }
    pthread_mutex_unlock(&g_session_lock);
    return 0;
}

/// Sends one command line to the guest shell. Returns 0 on success.
int e2_bridge_session_write(const char *line) {
    if (g_session_stdin < 0 || line == NULL) return -1;
    const size_t n = strlen(line);
    if (write(g_session_stdin, line, n) != (ssize_t)n) return -1;
    if (n == 0 || line[n - 1] != '\n') {
        if (write(g_session_stdin, "\n", 1) != 1) return -1;
    }
    return 0;
}

/// Waits until the marker file has at least `round` lines, or the timeout expires.
///
/// The marker file is how a caller knows a command finished; the output file says what it printed.
/// Polling is the honest implementation here - the harness owns no notification channel - and the
/// interval is short enough that a command's cost dominates.
int e2_bridge_session_wait(int round, int timeout_ms, const char *mark_path) {
    if (mark_path == NULL) return -1;
    const int step = 5;
    for (int waited = 0; waited <= timeout_ms; waited += step) {
        int lines = 0;
        FILE *f = fopen(mark_path, "r");
        if (f != NULL) {
            int ch, prev = '\n';
            while ((ch = fgetc(f)) != EOF) {
                if (ch == '\n') lines++;
                prev = ch;
            }
            if (prev != '\n') lines++;   // a marker written without its newline yet
            fclose(f);
        }
        if (lines >= round) return 0;
        if (!g_session_running) return lines >= round ? 0 : -2;
        struct timespec ts = { 0, (long)step * 1000000L };
        nanosleep(&ts, NULL);
    }
    return -1;
}

/// Ends the session and waits for the shell to go.
void e2_bridge_session_close(void) {
    pthread_mutex_lock(&g_session_lock);
    if (g_session_stdin >= 0) {
        // Closing the write end is what tells the shell there are no more commands.
        close(g_session_stdin);
        g_session_stdin = -1;
    }
    if (g_session_running) {
        pthread_join(g_session_thread, NULL);
        g_session_running = 0;
    }
    // Every variable the session set, not just two of the three. E2_SESSION_OUT left behind made
    // the next one-shot run point the guest's stdout at the session's file - so its output went
    // there and the caller got nothing, which is what made three unrelated tests fail with an empty
    // result.
    unsetenv("E2_SESSION");
    unsetenv("E2_SESSION_MARK");
    unsetenv("E2_SESSION_OUT");
    pthread_mutex_unlock(&g_session_lock);
}

int e2_bridge_session_alive(void) { return g_session_running; }

/// Stops the run in progress, from any thread.
void e2_bridge_cancel(void) { e2_request_cancel(); }

int e2_bridge_run_in(const char *rootfs, const char *workspace, const char *out_path,
                     int argc, char **argv) {
    // A session keeps a thread inside e2_rune_main, and that function is not re-entrant: it resets
    // the descriptor table, the process table and the machine. A run that started while a session
    // was alive would corrupt both - which is what hung the iOS tests, where every test shares one
    // process and a session left over from an earlier one was still there. Ending it first makes
    // that impossible rather than merely unlikely.
    if (e2_bridge_session_alive()) {
        e2_bridge_session_close();
    }
    // A caller that named a workspace wins over the scratch directory: the guest's /workspace is
    // then the project the command was about, not a temporary folder nothing else can see.
    // Set every call, including when there is no workspace: the variable outlives this function,
    // and leaving a previous caller's project mounted would make the next command see files it was
    // never told about. Clearing it lets the default scratch directory apply below.
    if (workspace != NULL && workspace[0] != '\0') {
        setenv("E2_WORKSPACE", workspace, 1);
    } else {
        unsetenv("E2_WORKSPACE");
    }
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
    // Restored before returning, because the first dup2 can succeed while the second fails. The
    // short-circuit || then left stdout pointing at a file that this function closes, and every
    // later write to stdout in the process - for the rest of its life - went to a closed
    // descriptor. One failed run silently broke the ones after it.
    const int out_redirected = dup2(sink, STDOUT_FILENO);
    const int err_redirected = out_redirected < 0 ? -1 : dup2(sink, STDERR_FILENO);
    if (out_redirected < 0 || err_redirected < 0) {
        if (out_redirected >= 0) dup2(saved_out, STDOUT_FILENO);
        if (err_redirected >= 0) dup2(saved_err, STDERR_FILENO);
        close(sink);
        close(saved_out);
        close(saved_err);
        return -1;
    }
    close(sink);

    // Cleared first: a cancellation belongs to the run that asked for it, and a stale flag would
    // stop the next command before it began.
    e2_clear_cancel();
    // A ceiling on what a command may write.
    //
    // The output is captured to a file and the cap on what is read back was applied afterwards, so
    // a command that flooded - yes, a runaway loop, anything reading a binary - filled the device's
    // storage before anything noticed. Sixty-four megabytes is far beyond what a tool result can
    // carry and far below what a phone can spare; past it the write fails, which the guest sees as
    // an error instead of the app being killed for storage.
    struct rlimit file_limit = { 64ULL << 20, 64ULL << 20 };
    struct rlimit previous_limit;
    const int had_limit = (getrlimit(RLIMIT_FSIZE, &previous_limit) == 0);
    setrlimit(RLIMIT_FSIZE, &file_limit);

    const int status = e2_rune_main(argc, argv);

    if (had_limit) {
        setrlimit(RLIMIT_FSIZE, &previous_limit);
    }

    fflush(stdout);
    fflush(stderr);
    dup2(saved_out, STDOUT_FILENO);
    dup2(saved_err, STDERR_FILENO);
    close(saved_out);
    close(saved_err);
    return status;
}
