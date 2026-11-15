#include "ISHBridge.h"

#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "kernel/calls.h"
#include "kernel/task.h"
#include "xX_main_Xx.h"
#include "kernel/fs.h"
#include "kernel/init.h"
#include "kernel/task.h"
#include "fs/devices.h"

static sigjmp_buf ish_jump;
static volatile int ish_exit_code;

static void pocket_exit_hook(struct task *task, int code) {
    (void)task;
    ish_exit_code = code;
    siglongjmp(ish_jump, 1);
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

    dup2(output_fd, STDOUT_FILENO);
    dup2(output_fd, STDERR_FILENO);

    char root_arg[4096];
    char command_arg[8192];
    snprintf(root_arg, sizeof(root_arg), "%s", rootfs);
    snprintf(command_arg, sizeof(command_arg), "%s", command);

    char *argv[] = {
        "ish",
        "-f",
        root_arg,
        "/bin/sh",
        "-c",
        command_arg,
        NULL
    };
    char envp[] = "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin\0";

    ish_exit_code = 0;
    if (sigsetjmp(ish_jump, 1) == 0) {
        int err = xX_main_Xx(6, argv, envp);
        if (err < 0) {
            dprintf(output_fd, "iSH startup failed: %s\n", strerror(-err));
            ish_exit_code = err;
        } else {
            create_some_device_nodes();
            do_mount(&procfs, "proc", "/proc", "", 0);
            do_mount(&devptsfs, "devpts", "/dev/pts", "", 0);
            exit_hook = pocket_exit_hook;
            task_run_current();
        }
    }

    fsync(output_fd);
    dup2(saved_stdout, STDOUT_FILENO);
    dup2(saved_stderr, STDERR_FILENO);
    close(saved_stdout);
    close(saved_stderr);
    close(output_fd);

    return ish_exit_code;
}
