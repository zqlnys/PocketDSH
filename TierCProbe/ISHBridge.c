#include "ISHBridge.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "kernel/calls.h"
#include "kernel/task.h"
#include "xX_main_Xx.h"
#include "kernel/fs.h"
#include "kernel/init.h"
#include "fs/devices.h"

extern const char *uname_hostname_override;

static char shell_root[4096];
static int shell_in = -1;
static int shell_out = -1;
static int shell_err = -1;
static volatile int shell_running;
static volatile int shell_started;

static void shell_exit_hook(struct task *task, int code) {
    (void)task;
    (void)code;
    shell_running = 0;
    pthread_exit(NULL);
}

static void *shell_thread(void *unused) {
    (void)unused;

    char *argv[] = {"ish", "-f", shell_root, "/bin/sh", NULL};
    char envp[] = "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin\0";

    uname_hostname_override = "pocketdsh";
    int err = xX_main_Xx(5, argv, envp);
    if (err < 0) {
        shell_started = -err;
        return NULL;
    }
    shell_started = 1;
    create_some_device_nodes();
    do_mount(&procfs, "proc", "/proc", "", 0);
    do_mount(&devptsfs, "devpts", "/dev/pts", "", 0);
    exit_hook = shell_exit_hook;
    shell_running = 1;
    task_run_current();
    return NULL;
}

int pocket_ish_start(const char *rootfs) {
    if (shell_in >= 0 || rootfs == NULL) {
        return -EALREADY;
    }

    int input_pipe[2];
    int output_pipe[2];
    if (pipe(input_pipe) != 0 || pipe(output_pipe) != 0) {
        return -errno;
    }

    int saved_stdin = dup(STDIN_FILENO);
    int saved_stdout = dup(STDOUT_FILENO);
    int saved_stderr = dup(STDERR_FILENO);
    dup2(input_pipe[0], STDIN_FILENO);
    dup2(output_pipe[1], STDOUT_FILENO);
    dup2(output_pipe[1], STDERR_FILENO);
    close(input_pipe[0]);
    close(output_pipe[1]);

    shell_in = input_pipe[1];
    shell_out = output_pipe[0];
    shell_err = dup(shell_out);
    snprintf(shell_root, sizeof(shell_root), "%s", rootfs);
    shell_started = 0;
    shell_running = 0;

    pthread_t thread;
    int create_error = pthread_create(&thread, NULL, shell_thread, NULL);
    if (create_error != 0) {
        dup2(saved_stdin, STDIN_FILENO);
        dup2(saved_stdout, STDOUT_FILENO);
        dup2(saved_stderr, STDERR_FILENO);
        close(saved_stdin);
        close(saved_stdout);
        close(saved_stderr);
        close(shell_in);
        close(shell_out);
        close(shell_err);
        shell_in = shell_out = shell_err = -1;
        return -create_error;
    }

    // xX_main_Xx wraps these host descriptors before returning; restoring
    // the app's original descriptors keeps logging and UIKit unaffected.
    for (int i = 0; i < 100 && shell_started == 0; i++) {
        usleep(10_000);
    }
    dup2(saved_stdin, STDIN_FILENO);
    dup2(saved_stdout, STDOUT_FILENO);
    dup2(saved_stderr, STDERR_FILENO);
    close(saved_stdin);
    close(saved_stdout);
    close(saved_stderr);

    if (shell_started < 0) {
        return shell_started;
    }
    return 0;
}

int pocket_ish_send(const char *command) {
    if (shell_in < 0 || command == NULL) {
        return -EINVAL;
    }
    size_t length = strlen(command);
    ssize_t written = write(shell_in, command, length);
    if (written < 0) {
        return -errno;
    }
    return (int)written;
}

int pocket_ish_read(char *buffer, size_t capacity, int timeout_ms) {
    if (shell_out < 0 || buffer == NULL || capacity == 0) {
        return -EINVAL;
    }
    struct pollfd descriptor = {.fd = shell_out, .events = POLLIN};
    int ready = poll(&descriptor, 1, timeout_ms);
    if (ready < 0) {
        return -errno;
    }
    if (ready == 0) {
        return 0;
    }
    ssize_t count = read(shell_out, buffer, capacity);
    if (count < 0) {
        return -errno;
    }
    return (int)count;
}

int pocket_ish_is_running(void) {
    return shell_running;
}

void pocket_ish_close(void) {
    if (shell_in >= 0) {
        (void)write(shell_in, "exit\n", 5);
        close(shell_in);
        shell_in = -1;
    }
    if (shell_out >= 0) {
        close(shell_out);
        shell_out = -1;
    }
    if (shell_err >= 0) {
        close(shell_err);
        shell_err = -1;
    }
    shell_running = 0;
}
