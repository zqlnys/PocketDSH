#include "ISHBridge.h"

#include <errno.h>
#include <fcntl.h>
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

static volatile int ish_exit_code;
static char thread_root[4096];
static char thread_command[8192];

static void pocket_exit_hook(struct task *task, int code) {
    (void)task;
    ish_exit_code = code;
    pthread_exit(NULL);
}

static void *ish_thread(void *unused) {
    (void)unused;
    char *argv[] = {
        "ish",
        "-f",
        thread_root,
        "/bin/sh",
        "-c",
        thread_command,
        NULL
    };
    char envp[] = "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin\0";

    uname_hostname_override = "pocketdsh";
    int err = xX_main_Xx(6, argv, envp);
    if (err < 0) {
        ish_exit_code = err;
        return NULL;
    }
    create_some_device_nodes();
    do_mount(&procfs, "proc", "/proc", "", 0);
    do_mount(&devptsfs, "devpts", "/dev/pts", "", 0);
    exit_hook = pocket_exit_hook;
    task_run_current();
    return NULL;
}

int pocket_ish_run(const char *rootfs, const char *command, const char *output_path) {
    if (rootfs == NULL || command == NULL || output_path == NULL) {
        return -EINVAL;
    }

    int output_fd = open(output_path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (output_fd < 0) {
        return -errno;
    }

    int saved_stdout = dup(STDOUT_FILENO);
    int saved_stderr = dup(STDERR_FILENO);
    if (saved_stdout < 0 || saved_stderr < 0) {
        close(output_fd);
        return -errno;
    }

    snprintf(thread_root, sizeof(thread_root), "%s", rootfs);
    snprintf(thread_command, sizeof(thread_command), "%s", command);
    ish_exit_code = 0;

    dup2(output_fd, STDOUT_FILENO);
    dup2(output_fd, STDERR_FILENO);

    pthread_t thread;
    int create_error = pthread_create(&thread, NULL, ish_thread, NULL);
    if (create_error != 0) {
        dup2(saved_stdout, STDOUT_FILENO);
        dup2(saved_stderr, STDERR_FILENO);
        close(saved_stdout);
        close(saved_stderr);
        close(output_fd);
        return -create_error;
    }

    pthread_join(thread, NULL);
    fsync(output_fd);
    dup2(saved_stdout, STDOUT_FILENO);
    dup2(saved_stderr, STDERR_FILENO);
    close(saved_stdout);
    close(saved_stderr);
    close(output_fd);

    return ish_exit_code;
}
