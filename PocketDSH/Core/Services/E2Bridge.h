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

/// Starts a guest shell that stays alive across commands.
///
/// The bridge redirects this process's stdout and stderr while a run is in progress, so a session
/// owns them for as long as it lives: one session at a time, and nothing else may print meanwhile.
int e2_bridge_session_start(const char *rootfs, const char *workspace,
                            const char *out_path, const char *mark_path,
                            const char *busybox);

/// Sends one command line to the session's shell. Returns 0 on success.
int e2_bridge_session_write(const char *line);

/// Waits until the marker file has at least `round` lines. 0 when it does, -1 on timeout,
/// -2 when the shell has gone without producing them.
int e2_bridge_session_wait(int round, int timeout_ms, const char *mark_path);

/// True while the session's thread is running.
int e2_bridge_session_alive(void);

/// Ends the session and waits for the shell to exit.
void e2_bridge_session_close(void);

#endif

/// Stops whatever run is in progress, from any thread. The run ends with a non-zero status and
/// its output so far; the next call clears the request.
void e2_bridge_cancel(void);
