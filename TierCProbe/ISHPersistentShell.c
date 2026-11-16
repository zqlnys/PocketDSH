#include "ISHPersistentShell.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "fs/devices.h"
#include "fs/fd.h"
#include "fs/real.h"
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/init.h"
#include "kernel/task.h"
#include "xX_main_Xx.h"

extern const char *uname_hostname_override;

/// Emitted by the shell after every command so the host can find the boundary
/// without guessing at timing.
static const char kSentinel[] = "__POCKET_DONE__";

#define POCKET_WORKSPACE_POINT "/workspace"
#define POCKET_STOP_TIMEOUT_MS 3000

struct pocket_shell {
    int to_guest[2];   // host writes [1], guest stdin reads [0]
    int from_guest[2]; // guest stdout/stderr writes [1], host reads [0]
    pthread_t thread;
    pthread_mutex_t io_lock;
    volatile int running;
    volatile int exited;
    int workspace_read_only;
    char *rootfs;
    char *workspace;
    char error[256];
};

/// iSH drives one global kernel state (mount list, pid table, fakefs database).
/// Two concurrent emulated shells would corrupt it, so sessions are serialized.
static pthread_mutex_t g_shell_mutex = PTHREAD_MUTEX_INITIALIZER;
static pocket_shell *g_active_shell = NULL;

static void set_error(char *error, size_t size, const char *text) {
    if (error != NULL && size > 0) {
        snprintf(error, size, "%s", text);
    }
}

// MARK: - Read-only realfs

/// realfs stores MS_READONLY_ in mount->flags but never consults it, so a
/// read-only workspace needs a wrapper that refuses writes outright. Reads are
/// forwarded verbatim; every mutating entry point returns EROFS.
static struct fd *readonly_open(struct mount *mount, const char *path, int flags, int mode) {
    if ((flags & O_ACCMODE_) != O_RDONLY_ || (flags & (O_CREAT_ | O_TRUNC_ | O_APPEND_))) {
        return ERR_PTR(_EROFS);
    }
    return realfs_open(mount, path, flags, mode);
}

static int ro_link(struct mount *m, const char *src, const char *dst) {
    (void)m; (void)src; (void)dst; return _EROFS;
}
static int ro_unlink(struct mount *m, const char *path) {
    (void)m; (void)path; return _EROFS;
}
static int ro_rmdir(struct mount *m, const char *path) {
    (void)m; (void)path; return _EROFS;
}
static int ro_rename(struct mount *m, const char *src, const char *dst) {
    (void)m; (void)src; (void)dst; return _EROFS;
}
static int ro_symlink(struct mount *m, const char *target, const char *link) {
    (void)m; (void)target; (void)link; return _EROFS;
}
static int ro_mknod(struct mount *m, const char *path, mode_t_ mode, dev_t_ dev) {
    (void)m; (void)path; (void)mode; (void)dev; return _EROFS;
}
static int ro_mkdir(struct mount *m, const char *path, mode_t_ mode) {
    (void)m; (void)path; (void)mode; return _EROFS;
}
static int ro_setattr(struct mount *m, const char *path, struct attr attr) {
    (void)m; (void)path; (void)attr; return _EROFS;
}
static int ro_fsetattr(struct fd *fd, struct attr attr) {
    (void)fd; (void)attr; return _EROFS;
}
static int ro_utime(struct mount *m, const char *path, struct timespec atime, struct timespec mtime) {
    (void)m; (void)path; (void)atime; (void)mtime; return _EROFS;
}

static int readonly_umount(struct mount *mount) {
    if (mount->root_fd >= 0) {
        close(mount->root_fd);
        mount->root_fd = -1;
    }
    return 0;
}

static const struct fs_ops readonly_realfs = {
    .name = "real-ro", .magic = 0x7265616c,
    .mount = realfs_mount,
    .umount = readonly_umount,
    .statfs = realfs_statfs,

    .open = readonly_open,
    .readlink = realfs_readlink,

    .link = ro_link,
    .unlink = ro_unlink,
    .rmdir = ro_rmdir,
    .rename = ro_rename,
    .symlink = ro_symlink,
    .mknod = ro_mknod,
    .mkdir = ro_mkdir,

    .stat = realfs_stat,
    .fstat = realfs_fstat,
    .setattr = ro_setattr,
    .fsetattr = ro_fsetattr,
    .utime = ro_utime,
    .getpath = realfs_getpath,
    .flock = realfs_flock,
};

/// Set while a shell thread is live so the exit hook can flip its flags.
static pocket_shell *g_exiting_shell = NULL;

/// pid of the emulated /bin/sh itself.
///
/// exit_hook fires for *every* exiting task, including children forked by shell
/// commands. A persistent shell must only tear down its host thread when the
/// shell itself exits — otherwise the first external command (`cat`, `ls`,
/// `uname`, …) takes the whole session with it.
static pid_t_ g_shell_pid = 0;

static void pocket_exit_hook(struct task *task, int code) {
    (void)code;
    if (task != NULL && g_shell_pid != 0 && task->pid != g_shell_pid) {
        return;
    }
    if (g_exiting_shell != NULL) {
        g_exiting_shell->running = 0;
        g_exiting_shell->exited = 1;
    }
    pthread_exit(NULL);
}

/// Wraps a host file descriptor as an iSH fd so the emulated kernel can use it.
/// This mirrors iSH's own create_piped_stdio() helper.
static struct fd *fd_wrap_host(int host_fd, int flags) {
    struct fd *fd = adhoc_fd_create(&realfs_fdops);
    if (fd == NULL) {
        return NULL;
    }
    fd->real_fd = host_fd;
    fd->dir = NULL;
    fd->flags = flags;
    return fd;
}

static void pocket_mount_workspace(const char *workspace, int read_only) {
    if (workspace == NULL || workspace[0] == '\0') {
        return;
    }

    struct stat info;
    if (stat(workspace, &info) != 0) {
        dprintf(STDOUT_FILENO, "[workspace] host path missing: %s (errno=%d)\n",
                workspace, errno);
        return;
    }

    do_umount(POCKET_WORKSPACE_POINT);
    const struct fs_ops *fs = read_only ? &readonly_realfs : &realfs;
    int err = do_mount(fs, workspace, POCKET_WORKSPACE_POINT, "", 0);
    if (err < 0) {
        dprintf(STDOUT_FILENO, "[workspace] mount failed: errno=%d\n", err);
        return;
    }
    dprintf(STDOUT_FILENO, "[workspace] mounted %s -> %s (%s)\n",
            workspace, POCKET_WORKSPACE_POINT, read_only ? "read-only" : "read-write");
}

static int install_stdio(pocket_shell *shell) {
    struct fd *stdin_fd = fd_wrap_host(shell->to_guest[0], O_RDONLY_);
    struct fd *stdout_fd = fd_wrap_host(shell->from_guest[1], O_WRONLY_);
    if (stdin_fd == NULL || stdout_fd == NULL) {
        if (stdin_fd != NULL) fd_close(stdin_fd);
        if (stdout_fd != NULL) fd_close(stdout_fd);
        return -1;
    }

    current->files->files[0] = stdin_fd;
    current->files->files[1] = fd_retain(stdout_fd);
    current->files->files[2] = stdout_fd;
    return 0;
}

static void *shell_thread(void *argument) {
    pocket_shell *shell = argument;

    char *argv[] = {
        "ish",
        "-f",
        shell->rootfs,
        "/bin/sh",
        NULL
    };
    char envp[] =
        "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin\0"
        "HOME=/root\0"
        "TERM=dumb\0";

    uname_hostname_override = "pocketdsh";
    int err = xX_main_Xx(4, argv, envp);
    if (err < 0) {
        snprintf(shell->error, sizeof(shell->error),
                 "xX_main_Xx failed: %d", err);
        shell->running = 0;
        shell->exited = 1;
        return NULL;
    }

    create_some_device_nodes();
    do_mount(&procfs, "proc", "/proc", "", 0);
    do_mount(&devptsfs, "devpts", "/dev/pts", "", 0);
    pocket_mount_workspace(shell->workspace, shell->workspace_read_only);

    if (install_stdio(shell) != 0) {
        snprintf(shell->error, sizeof(shell->error), "failed to install stdio pipes");
        shell->running = 0;
        shell->exited = 1;
        return NULL;
    }

    g_exiting_shell = shell;
    g_shell_pid = current->pid;
    exit_hook = pocket_exit_hook;
    task_run_current();
    g_shell_pid = 0;
    g_exiting_shell = NULL;

    shell->running = 0;
    shell->exited = 1;
    return NULL;
}

pocket_shell *pocket_shell_start(const char *rootfs, const char *workspace,
                                 int workspace_read_only,
                                 char *error, size_t error_size) {
    // Callers may hand us an uninitialised buffer; normalise it before any path
    // can read from it.
    set_error(error, error_size, "");

    if (rootfs == NULL) {
        set_error(error, error_size, "rootfs path is required");
        return NULL;
    }

    if (pthread_mutex_trylock(&g_shell_mutex) != 0) {
        set_error(error, error_size, "another shell session is active");
        return NULL;
    }

    pocket_shell *shell = calloc(1, sizeof(pocket_shell));
    if (shell == NULL) {
        pthread_mutex_unlock(&g_shell_mutex);
        set_error(error, error_size, "out of memory");
        return NULL;
    }

    shell->to_guest[0] = shell->to_guest[1] = -1;
    shell->from_guest[0] = shell->from_guest[1] = -1;

    if (pipe(shell->to_guest) != 0 || pipe(shell->from_guest) != 0) {
        set_error(error, error_size, "pipe() failed");
        goto fail;
    }

    // Keep the guest side blocking; the host side polls with timeouts.
    fcntl(shell->to_guest[0], F_SETFL, 0);
    fcntl(shell->from_guest[0], F_SETFL, 0);

    // A write into a pipe with no reader raises SIGPIPE, which kills the process
    // by default. Do not depend on some other subsystem having ignored it.
    signal(SIGPIPE, SIG_IGN);

    shell->rootfs = strdup(rootfs);
    shell->workspace = workspace != NULL ? strdup(workspace) : NULL;
    shell->workspace_read_only = workspace_read_only;
    pthread_mutex_init(&shell->io_lock, NULL);
    shell->running = 1;

    int create_error = pthread_create(&shell->thread, NULL, shell_thread, shell);
    if (create_error != 0) {
        snprintf(shell->error, sizeof(shell->error),
                 "pthread_create failed: %d", create_error);
        set_error(error, error_size, shell->error);
        goto fail;
    }

    g_active_shell = shell;
    pthread_mutex_unlock(&g_shell_mutex);
    return shell;

fail:
    if (shell->to_guest[0] >= 0) close(shell->to_guest[0]);
    if (shell->to_guest[1] >= 0) close(shell->to_guest[1]);
    if (shell->from_guest[0] >= 0) close(shell->from_guest[0]);
    if (shell->from_guest[1] >= 0) close(shell->from_guest[1]);
    free(shell->rootfs);
    free(shell->workspace);
    free(shell);
    pthread_mutex_unlock(&g_shell_mutex);
    if (error != NULL && error_size > 0 && error[0] == '\0') {
        set_error(error, error_size, "shell start failed");
    }
    return NULL;
}

int pocket_shell_is_running(pocket_shell *shell) {
    return shell != NULL && shell->running && !shell->exited;
}

long pocket_shell_exec(pocket_shell *shell, const char *command,
                       char *out, size_t out_size, int timeout_ms,
                       int *truncated) {
    if (shell == NULL || command == NULL || out == NULL || out_size == 0) {
        return -EINVAL;
    }
    if (truncated != NULL) {
        *truncated = 0;
    }
    out[0] = '\0';

    if (!pocket_shell_is_running(shell)) {
        return -EPIPE;
    }

    pthread_mutex_lock(&shell->io_lock);

    // Strip a trailing newline; the wrapper adds its own.
    size_t command_len = strlen(command);
    while (command_len > 0
           && (command[command_len - 1] == '\n' || command[command_len - 1] == '\r')) {
        command_len--;
    }

    char *payload = malloc(command_len + sizeof(kSentinel) + 32);
    if (payload == NULL) {
        pthread_mutex_unlock(&shell->io_lock);
        return -ENOMEM;
    }
    int payload_len = snprintf(payload, command_len + sizeof(kSentinel) + 32,
                               "%.*s\necho %s\n",
                               (int)command_len, command, kSentinel);

    ssize_t written = 0;
    while (written < payload_len) {
        ssize_t n = write(shell->to_guest[1], payload + written,
                          (size_t)(payload_len - written));
        if (n < 0) {
            if (errno == EINTR) continue;
            free(payload);
            pthread_mutex_unlock(&shell->io_lock);
            return -errno;
        }
        written += n;
    }
    free(payload);

    size_t used = 0;
    int found_sentinel = 0;
    int timed_out = 0;
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);

    while (!found_sentinel) {
        struct pollfd pfd = {.fd = shell->from_guest[0], .events = POLLIN};
        int ready = poll(&pfd, 1, 100);
        if (ready > 0) {
            char chunk[1024];
            ssize_t n = read(shell->from_guest[0], chunk, sizeof(chunk) - 1);
            if (n > 0) {
                chunk[n] = '\0';
                if (used + (size_t)n < out_size) {
                    memcpy(out + used, chunk, (size_t)n);
                    used += (size_t)n;
                    out[used] = '\0';
                } else {
                    if (truncated != NULL) *truncated = 1;
                    break;
                }
                if (strstr(out, kSentinel) != NULL) {
                    found_sentinel = 1;
                }
            } else if (n == 0) {
                // Guest side closed its output pipe: no further output can ever
                // arrive, so this session is unusable even if the thread runs on.
                shell->running = 0;
                shell->exited = 1;
                break;
            }
        } else if (ready < 0 && errno != EINTR) {
            break;
        }

        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed = (now.tv_sec - start.tv_sec) * 1000L
                     + (now.tv_nsec - start.tv_nsec) / 1000000L;
        if (elapsed >= timeout_ms) {
            if (truncated != NULL) *truncated = 1;
            timed_out = 1;
            break;
        }
    }

    // Strip the sentinel line and trailing whitespace from the payload.
    char *marker = strstr(out, kSentinel);
    if (marker != NULL) {
        while (marker > out && (marker[-1] == '\n' || marker[-1] == '\r')) {
            marker--;
        }
        *marker = '\0';
        used = (size_t)(marker - out);
    }

    pthread_mutex_unlock(&shell->io_lock);

    if (timed_out) {
        // The command may still be running and its output will land in the pipe
        // later, which would corrupt the next command's boundaries. Report the
        // timeout so the caller can rebuild the session.
        return POCKET_SHELL_TIMEOUT;
    }
    return (long)used;
}

void pocket_shell_stop(pocket_shell *shell) {
    if (shell == NULL) {
        return;
    }

    if (pocket_shell_is_running(shell)) {
        const char *bye = "exit\n";
        ssize_t ignored = write(shell->to_guest[1], bye, strlen(bye));
        (void)ignored;

        struct timespec start, now;
        clock_gettime(CLOCK_MONOTONIC, &start);
        while (!shell->exited) {
            usleep(20 * 1000);
            clock_gettime(CLOCK_MONOTONIC, &now);
            long elapsed = (now.tv_sec - start.tv_sec) * 1000L
                         + (now.tv_nsec - start.tv_nsec) / 1000000L;
            if (elapsed >= POCKET_STOP_TIMEOUT_MS) {
                break;
            }
        }
        if (shell->exited) {
            pthread_join(shell->thread, NULL);
        } else {
            pthread_cancel(shell->thread);
            pthread_detach(shell->thread);
            // Deliberately not freed: the thread may still touch this struct.
            if (g_active_shell == shell) {
                g_active_shell = NULL;
                pthread_mutex_unlock(&g_shell_mutex);
            }
            return;
        }
    }

    if (shell->to_guest[0] >= 0) close(shell->to_guest[0]);
    if (shell->to_guest[1] >= 0) close(shell->to_guest[1]);
    if (shell->from_guest[0] >= 0) close(shell->from_guest[0]);
    if (shell->from_guest[1] >= 0) close(shell->from_guest[1]);

    pthread_mutex_destroy(&shell->io_lock);
    free(shell->rootfs);
    free(shell->workspace);

    if (g_active_shell == shell) {
        g_active_shell = NULL;
        pthread_mutex_unlock(&g_shell_mutex);
    }
    free(shell);
}
