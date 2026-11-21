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
#define E2_ENAMETOOLONG 36
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
    // The buffer holds a path in every caller that matters, and the kernel answers a path
    // that does not fit with ENAMETOOLONG rather than EINVAL. The difference is visible:
    // `stat` on a 1100-character path printed "Invalid argument" here and "Filename too
    // long" from qemu.
    return -E2_ENAMETOOLONG;  // no terminator within the limit
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
extern int g_dbg_pid;
unsigned long long g_dbg_last_syscall;

// Guest paths. 512 was enough for every case tested and would still silently
// truncate a longer one into a different path, which is the same failure the
// argument and environment buffers had.
#define E2_PATH_MAX 1024

// getenv walks environ and compares strings, and these two switches are consulted on every
// single syscall. Profiling put getenv at 84% of the run: the tracing that exists to debug
// was costing more than the work being debugged. Resolved once, on first use.
static int trace_path_on(void) {
    static int cached = -1;
    if (cached < 0) cached = getenv("E2_TRACE_PATH") != NULL;
    return cached;
}

static int trace_sys_on(void) {
    static int cached = -1;
    if (cached < 0) cached = getenv("E2_TRACE_SYS") != NULL;
    return cached;
}

e2_status e2_service_syscall(e2_machine *m, e2_syscall_env *env) {
    // One line per syscall, opt-in. Comparing this against qemu's -strace is the
    // fastest way to see which operation a program takes and where the two
    // implementations diverge.
    if (trace_path_on() && m->cpu.x[8] == 56) {
        char pbuf[256];
        if (read_guest_string(m, m->cpu.x[1], pbuf, sizeof(pbuf)) == 0) {
            fprintf(stderr, "[open] %s\n", pbuf);
        }
    }
    if (trace_sys_on()) {
        fprintf(stderr, "[sys@%llu p%d pc=0x%llx] %llu(0x%llx, 0x%llx, 0x%llx, 0x%llx, 0x%llx, 0x%llx)\n",
                g_global_step, g_dbg_pid, (unsigned long long)m->cpu.pc,
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
    // Report a failing syscall at the point the result is produced. Probing x0 after
    // every step instead reported AT_FDCWD (-100) as a failed return, because a
    // negative register value is also a legitimate argument.
    #define E2_RETURN(value) do {                                                 \
        const int64_t e2_rv = (int64_t)(value);                                   \
        if (e2_rv < 0 && getenv("E2_TRACE_EFAIL") != NULL)                        \
            fprintf(stderr, "[efail] sys=%llu ret=%lld\n",                        \
                    (unsigned long long)number, (long long)e2_rv);                \
        m->cpu.x[0] = (uint64_t)e2_rv; return E2_OK; } while (0)

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
        case 45:      // truncate(path, length). No descriptor is involved, so the
                      // host resolves the path itself.
        case 46: {    // ftruncate(fd, length)
            if (files == NULL || files->truncate_file == NULL) {
                E2_RETURN(-E2_ENOSYS);
            }
            if (number == 46) {
                E2_RETURN(files->truncate_file(env->ctx, (int)a0, NULL, (long long)a1));
            }
            char path[E2_PATH_MAX];
            const int rc = read_guest_string(m, a0, path, sizeof(path));
            if (rc != 0) {
                E2_RETURN(rc);
            }
            E2_RETURN(files->truncate_file(env->ctx, -1, path, (long long)a1));
        }
        case 54:      // fchownat(dirfd, path, uid, gid, flags)
        case 55: {    // fchown(fd, uid, gid)
            if (files == NULL || files->pathop == NULL) {
                E2_RETURN(-E2_ENOSYS);
            }
            // The path form takes the path in x1; the descriptor form has no path.
            const char *path = (number == 54) ? NULL : NULL;
            char buf[E2_PATH_MAX];
            if (number == 54) {
                const int rc = read_guest_string(m, a1, buf, sizeof(buf));
                if (rc != 0) {
                    E2_RETURN(rc);
                }
                path = buf;
            }
            E2_RETURN(files->pathop(env->ctx, E2_PATHOP_CHOWN, path, NULL,
                                    (unsigned)(m->cpu.x[3] & 0xFFFFFFFFu)));
        }
        case 43:      // statfs(path, buf)
        case 44: {    // fstatfs(fd, buf)
            if (files == NULL || files->statfs_of == NULL) {
                E2_RETURN(-E2_ENOSYS);
            }
            // The aarch64 layout is 128 bytes: eleven 8-byte words, an 8-byte
            // fsid and four spare words.
            uint8_t *dst = e2_memory_host_ptr(&m->mem, a1, 128, E2_PROT_WRITE);
            if (dst == NULL) {
                E2_RETURN(-E2_EFAULT);
            }
            if (number == 43) {
                char path[E2_PATH_MAX];
                const int rc = read_guest_string(m, a0, path, sizeof(path));
                if (rc != 0) {
                    E2_RETURN(rc);
                }
                E2_RETURN(files->statfs_of(env->ctx, -1, path, dst, 128));
            }
            E2_RETURN(files->statfs_of(env->ctx, (int)a0, NULL, dst, 128));
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
            char path[E2_PATH_MAX];
            int rc = read_guest_string(m, a0, path, sizeof(path));
            if (rc != 0) {
                E2_RETURN(rc);
            }
            E2_RETURN(env->execve(env->ctx, m, path, a1, a2));
        }
        case 179: {  // sysinfo: total and free memory, uptime, load averages
            // Absent until now, and its absence was not loud: busybox `free` calls this
            // first and, on failure, prints a struct it never initialised - a row of
            // thirty-digit numbers where the memory figures belong. The values below
            // match the synthetic /proc/meminfo so both sources agree.
            uint8_t *dst = e2_memory_host_ptr(&m->mem, a0, 112, E2_PROT_WRITE);
            if (dst == NULL) {
                E2_RETURN(-E2_EFAULT);
            }
            memset(dst, 0, 112);
            const int64_t uptime = 12345;
            memcpy(dst + 0, &uptime, 8);                 // uptime
            const uint64_t loads[3] = {0, 0, 0};
            memcpy(dst + 8, loads, sizeof(loads));       // loads[3]
            const uint64_t totalram = 1024000ULL * 1024; // bytes, matching meminfo
            const uint64_t freeram = 512000ULL * 1024;
            const uint64_t sharedram = 0;
            const uint64_t bufferram = 10000ULL * 1024;
            const uint64_t totalswap = 0;
            const uint64_t freeswap = 0;
            memcpy(dst + 32, &totalram, 8);
            memcpy(dst + 40, &freeram, 8);
            memcpy(dst + 48, &sharedram, 8);
            memcpy(dst + 56, &bufferram, 8);
            memcpy(dst + 64, &totalswap, 8);
            memcpy(dst + 72, &freeswap, 8);
            const uint16_t procs = 1;
            memcpy(dst + 80, &procs, 2);                 // procs
            const uint64_t totalhigh = 0, freehigh = 0;
            memcpy(dst + 88, &totalhigh, 8);
            memcpy(dst + 96, &freehigh, 8);
            const uint32_t mem_unit = 1;
            memcpy(dst + 104, &mem_unit, 4);
            E2_RETURN(0);
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
            if (getenv("E2_TRACE_PIPE"))
                fprintf(stderr, "[pipe] p%d r=%d w=%d\n", g_dbg_pid, fds[0], fds[1]);
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
            char path[E2_PATH_MAX];
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
            char path[E2_PATH_MAX];
            const int rc2 = read_guest_string(m, a0, path, sizeof(path));
            if (rc2 != 0) {
                E2_RETURN(rc2);
            }
            // env->cwd is its own fixed buffer; copy at most what fits rather than
            // letting the compiler infer a truncation.
            // Check the directory really exists before reporting success: a host
            // that accepts any string makes `cd` lie.
            if (files != NULL && files->opendir != NULL) {
                const int probe = files->opendir(env->ctx, path);
                if (probe < 0) {
                    E2_RETURN(probe);
                }
                files->close(env->ctx, probe);
            }
            // The flag has to be set on both branches: the first version of this only set it
            // for relative paths, so `cd /tmp` changed cwd without telling the caller and the
            // next mkdir landed in the wrong directory.
            env->cwd_changed = 1;
            if (path[0] == '/') {
                snprintf(env->cwd, sizeof(env->cwd), "%.*s",
                         (int)(sizeof(env->cwd) - 1), path);
            } else {
                const size_t len = strlen(env->cwd);
                if (len + 1 < sizeof(env->cwd)) {
                    snprintf(env->cwd + len, sizeof(env->cwd) - len, "/%.*s",
                             (int)(sizeof(env->cwd) - len - 2), path);
                }
            }
            E2_RETURN(0);
        }
        case E2_SYS_openat: {
            if (files == NULL) {
                E2_RETURN(-E2_ENOSYS);
            }
            char path[E2_PATH_MAX];
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
            char path[E2_PATH_MAX];
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
            // AT_SYMLINK_NOFOLLOW (0x100 in flags, which is x3) has to be honoured:
            // rm asks for lstat before unlinking a name, and a dangling symlink has no
            // stat result at all - resolving it first makes the link undeletable.
            const int follow = (int)((m->cpu.x[3] & 0x100u) == 0);
            if (files->stat_path != NULL) {
                const int src = files->stat_path(env->ctx, path, &st, sizeof(st), follow);
                if (src < 0) {
                    E2_RETURN(src);
                }
            } else {
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
            }
            memcpy(dst, &st, sizeof(st));
            E2_RETURN(0);
        }
        case E2_SYS_lseek: {
            if (files == NULL || files->seek == NULL) {
                E2_RETURN(-E2_ENOSYS);
            }
            // The host owns the file position, so the seek has to reach it. This
            // used to compute the new offset and return it without moving anything,
            // which made every re-read see EOF.
            E2_RETURN(files->seek(env->ctx, (int)a0, (long long)a1, (int)a2));
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
            E2_RETURN(env->pid);
        case 173:  // getppid
            E2_RETURN(env->ppid);
        case 174:  // getuid
        case 175:  // geteuid
        case 176:  // getgid
        case 177:  // getegid
            E2_RETURN(0);
        case 178:  // gettid
            E2_RETURN(1);
        case 134: {  // rt_sigaction(sig, act, oldact, sigsetsize)
            // Earlier this returned 0 and installed nothing, so a program that registered
            // a handler believed it had one. Record the address; delivery happens in
            // e2_deliver_pending_signal. sigaction is {handler, flags, restorer, mask} and
            // only the handler is needed here.
            const unsigned sig = (unsigned)a0;
            if (sig == 0 || sig >= 64) {
                E2_RETURN(-E2_EINVAL);
            }
            if (a2 != 0) {
                // Asking for the previous handler: report what was recorded.
                uint8_t *old_act = e2_memory_host_ptr(&m->mem, a2, 32, E2_PROT_WRITE);
                if (old_act == NULL) {
                    E2_RETURN(-E2_EFAULT);
                }
                memset(old_act, 0, 32);
                memcpy(old_act, &m->sig_handler[sig], 8);
            }
            if (a1 != 0) {
                uint8_t act[32];
                if (e2_memory_read(&m->mem, a1, act, sizeof(act)) != 0) {
                    E2_RETURN(-E2_EFAULT);
                }
                uint64_t handler = 0;
                memcpy(&handler, act, 8);
                m->sig_handler[sig] = handler;
            }
            E2_RETURN(0);
        }
        case 139: {  // rt_sigreturn: restore the context saved when the signal was taken
            e2_sigframe frame;
            if (e2_machine_pop_sigframe(m, &frame) != 0) {
                E2_RETURN(-E2_EFAULT);
            }
            E2_RETURN(0);
        }
        case 135:  // rt_sigprocmask
            // The mask is not modelled: delivery happens between instructions and the
            // guest is single-threaded, so there is no window a mask would protect.
            E2_RETURN(0);
        case 132:  // sigaltstack
            E2_RETURN(0);
        case 129:   // kill(pid, sig)
        case 130: { // rt_sigqueueinfo / tgkill
            // Only the "signal this process" case is modelled, which is what `kill -INT $$`
            // and `kill -USR1 $$` do. A signal aimed at another process is recorded but not
            // delivered, because the target may not be the one running.
            const unsigned sig = (unsigned)a1;   // kill: sig is arg1; tgkill: also arg1
            if (sig == 0) {
                // Signal 0 is the existence check: it must not deliver anything, and
                // rejecting it broke `kill -0 $$`, which is how a script asks whether a
                // process is still there.
                E2_RETURN(0);
            }
            if (sig >= 64) {
                E2_RETURN(-E2_EINVAL);
            }
            const long target = (long)a0;
            const bool self = (target == 0 || target == (long)env->pid);
            if (self || number == 130) {
                m->sig_pending |= (1ULL << sig);
            }
            E2_RETURN(0);
        }
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
        case 261: {  // prlimit64(pid, resource, new, old)
            // Returning 0 without touching `old` left the caller reading whatever
            // was in its buffer - `ulimit -n` printed 0. The limits themselves are
            // not enforced here, but they have to be reported.
            const uint64_t resource = a1;
            const uint64_t old_ptr = a3;
            (void)a2;   // new limits are accepted and ignored
            if (old_ptr != 0) {
                uint64_t *dst = (uint64_t *)e2_memory_host_ptr(&m->mem, old_ptr, 16,
                                                               E2_PROT_WRITE);
                if (dst == NULL) {
                    E2_RETURN(-E2_EFAULT);
                }
                uint64_t cur = 0, max = 0;
                switch (resource) {
                case 3:  cur = 8u * 1024u * 1024u; max = (uint64_t)-1; break;  // STACK
                case 4:  cur = 0;  max = 0; break;                             // CORE
                case 5:  cur = 0;  max = (uint64_t)-1; break;                  // RSS
                // The harness descriptor table holds 256 entries, so that is the
                // honest number. Reporting the host's 1048576 would let a caller
                // size buffers for descriptors that cannot exist. 64 was too low:
                // a program opening a few dozen files ran out.
                case 7:  cur = 256; max = 256; break;                          // NOFILE
                case 6:  cur = 32; max = 32; break;                           // NPROC
                case 12: cur = 8192; max = 8192; break;                        // NICE
                default: cur = (uint64_t)-1; max = (uint64_t)-1; break;         // unlimited
                }
                dst[0] = cur;
                dst[1] = max;
            }
            E2_RETURN(0);
        }
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
            // The heap grows in chunks instead of being reserved whole. A single 256 MB
            // calloc is fine on a desktop and is not on a phone, where the system can refuse
            // it outright - and when it does, CPython reports MemoryError partway through
            // importing its standard library rather than failing at the start.
            //
            // E2_HEAP_CHUNK_MB sets the chunk size; E2_HEAP_MB caps the total, so a program
            // that never asks for much never costs much.
            if (m->brk_base == 0) {
                static long chunk_bytes = -1;
                static uint64_t cap_bytes = 0;
                if (chunk_bytes < 0) {
                    const char *c = getenv("E2_HEAP_CHUNK_MB");
                    const char *t = getenv("E2_HEAP_MB");
                    const long chunk_mb = c ? strtol(c, NULL, 0) : 0;
                    const long cap_mb = t ? strtol(t, NULL, 0) : 0;
                    chunk_bytes = (chunk_mb > 0 ? chunk_mb : 16) << 20;
                    cap_bytes = (uint64_t)(cap_mb > 0 ? cap_mb : 0) << 20;
                }
                const uint64_t base = 0x20000000ULL;
                if (e2_memory_map_at(&m->mem, base, (size_t)chunk_bytes,
                                    E2_PROT_READ | E2_PROT_WRITE) != 0) {
                    E2_RETURN(-12);
                }
                m->brk_base = base;
                m->brk_current = base;
                m->brk_limit = base + (uint64_t)chunk_bytes;
                (void)cap_bytes;
            }
            if (a0 == 0) {
                E2_RETURN(m->brk_current);
            }
            if (a0 > m->brk_limit) {
                // Extend by whole chunks until the request fits. Growth is contiguous because
                // the guest expects a single range between brk_base and brk.
                uint64_t want = a0;
                while (m->brk_limit < want) {
                    const uint64_t more = 16ULL << 20;
                    if (e2_memory_map_at(&m->mem, m->brk_limit, (size_t)more,
                                        E2_PROT_READ | E2_PROT_WRITE) != 0) {
                        E2_RETURN(-12);
                    }
                    m->brk_limit += more;
                }
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
            if (getenv("E2_TRACE_MMAPRET") != NULL) {
                fprintf(stderr, "[mmapret] len=0x%llx flags=0x%x fd=%d off=0x%llx -> 0x%llx\n",
                        (unsigned long long)length, mmap_flags, map_fd,
                        (unsigned long long)map_off, (unsigned long long)base);
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
