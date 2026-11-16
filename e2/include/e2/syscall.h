#ifndef E2_SYSCALL_H
#define E2_SYSCALL_H

#include <stddef.h>
#include <stdint.h>

#include "e2/exec.h"

/// Linux AArch64 syscall numbers used so far. The ABI is asm-generic, which is
/// the numbering every aarch64 Linux userspace binary is built against.
#define E2_SYS_dup3 24
#define E2_SYS_fcntl 25
#define E2_SYS_clone 220
#define E2_SYS_execve 221
#define E2_SYS_wait4 260
#define E2_SYS_getcwd 17
#define E2_SYS_chdir 49
#define E2_SYS_openat 56
#define E2_SYS_readlinkat 78
#define E2_SYS_pipe2 59
#define E2_SYS_close 57
#define E2_SYS_lseek 62
#define E2_SYS_read 63
#define E2_SYS_write 64
#define E2_SYS_pread64 67
#define E2_SYS_fstat 80
#define E2_SYS_exit 93
#define E2_SYS_exit_group 94
#define E2_SYS_clock_gettime 113
#define E2_SYS_mmap 222
#define E2_SYS_munmap 215
#define E2_SYS_brk 214
#define E2_SYS_ioctl 29
#define E2_SYS_mprotect 226

/// Host-side file operations. The guest never sees host paths directly: it asks
/// for a path and the host decides whether that path is inside the sandbox and
/// what it maps to. This keeps the policy out of the interpreter.
typedef struct {
    /// Opens `path` (guest-relative) and returns a guest fd >= 0, or negative errno.
    int (*open)(void *ctx, const char *path, int flags, int mode);
    /// Reads up to `len` bytes at `offset`; `offset` of -1 means "use the file
    /// cursor". Returns bytes read, 0 at EOF, or negative errno.
    long (*read)(void *ctx, int fd, void *buf, size_t len, long long offset);
    /// Writes up to `len` bytes at `offset`; -1 means "use the file cursor".
    long (*write)(void *ctx, int fd, const void *buf, size_t len, long long offset);
    /// Closes a guest fd.
    int (*close)(void *ctx, int fd);
    /// Fills `buf` with a struct stat (aarch64 layout). Returns 0 or negative errno.
    int (*stat)(void *ctx, int fd, void *buf, size_t buflen);
    /// Opens a directory for getdents64. Returns a guest fd or negative errno.
    int (*opendir)(void *ctx, const char *path);
    /// Reads one directory entry into a linux_dirent64 record. Writes at most
    /// `buf_size` bytes and returns the record length, 0 at end, or negative
    /// errno. Uses the host's own cursor for that directory.
    long (*readdir)(void *ctx, int fd, void *buf, size_t buf_size);
    /// Current wall clock in nanoseconds since the epoch.
    long long (*clock_ns)(void *ctx);
    /// Creates a pipe and writes its two guest fds into `fds`. Returns 0 or a
    /// negative errno. Optional: shells need it for pipelines.
    int (*pipe)(void *ctx, int fds[2]);
    /// Duplicates `oldfd` onto `newfd` (closing it first). Returns `newfd` or a
    /// negative errno. Optional: shells need it for redirection.
    int (*dup3)(void *ctx, int oldfd, int newfd, int flags);
    /// fcntl(2) for the commands a shell needs: F_DUPFD / F_DUPFD_CLOEXEC and
    /// the FD/FL inspectors. Returns the command's result or a negative errno.
    long (*fcntl)(void *ctx, int fd, int cmd, long arg);
    /// Resolves a symlink. `out` receives the target without a terminator and
    /// the return value is its length, or a negative errno. Python's path
    /// configuration calls os.readlink while normalising sys.path.
    long (*readlink)(void *ctx, const char *path, char *out, size_t outlen);
    /// Whether `fd` is ready for the given poll events (1 = read, 4 = write).
    /// Returns a bitmask of ready events, 0 for "not ready", or negative errno.
    /// Optional: without it, poll reports every open descriptor as ready, which
    /// is correct for regular files and for pipes that hold data.
    int (*pollable)(void *ctx, int fd, int events);
} e2_file_ops;

/// Everything the syscall layer needs from its host. The iOS layer will provide
/// file descriptors backed by the app sandbox; the host test harness maps them
/// onto the real filesystem.
typedef struct {
    /// Called for exit/exit_group. The interpreter stops afterwards.
    void (*exit)(void *ctx, int status);
    /// File operations backing openat/read/write/close/fstat. Required: the
    /// syscall layer routes every file operation through it so that path policy
    /// lives with the host, not in the interpreter.
    const e2_file_ops *files;
    void *ctx;
    /// Forks the calling process. `parent` is the machine to duplicate; the
    /// host decides where the child runs. Returns the child pid in the parent,
    /// 0 in the child's own image, or a negative errno.
    long (*fork)(void *ctx, const e2_machine *parent);
    /// Replaces the current image with the program at `path`. The host builds
    /// the new address space and stack inside `m`. Returns 0 or negative errno.
    int (*execve)(void *ctx, e2_machine *m, const char *path, uint64_t argv,
                  uint64_t envp);
    /// Waits for a child and writes its 32-bit wait status to `status_ptr`.
    /// Returns the reaped pid or a negative errno.
    long (*wait4)(void *ctx, int pid, uint64_t status_ptr, int options);
    /// The guest's current working directory, as it should see it. Kept here
    /// because getcwd has no other source: Python computes sys.path from
    /// os.getcwd(), and returning ENOSYS makes that fail before any file is
    /// opened, which shows up much later as an unrelated crash.
    char cwd[512];
} e2_syscall_env;

/// Services the syscall the machine stopped on.
///
/// Returns E2_OK to continue executing, or E2_HALTED when the guest exited.
/// The return value of the syscall is placed in X0, as the ABI requires.
e2_status e2_service_syscall(e2_machine *machine, e2_syscall_env *env);

#endif
