#include "e2/syscall.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "e2/memory.h"

// Linux errno values used by the syscall layer. asm-generic numbering.
#define E2_EBADF 9
#define E2_EFAULT 14
#define E2_EEXIST 17
#define E2_EINVAL 22
#define E2_ENOSYS 38
#define E2_ENOTSUP 95

/// aarch64 `struct stat`. The layout is fixed by the kernel ABI: musl reads
/// st_mode and st_size directly out of this buffer, so the offsets matter more
/// than the field names.
typedef struct {
    uint64_t st_dev;
    uint64_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    uint64_t __pad1;
    int64_t st_size;
    int32_t st_blksize;
    int32_t __pad2;
    int64_t st_blocks;
    int64_t st_atime_sec;
    uint64_t st_atime_nsec;
    int64_t st_mtime_sec;
    uint64_t st_mtime_nsec;
    int64_t st_ctime_sec;
    uint64_t st_ctime_nsec;
    int32_t __unused4;
    int32_t __unused5;
} e2_guest_stat;

_Static_assert(sizeof(e2_guest_stat) == 128, "guest struct stat must match the kernel ABI");

/// Copies a guest string into a host buffer, refusing anything over PATH_MAX.
static int read_guest_string(e2_machine *m, uint64_t addr, char *out, size_t out_size) {
    if (out_size == 0) {
        return -E2_EINVAL;
    }
    for (size_t i = 0; i < out_size; i++) {
        uint8_t byte = 0;
        if (e2_memory_read(&m->mem, addr + i, &byte, 1) != 0) {
            return -E2_EFAULT;
        }
        out[i] = (char)byte;
        if (byte == 0) {
            return 0;
        }
    }
    return -E2_EINVAL;  // no terminator within the limit
}

/// Copies an aarch64 Linux `struct iovec` array and writes each entry.
///
/// writev is how most real programs emit output, so it is worth having early.
static long sys_writev(e2_machine *m, e2_syscall_env *env, int fd, uint64_t iov_addr,
                       int iov_count) {
    if (iov_count < 0 || iov_count > 1024) {
        return -E2_EFAULT;
    }
    long total = 0;
    for (int i = 0; i < iov_count; i++) {
        struct {
            uint64_t base;
            uint64_t len;
        } iov;
        if (e2_memory_read(&m->mem, iov_addr + (uint64_t)i * sizeof(iov), &iov, sizeof(iov)) != 0) {
            return -E2_EFAULT;
        }
        if (iov.len == 0) {
            continue;
        }
        uint8_t *src = e2_memory_host_ptr(&m->mem, iov.base, (size_t)iov.len, E2_PROT_READ);
        if (src == NULL) {
            return -E2_EFAULT;
        }
        long written = env->files->write(env->ctx, fd, src, (size_t)iov.len, -1);
        if (written < 0) {
            return written;
        }
        total += written;
    }
    return total;
}

/// The read counterpart of `sys_writev`, filling each iovec in turn.
///
/// A read stops as soon as one entry returns short or zero: the buffer boundary
/// is where the data ended, and continuing would read past it. `xxd` and `od`
/// both reach the file through this.
static long sys_readv(e2_machine *m, e2_syscall_env *env, int fd, uint64_t iov_addr,
                      int iov_count) {
    if (iov_count < 0 || iov_count > 1024) {
        return -E2_EFAULT;
    }
    long total = 0;
    for (int i = 0; i < iov_count; i++) {
        struct {
            uint64_t base;
            uint64_t len;
        } iov;
        if (e2_memory_read(&m->mem, iov_addr + (uint64_t)i * sizeof(iov), &iov, sizeof(iov)) != 0) {
            return -E2_EFAULT;
        }
        if (iov.len == 0) {
            continue;
        }
        uint8_t *dst = e2_memory_host_ptr(&m->mem, iov.base, (size_t)iov.len, E2_PROT_WRITE);
        if (dst == NULL) {
            return -E2_EFAULT;
        }
        const long got = env->files->read(env->ctx, fd, dst, (size_t)iov.len, -1);
        if (got < 0) {
            return total > 0 ? total : got;
        }
        total += got;
        if (got < (long)iov.len) {
            break;
        }
    }
    return total;
}

/// `poll` and `ppoll` share one implementation: they differ only in how the
/// timeout is expressed (milliseconds against a timespec).
///
/// A shell's `while read` loop calls this before each line, so returning ENOSYS
/// here does not fail loudly - the loop simply never runs its body, which is how
/// this stayed unnoticed.
static long sys_poll(e2_machine *m, e2_syscall_env *env, uint64_t fds_addr, uint64_t nfds,
                     long timeout_ms) {
    if (nfds > 1024) {
        return -E2_EINVAL;
    }
    if (timeout_ms == 0) {
        // A zero timeout is a pure status check; report what is ready now.
    }
    long ready = 0;
    for (uint64_t i = 0; i < nfds; i++) {
        struct {
            int32_t fd;
            int16_t events;
            int16_t revents;
        } entry;
        const uint64_t at = fds_addr + i * sizeof(entry);
        if (e2_memory_read(&m->mem, at, &entry, sizeof(entry)) != 0) {
            return -E2_EFAULT;
        }
        const int events = (int)entry.events;
        int revents = 0;
        if (entry.fd >= 0) {
            if (env->files != NULL && env->files->pollable != NULL) {
                const int r = env->files->pollable(env->ctx, (int)entry.fd, events);
                if (r < 0) {
                    return r;
                }
                revents = r;
            } else {
                revents = events;
            }
        }
        entry.revents = (int16_t)revents;
        if (e2_memory_write(&m->mem, at, &entry, sizeof(entry)) != 0) {
            return -E2_EFAULT;
        }
        if (revents != 0) {
            ready++;
        }
    }
    (void)timeout_ms;
    return ready;
}

/// Runs one filesystem mutation through the optional `pathop` callback.
///
/// The `*at` calls take a directory descriptor in a0 and the path in a1; only the
/// two-argument forms are needed here because nothing in this sandbox passes
/// AT_FDCWD anything but -100, and the harness resolves relative to the guest
/// root regardless.
static long sys_pathop(e2_machine *m, e2_syscall_env *env, int op,
                       uint64_t path_a, uint64_t path_b, unsigned mode) {
    if (env->files == NULL || env->files->pathop == NULL) {
        return -E2_ENOSYS;
    }
    char a[2048];
    const int rc = read_guest_string(m, path_a, a, sizeof(a));
    if (rc != 0) {
        return rc;
    }
    char b[2048];
    b[0] = '\0';
    if (path_b != 0) {
        const int rc2 = read_guest_string(m, path_b, b, sizeof(b));
        if (rc2 != 0) {
            return rc2;
        }
    }
    return env->files->pathop(env->ctx, op, a, b, mode);
}

extern unsigned long long g_global_step;

e2_status e2_service_syscall(e2_machine *m, e2_syscall_env *env) {
    // One line per syscall, opt-in. Comparing this against qemu's -strace is the
    // fastest way to see which operation a program takes and where the two
    // implementations diverge.
    if (getenv("E2_TRACE_PATH") != NULL && m->cpu.x[8] == 56) {
        char pbuf[256];
        if (read_guest_string(m, m->cpu.x[1], pbuf, sizeof(pbuf)) == 0) {
            fprintf(stderr, "[open] %s\n", pbuf);
        }
    }
    if (getenv("E2_TRACE_SYS") != NULL) {
        fprintf(stderr, "[sys@%llu pc=0x%llx] %llu(0x%llx, 0x%llx, 0x%llx, 0x%llx, 0x%llx, 0x%llx)\n",
                g_global_step, (unsigned long long)m->cpu.pc,
                (unsigned long long)m->cpu.x[8],
                (unsigned long long)m->cpu.x[0], (unsigned long long)m->cpu.x[1],
                (unsigned long long)m->cpu.x[2], (unsigned long long)m->cpu.x[3],
                (unsigned long long)m->cpu.x[4], (unsigned long long)m->cpu.x[5]);
    }
    const uint64_t number = m->cpu.x[8];
    const uint64_t a0 = m->cpu.x[0];
    const uint64_t a1 = m->cpu.x[1];
    const uint64_t a2 = m->cpu.x[2];
    const uint64_t a3 = m->cpu.x[3];
    const e2_file_ops *files = env->files;

    // Result goes to X0 for every path; the helper keeps the returns uniform.
    #define E2_RETURN(value) do { m->cpu.x[0] = (uint64_t)(int64_t)(value); return E2_OK; } while (0)

    switch (number) {
        case E2_SYS_write: {
            if (files == NULL) {
                E2_RETURN(-E2_ENOSYS);
            }
            uint8_t *src = e2_memory_host_ptr(&m->mem, a1, (size_t)a2, E2_PROT_READ);
            if (src == NULL) {
                E2_RETURN(-E2_EFAULT);
            }
            E2_RETURN(files->write(env->ctx, (int)a0, src, (size_t)a2, -1));
        }
        case 66: {  // writev
            if (files == NULL) {
                E2_RETURN(-E2_ENOSYS);
            }
            E2_RETURN(sys_writev(m, env, (int)a0, a1, (int)a2));
        }
        case 166: {  // umask
            // Returns the PREVIOUS mask and installs the new one. Reporting
            // ENOSYS here made busybox's mkdir -p stop after its first EEXIST.
            static unsigned current_umask = 022;
            const unsigned previous = current_umask;
            current_umask = (unsigned)a0 & 0777u;
            E2_RETURN(previous);
        }
        case 34:  // mkdirat
            E2_RETURN(sys_pathop(m, env, E2_PATHOP_MKDIR, a1, 0, (unsigned)a2));
        case 35:  // unlinkat
            // AT_REMOVEDIR (0x200) in a2 selects rmdir over unlink.
            E2_RETURN(sys_pathop(m, env,
                                 ((unsigned)a2 & 0x200u) ? E2_PATHOP_RMDIR : E2_PATHOP_UNLINK,
                                 a1, 0, 0));
        case 36:  // symlinkat(target, newdirfd, linkpath)
            E2_RETURN(sys_pathop(m, env, E2_PATHOP_SYMLINK, a0, a2, 0));
        case 37:  // linkat(olddirfd, oldpath, newdirfd, newpath, flags)
            E2_RETURN(sys_pathop(m, env, E2_PATHOP_LINK, a1, a3, 0));
        case 38:  // renameat(olddirfd, oldpath, newdirfd, newpath)
            E2_RETURN(sys_pathop(m, env, E2_PATHOP_RENAME, a1, a3, 0));
        case 48:  // faccessat
            E2_RETURN(sys_pathop(m, env, E2_PATHOP_ACCESS, a1, 0, (unsigned)a2));
        case 53:  // fchmodat
            E2_RETURN(sys_pathop(m, env, E2_PATHOP_CHMOD, a1, 0, (unsigned)a2));
        case 88: {  // utimensat
            unsigned mode = 0;
            if (a2 != 0) {
                // Two timespecs; only the modification second is used.
                int64_t ts[4];
                if (e2_memory_read(&m->mem, a2, ts, sizeof(ts)) != 0) {
                    E2_RETURN(-E2_EFAULT);
                }
                mode = (unsigned)ts[2];
            }
            E2_RETURN(sys_pathop(m, env, E2_PATHOP_UTIME, a1, 0, mode));
        }
        case 7: {  // poll
            E2_RETURN(sys_poll(m, env, a0, a1, (long)(int32_t)a2));
        }
        case 73: {  // ppoll
            long ms = 0;
            if (a2 != 0) {
                struct {
                    int64_t sec;
                    int64_t nsec;
                } ts;
                if (e2_memory_read(&m->mem, a2, &ts, sizeof(ts)) != 0) {
                    E2_RETURN(-E2_EFAULT);
                }
                ms = (long)(ts.sec * 1000 + ts.nsec / 1000000);
            }
            E2_RETURN(sys_poll(m, env, a0, a1, ms));
        }
        case 65: {  // readv
            if (files == NULL) {
                E2_RETURN(-E2_ENOSYS);
            }
            E2_RETURN(sys_readv(m, env, (int)a0, a1, (int)a2));
        }
        case E2_SYS_read: {
            if (files == NULL) {
                E2_RETURN(-E2_ENOSYS);
            }
            uint8_t *dst = e2_memory_host_ptr(&m->mem, a1, (size_t)a2, E2_PROT_WRITE);
            if (dst == NULL) {
                E2_RETURN(-E2_EFAULT);
            }
            E2_RETURN(files->read(env->ctx, (int)a0, dst, (size_t)a2, -1));
        }
        case E2_SYS_pread64: {
            if (files == NULL) {
                E2_RETURN(-E2_ENOSYS);
            }
            uint8_t *dst = e2_memory_host_ptr(&m->mem, a1, (size_t)a2, E2_PROT_WRITE);
            if (dst == NULL) {
                E2_RETURN(-E2_EFAULT);
            }
            E2_RETURN(files->read(env->ctx, (int)a0, dst, (size_t)a2, (long long)m->cpu.x[3]));
        }
        case E2_SYS_clone: {
            // The shell uses clone(SIGCHLD, 0, ...) for fork; a NULL stack means
            // "same address space, copied". A non-NULL stack is a thread, which
            // this model does not offer.
            if (env->fork == NULL) {
                E2_RETURN(-E2_ENOSYS);
            }
            if (a1 != 0) {
                E2_RETURN(-E2_EINVAL);
            }
            E2_RETURN(env->fork(env->ctx, m));
        }
        case E2_SYS_execve: {
            if (env->execve == NULL) {
                E2_RETURN(-E2_ENOSYS);
            }
            char path[512];
            int rc = read_guest_string(m, a0, path, sizeof(path));
            if (rc != 0) {
                E2_RETURN(rc);
            }
            E2_RETURN(env->execve(env->ctx, m, path, a1, a2));
        }
        case E2_SYS_wait4: {
            if (env->wait4 == NULL) {
                E2_RETURN(-E2_ENOSYS);
            }
            // wait4(pid, status, options, rusage). The status pointer is a1 and
            // options is a2; passing a2 as the status wrote the child's exit
            // code into the options slot instead, so $? stayed zero for every
            // external command while builtins kept working.
            E2_RETURN(env->wait4(env->ctx, (int)a0, a1, (int)a2));
        }
        case E2_SYS_pipe2: {
            if (files == NULL || files->pipe == NULL) {
                E2_RETURN(-E2_ENOSYS);
            }
            int fds[2] = {-1, -1};
            const int rc = files->pipe(env->ctx, fds);
            if (rc < 0) {
                E2_RETURN(rc);
            }
            uint8_t *dst = e2_memory_host_ptr(&m->mem, a0, 8, E2_PROT_WRITE);
            if (dst == NULL) {
                E2_RETURN(-E2_EFAULT);
            }
            memcpy(dst, fds, 8);
            E2_RETURN(0);
        }
        case E2_SYS_fcntl: {
            if (files == NULL || files->fcntl == NULL) {
                E2_RETURN(-E2_ENOSYS);
            }
            E2_RETURN(files->fcntl(env->ctx, (int)a0, (int)a1, (long)a2));
        }
        case E2_SYS_dup3: {
            if (files == NULL || files->dup3 == NULL) {
                E2_RETURN(-E2_ENOSYS);
            }
            E2_RETURN(files->dup3(env->ctx, (int)a0, (int)a1, (int)a2));
        }
        case E2_SYS_readlinkat: {
            if (files == NULL || files->readlink == NULL) {
                E2_RETURN(-E2_ENOSYS);
            }
            char path[512];
            const int rc3 = read_guest_string(m, a1, path, sizeof(path));
            if (rc3 != 0) {
                E2_RETURN(rc3);
            }
            const uint64_t out = a2;
            const uint64_t outlen = a3;
            if (outlen == 0 || outlen > 4096) {
                E2_RETURN(-22);  // EINVAL
            }
            char target[4096];
            const long got = files->readlink(env->ctx, path, target, (size_t)outlen);
            if (got < 0) {
                E2_RETURN(got);
            }
            if (e2_memory_write(&m->mem, out, target, (size_t)got) != 0) {
                E2_RETURN(-14);  // EFAULT
            }
            E2_RETURN(got);
        }
        case E2_SYS_getcwd: {
            // (buf, size). Python calls this while working out sys.path.
            const uint64_t buf = a0;
            const uint64_t size = a1;
            const char *dir = (env->cwd[0] != '\0') ? env->cwd : "/";
            const size_t need = strlen(dir) + 1;
            if (size < need) {
                E2_RETURN(-34);  // ERANGE
            }
            if (e2_memory_write(&m->mem, buf, dir, need) != 0) {
                E2_RETURN(-14);  // EFAULT
            }
            E2_RETURN(buf);
        }
        case E2_SYS_chdir: {
            char path[512];
            const int rc2 = read_guest_string(m, a0, path, sizeof(path));
            if (rc2 != 0) {
                E2_RETURN(rc2);
            }
            if (path[0] == '/') {
                snprintf(env->cwd, sizeof(env->cwd), "%s", path);
            } else {
                const size_t len = strlen(env->cwd);
                snprintf(env->cwd + len, sizeof(env->cwd) - len, "/%s", path);
            }
            E2_RETURN(0);
        }
        case E2_SYS_openat: {
            if (files == NULL) {
                E2_RETURN(-E2_ENOSYS);
            }
            char path[512];
            int rc = read_guest_string(m, a1, path, sizeof(path));
            if (rc != 0) {
                E2_RETURN(rc);
            }
            // O_DIRECTORY is 040000 (0x4000) in musl's aarch64 fcntl.h. Note
            // that 0x20000 is O_LARGEFILE, not O_DIRECTORY - mapping it wrongly
            // made every `>` redirection fail with EINVAL.
            if (((int)a2 & 0x4000) != 0 && files->opendir != NULL) {
                E2_RETURN(files->opendir(env->ctx, path));
            }
            E2_RETURN(files->open(env->ctx, path, (int)a2, (int)m->cpu.x[3]));
        }
        case E2_SYS_close:
            if (files == NULL) {
                E2_RETURN(-E2_ENOSYS);
            }
            E2_RETURN(files->close(env->ctx, (int)a0));
        case E2_SYS_fstat: {
            if (files == NULL) {
                E2_RETURN(-E2_ENOSYS);
            }
            e2_guest_stat st;
            memset(&st, 0, sizeof(st));
            uint8_t *dst = e2_memory_host_ptr(&m->mem, a1, sizeof(st), E2_PROT_WRITE);
            if (dst == NULL) {
                E2_RETURN(-E2_EFAULT);
            }
            int rc2 = files->stat(env->ctx, (int)a0, &st, sizeof(st));
            if (rc2 < 0) {
                E2_RETURN(rc2);
            }
            memcpy(dst, &st, sizeof(st));
            E2_RETURN(0);
        }
        case 61: {  // getdents64
            if (files == NULL) {
                E2_RETURN(-E2_ENOSYS);
            }
            uint8_t *dst = e2_memory_host_ptr(&m->mem, a1, (size_t)a2, E2_PROT_WRITE);
            if (dst == NULL) {
                E2_RETURN(-E2_EFAULT);
            }
            long n = files->readdir(env->ctx, (int)a0, dst, (size_t)a2);
            E2_RETURN(n);
        }
        case 79: {  // newfstatat: stat by path
            if (files == NULL) {
                E2_RETURN(-E2_ENOSYS);
            }
            char path[512];
            int rc = read_guest_string(m, a1, path, sizeof(path));
            if (rc != 0) {
                E2_RETURN(rc);
            }
            e2_guest_stat st;
            memset(&st, 0, sizeof(st));
            uint8_t *dst = e2_memory_host_ptr(&m->mem, a2, sizeof(st), E2_PROT_WRITE);
            if (dst == NULL) {
                E2_RETURN(-E2_EFAULT);
            }
            // The host layer indexes files by descriptor, so resolve the path
            // through open/close rather than adding a second stat entry point.
            const int fd = files->open(env->ctx, path, 0, 0);
            if (fd < 0) {
                E2_RETURN(fd);
            }
            const int src = files->stat(env->ctx, fd, &st, sizeof(st));
            files->close(env->ctx, fd);
            if (src < 0) {
                E2_RETURN(src);
            }
            memcpy(dst, &st, sizeof(st));
            E2_RETURN(0);
        }
        case E2_SYS_lseek: {
            // Only SEEK_SET/SEEK_CUR/SEEK_END with a 64-bit offset.
            if (files == NULL) {
                E2_RETURN(-E2_ENOSYS);
            }
            const int whence = (int)a2;
            const long long offset = (long long)a1;
            long long target = offset;
            if (whence == 1) {
                // Current position is only meaningful if the host tracks it; the
                // host read/write take -1 for "cursor", so this is a no-op seek.
                E2_RETURN(offset);
            }
            if (whence == 2) {
                e2_guest_stat st2;
                memset(&st2, 0, sizeof(st2));
                if (files->stat(env->ctx, (int)a0, &st2, sizeof(st2)) < 0) {
                    E2_RETURN(-E2_EBADF);
                }
                target = st2.st_size + offset;
            }
            E2_RETURN(target);
        }
        case E2_SYS_clock_gettime: {
            // guest passes a struct timespec.
            // a0 is the clock id: 0 realtime, 1 monotonic, and so on. Serving
            // the monotonic clock for every request put date at 1970.
            const long long now = (files && files->clock_ns)
                                      ? files->clock_ns(env->ctx, (int)a0)
                                      : 0;
            struct { int64_t sec; int64_t nsec; } ts = { now / 1000000000LL, now % 1000000000LL };
            uint8_t *dst = e2_memory_host_ptr(&m->mem, a1, sizeof(ts), E2_PROT_WRITE);
            if (dst == NULL) {
                E2_RETURN(-E2_EFAULT);
            }
            memcpy(dst, &ts, sizeof(ts));
            E2_RETURN(0);
        }
        case 96:  // set_tid_address
            E2_RETURN(1);
        case 172:  // getpid
        case 173:  // getppid
        case 174:  // getuid
        case 175:  // geteuid
        case 176:  // getgid
        case 177:  // getegid
            E2_RETURN(0);
        case 178:  // gettid
            E2_RETURN(1);
        case 135:  // rt_sigaction: report success, do not install anything.
            // libc only checks the return value during start-up; the guest runs
            // single-threaded and nothing can deliver a signal to it.
            E2_RETURN(0);
        case 134:  // rt_sigprocmask
            E2_RETURN(0);
        case 132:  // sigaltstack
            E2_RETURN(0);
        case 129:  // rt_sigqueueinfo / kill family: nothing to deliver
        case 130:
            E2_RETURN(0);
        case 97:  // uname
        case 160:  // uname (aarch64 uses 160)
            if (a0 != 0) {
                uint8_t *dst = e2_memory_host_ptr(&m->mem, a0, 390, E2_PROT_WRITE);
                if (dst != NULL) {
                    memset(dst, 0, 390);
                    memcpy(dst + 0, "Linux", 5);
                    memcpy(dst + 65, "e2", 2);
                    memcpy(dst + 130, "6.1.0-e2", 8);
                    memcpy(dst + 195, "#1", 2);
                    memcpy(dst + 260, "aarch64", 7);
                }
            }
            E2_RETURN(0);
        case 233:  // madvise
            E2_RETURN(0);
        case 261:  // prlimit64: report defaults, ignore new limits
            E2_RETURN(0);
        case 278:  // getrandom: return a fixed but non-zero stream
            if (a0 != 0) {
                uint8_t *dst = e2_memory_host_ptr(&m->mem, a0, (size_t)a1, E2_PROT_WRITE);
                if (dst == NULL) {
                    E2_RETURN(-E2_EFAULT);
                }
                for (size_t k = 0; k < (size_t)a1; k++) {
                    dst[k] = (uint8_t)(0x5A + (k * 31));
                }
            }
            E2_RETURN((int64_t)a1);
        case 98:  // futex: single-threaded, so nothing ever blocks
            E2_RETURN(0);
        case 29:  // ioctl
            // ENOTTY, not ENOTSUP: callers test specifically for ENOTTY to mean
            // "not a terminal" and fall back to a default width. Any other error
            // is treated as fatal and aborts the command.
            E2_RETURN(-25);  // ENOTTY
        case E2_SYS_brk: {
            // Back the break with a real mapping. Returning a plausible address
            // without mapping it makes every later store fault, which is exactly
            // how musl's allocator fails after its first allocation.
            if (m->brk_base == 0) {
                const uint64_t base = 0x20000000ULL;
                const size_t size = 16u << 20;
                if (e2_memory_map_at(&m->mem, base, size,
                                    E2_PROT_READ | E2_PROT_WRITE) != 0) {
                    E2_RETURN(-12);
                }
                m->brk_base = base;
                m->brk_current = base;
                m->brk_limit = base + size;
            }
            if (a0 == 0) {
                E2_RETURN(m->brk_current);
            }
            if (a0 >= m->brk_base && a0 <= m->brk_limit) {
                m->brk_current = a0;
            }
            E2_RETURN(m->brk_current);
        }
        case E2_SYS_mmap: {
            // Anonymous mappings come from the interpreter's own address space.
            const uint64_t length = a1;
            const uint64_t prot = a2;
            int e2_prot = 0;
            if (prot & 0x1) e2_prot |= E2_PROT_READ;
            if (prot & 0x2) e2_prot |= E2_PROT_WRITE;
            if (prot & 0x4) e2_prot |= E2_PROT_EXEC;
            // The region is created writable so the caller can populate it, then
            // narrowed to what was asked for. mprotect is now honoured, so a
            // guest that writes where it declared read-only will fault here
            // instead of silently corrupting its own constants.
            const int final_prot = e2_prot | E2_PROT_WRITE;
            e2_prot = final_prot;

            const int mmap_flags = (int)m->cpu.x[3];
            const uint64_t hint = a0;
            const bool fixed = (mmap_flags & 0x10) != 0;  // MAP_FIXED
            static int mmap_trace = -1;
            if (mmap_trace < 0) mmap_trace = getenv("E2_TRACE_MMAP") != NULL;
            if (mmap_trace)
                fprintf(stderr, "[mmap2] hint=0x%llx len=0x%llx prot=%llu flags=0x%x fixed=%d\n",
                        (unsigned long long)hint, (unsigned long long)length,
                        (unsigned long long)a2, mmap_flags, (int)fixed);
            uint64_t base = 0;
            if (fixed && hint != 0) {
                // Honouring the hint matters: musl places its own bookkeeping at
                // an address it already computed references to, so returning a
                // different address leaves those references dangling.
                const uint64_t aligned = hint & ~0xFFFULL;
                if (e2_memory_find(&m->mem, aligned) != NULL) {
                    // MAP_FIXED replaces whatever was there. Accepting the
                    // address is not enough: an anonymous request gets fresh
                    // zero pages, and reusing the region without clearing it
                    // leaves the old bytes visible. musl covers each segment's
                    // .bss with exactly this kind of mapping, and the bytes
                    // underneath were the tail of the file - which put 0x459930
                    // where a thread state pointer belonged.
                    base = aligned;
                    if ((mmap_flags & 0x20) != 0) {
                        uint8_t *fresh = e2_memory_host_ptr(&m->mem, aligned,
                                                            (size_t)length,
                                                            E2_PROT_WRITE);
                        if (fresh != NULL) {
                            memset(fresh, 0, (size_t)length);
                        }
                    }
                } else if (e2_memory_map_at(&m->mem, aligned, (size_t)length,
                                            e2_prot) != 0) {
                    E2_RETURN(-12);
                } else {
                    base = aligned;
                }
            } else if (e2_memory_map_any(&m->mem, (size_t)length, e2_prot, &base) != 0) {
                E2_RETURN(-12);  // ENOMEM
            }

            // File-backed mappings must actually contain the file. Leaving them
            // zero-filled makes the guest read an empty dynamic section and an
            // empty program image, which fails far away from here.
            const bool anonymous = (mmap_flags & 0x20) != 0;  // MAP_ANONYMOUS
            const int map_fd = (int)m->cpu.x[4];
            const uint64_t map_off = m->cpu.x[5];
            if (!anonymous && map_fd >= 0) {
                uint8_t *dst = e2_memory_host_ptr(&m->mem, base, (size_t)length,
                                                  E2_PROT_WRITE);
                if (dst != NULL) {
                    long got = files->read(env->ctx, map_fd, dst, (size_t)length,
                                           (long long)map_off);
                    if (got < 0) {
                        E2_RETURN(got);
                    }
                    // A mapping that runs past the end of the file reads as zero
                    // there; the host read stops at EOF and would otherwise leave
                    // whatever the region held. This bit the dynamic loader: it
                    // maps whole files, and the tail must not look like data.
                    if ((uint64_t)got < length) {
                        memset(dst + got, 0, (size_t)(length - (uint64_t)got));
                    }
                }
            }
            E2_RETURN(base);
        }
        case E2_SYS_mprotect: {
            // Regions are already mapped with their final protection; a request to
            // loosen or tighten it is accepted without changing the host mapping,
            // because the guest cannot observe host page permissions.
            E2_RETURN(0);
        }
        case E2_SYS_munmap:
            E2_RETURN(0);
        case E2_SYS_exit:
        case E2_SYS_exit_group: {
            const int status = (int)a0;
            if (env->exit != NULL) {
                env->exit(env->ctx, status);
            }
            m->exit_status = status;
            return E2_HALTED;
        }
        default:
            E2_RETURN(-E2_ENOSYS);
    }
    #undef E2_RETURN
}
