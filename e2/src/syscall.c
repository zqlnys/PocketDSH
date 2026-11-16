#include "e2/syscall.h"

#include <string.h>

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

e2_status e2_service_syscall(e2_machine *m, e2_syscall_env *env) {
    const uint64_t number = m->cpu.x[8];
    const uint64_t a0 = m->cpu.x[0];
    const uint64_t a1 = m->cpu.x[1];
    const uint64_t a2 = m->cpu.x[2];
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
        case E2_SYS_openat: {
            if (files == NULL) {
                E2_RETURN(-E2_ENOSYS);
            }
            char path[512];
            int rc = read_guest_string(m, a1, path, sizeof(path));
            if (rc != 0) {
                E2_RETURN(rc);
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
            const long long now = (files && files->clock_ns) ? files->clock_ns(env->ctx) : 0;
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
        case 29:  // ioctl: report "not a tty"
            E2_RETURN(-E2_ENOTSUP);
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
            // Always grant write. The guest's mprotect is advisory here, and the
            // loader must be able to populate text through a mapping it asked for
            // as R+X; on a real kernel that goes through a writable private
            // mapping followed by mprotect.
            e2_prot |= E2_PROT_WRITE;

            const int mmap_flags = (int)m->cpu.x[3];
            const uint64_t hint = a0;
            const bool fixed = (mmap_flags & 0x10) != 0;  // MAP_FIXED
            uint64_t base = 0;
            if (fixed && hint != 0) {
                // Honouring the hint matters: musl places its own bookkeeping at
                // an address it already computed references to, so returning a
                // different address leaves those references dangling.
                const uint64_t aligned = hint & ~0xFFFULL;
                if (e2_memory_find(&m->mem, aligned) != NULL) {
                    // MAP_FIXED replaces whatever was there. The address is
                    // already backed by a region of ours, which is what the guest
                    // asked for, so accept it rather than failing with ENOMEM.
                    base = aligned;
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
