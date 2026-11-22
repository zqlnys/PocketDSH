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

/// Operations for the optional `pathop` callback.
#define E2_PATHOP_MKDIR 1
#define E2_PATHOP_UNLINK 2
#define E2_PATHOP_RMDIR 3
#define E2_PATHOP_RENAME 4
#define E2_PATHOP_SYMLINK 5
#define E2_PATHOP_LINK 6
#define E2_PATHOP_CHMOD 7
#define E2_PATHOP_ACCESS 8
#define E2_PATHOP_UTIME 9
// Ownership change. Mostly a no-op inside the sandbox - everything runs as one
// user - but scripts call it, and "Function not implemented" stops them.
#define E2_PATHOP_CHOWN 10
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
    long long (*clock_ns)(void *ctx, int clock_id);
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
    /// AF_UNIX socketpair, for the self-pipe every asyncio event loop builds in its
    /// constructor. Optional: a host without it leaves clone-adjacent code paths intact and
    /// socketpair reports EAFNOSUPPORT.
    int (*socketpair)(void *ctx, int domain, int type, int protocol, int fds[2], int *err);
    /// Clears or sets the blocking flag behind a guest descriptor. socket.setblocking(False)
    /// needs it, and asyncio calls that on its self-pipe before it does anything else.
    /// Returns 0 or a negative errno.
    long (*set_nonblock)(void *ctx, int fd, int nonblocking);
    /// Bytes readable right now, or a negative errno. FIONREAD.
    long (*bytes_available)(void *ctx, int fd);
    /// Runs whatever could make progress and reports whether anything did.
    ///
    /// poll must be able to wait: a guest that polls a descriptor nothing has written yet would
    /// otherwise be told "nothing ready" immediately and call again, forever. This is the
    /// scheduler's turn to run children, the same way a blocking read yields.
    int (*wait_for_progress)(void *ctx);
    /// Filesystem mutations that do not go through an open descriptor, selected
    /// by `op` (see E2_PATHOP_*). `a` and `b` are guest-relative paths, `mode`
    /// is a permission or timestamp. Returns 0 or a negative errno. Optional:
    /// without it every mutating call reports ENOSYS, which is why mkdir, rm,
    /// ln, chmod and touch all failed at once.
    int (*pathop)(void *ctx, int op, const char *a, const char *b, unsigned mode);
    /// Sets a file's length. `fd` >= 0 truncates that descriptor, otherwise the
    /// path is used. Returns 0 or a negative errno. Optional: without it
    /// truncate and ftruncate report ENOSYS, which is what `truncate -s` hit.
    int (*truncate_file)(void *ctx, int fd, const char *path, long long length);
    /// Fills the guest's `statfs` layout for a descriptor or a path. Returns 0 or a
    /// negative errno. Optional: without it df and stat -f report ENOSYS.
    int (*statfs_of)(void *ctx, int fd, const char *path, void *out, size_t outlen);
    /// Repositions a descriptor's cursor. Returns the new offset or a negative errno.
    /// Required for anything that reads a file twice: diff seeks back to the start,
    /// and without a real seek the second read returns EOF and the two files compare
    /// equal.
    long (*seek)(void *ctx, int fd, long long offset, int whence);
    /// Stats a path directly. `follow` selects between stat and lstat: a dangling
    /// symlink has no stat result but does have an lstat one, and tools that delete
    /// link names - rm among them - ask for the latter before touching anything.
    /// Optional: without it newfstatat always follows the link.
    int (*stat_path)(void *ctx, const char *path, void *st, size_t stlen, int follow);
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
    /// Creates a thread: same address space, new CPU state, entry at `pc`, stack at `sp`.
    /// Returns the new thread's id for the child, or a negative errno. Optional - a host
    /// that cannot run threads simply leaves it NULL and clone() reports EINVAL as before.
    /// Creates a thread. `tls` is clone's TL argument and becomes the new thread's
    /// TPIDR_EL0 - musl's pthread_create passes one and the thread cannot run without it.
    /// `ptid` and `ctid` are clone's parent and child tid pointers, written when the matching
    /// CLONE_*_SETTID flags are set. musl's pthread_create sets both and waits on the result.
    long (*thread)(void *ctx, const e2_machine *parent, uint64_t pc, uint64_t sp, uint64_t tls,
                   uint64_t ptid, uint64_t ctid, uint64_t flags);
    /// FUTEX_WAIT: returns 0 when the caller should block, -EAGAIN when the word no longer
    /// holds `val`. A host that blocks must also end the current turn itself; `*yield` is set
    /// when it has done so.
    long (*futex_wait)(void *ctx, uint64_t addr, uint64_t val, int *yield);
    /// FUTEX_WAKE: wakes up to `count` waiters on `addr` and returns how many it woke.
    long (*futex_wake)(void *ctx, uint64_t addr, uint64_t count);
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
    /// This process's pid, as getpid must report it. Kept here because returning a
    /// constant is not an option: 0 means "the child, in a fork", so a guest that
    /// asks for its own pid and gets 0 takes the wrong branch, and `$$` in a shell
    /// becomes an unusable name for temporary files.
    int pid;
    /// The parent's pid, for getppid. 0 for the first process.
    int ppid;
    /// Set whenever cwd changes, so the caller does not have to compare strings after every
    /// instruction to notice. Two strcmp calls per step measured 10 ns of 86, which is more
    /// than the bookkeeping is worth.
    int cwd_changed;
} e2_syscall_env;

/// Services the syscall the machine stopped on.
///
/// Returns E2_OK to continue executing, or E2_HALTED when the guest exited.
/// The return value of the syscall is placed in X0, as the ABI requires.
e2_status e2_service_syscall(e2_machine *machine, e2_syscall_env *env);

#endif
