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

static int fakefs_insert(sqlite3 *db, const char *path, unsigned mode) {
    sqlite3_stmt *stat_stmt = NULL;
    if (sqlite3_prepare_v2(db, "INSERT INTO stats(stat) VALUES(?)", -1, &stat_stmt, NULL) != SQLITE_OK) {
        return -1;
    }
    struct ish_stat stat = {.mode = mode, .uid = 0, .gid = 0, .rdev = 0};
    sqlite3_bind_blob(stat_stmt, 1, &stat, sizeof(stat), SQLITE_TRANSIENT);
    int rc = sqlite3_step(stat_stmt);
    sqlite3_finalize(stat_stmt);
    if (rc != SQLITE_DONE) return -1;
    sqlite3_int64 inode = sqlite3_last_insert_rowid(db);

    sqlite3_stmt *path_stmt = NULL;
    if (sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO paths(path, inode) VALUES(?, ?)", -1, &path_stmt, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(path_stmt, 1, path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(path_stmt, 2, inode);
    rc = sqlite3_step(path_stmt);
    sqlite3_finalize(path_stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

static int fakefs_add_tree(sqlite3 *db, const char *source, const char *destination) {
    struct stat info;
    if (lstat(source, &info) != 0) return -1;
    unsigned mode = S_ISDIR(info.st_mode) ? (S_IFDIR | 0755) : (S_IFREG | 0644);
    if (fakefs_insert(db, destination, mode) != 0) return -1;
    if (!S_ISDIR(info.st_mode)) return 0;

    DIR *directory = opendir(source);
    if (directory == NULL) return -1;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char child_source[4096];
        char child_dest[4096];
        snprintf(child_source, sizeof(child_source), "%s/%s", source, entry->d_name);
        snprintf(child_dest, sizeof(child_dest), "%s/%s", destination, entry->d_name);
        if (fakefs_add_tree(db, child_source, child_dest) != 0) {
            closedir(directory);
            return -1;
        }
    }
    closedir(directory);
    return 0;
}

int pocket_ish_register_workspace(const char *rootfs, const char *workspace_path) {
    if (rootfs == NULL || workspace_path == NULL) return -EINVAL;
    char database_path[4096];
    snprintf(database_path, sizeof(database_path), "%s/meta.db", rootfs);
    sqlite3 *db = NULL;
    if (sqlite3_open(database_path, &db) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    int rc = sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
    if (rc == SQLITE_OK) {
        rc = fakefs_add_tree(db, workspace_path, "/workspace");
    }
    if (rc == SQLITE_OK) {
        rc = sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
    } else {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    }
    sqlite3_close(db);
    return rc == SQLITE_OK ? 0 : -1;
}
