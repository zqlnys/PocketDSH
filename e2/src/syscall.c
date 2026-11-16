#include "e2/syscall.h"

#include "e2/memory.h"

// Linux errno values used by the syscall layer. asm-generic numbering.
#define E2_EBADF 9
#define E2_EFAULT 14
#define E2_ENOSYS 38

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
        const uint8_t *src = e2_memory_host_ptr(&m->mem, iov.base, (size_t)iov.len, E2_PROT_READ);
        if (src == NULL) {
            return -E2_EFAULT;
        }
        long written = env->write(env->ctx, fd, src, (size_t)iov.len);
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

    switch (number) {
        case E2_SYS_write: {
            const uint8_t *src = e2_memory_host_ptr(&m->mem, a1, (size_t)a2, E2_PROT_READ);
            if (src == NULL) {
                m->cpu.x[0] = (uint64_t)(int64_t)(-E2_EFAULT);
                return E2_OK;
            }
            long written = env->write(env->ctx, (int)a0, src, (size_t)a2);
            m->cpu.x[0] = (uint64_t)(int64_t)written;
            return E2_OK;
        }
        case 66:  // writev
            m->cpu.x[0] = (uint64_t)(int64_t)sys_writev(m, env, (int)a0, a1, (int)a2);
            return E2_OK;
        case E2_SYS_exit:
        case E2_SYS_exit_group:
            m->exit_status = (int)(a0 & 0xFF);
            if (env->exit != NULL) {
                env->exit(env->ctx, m->exit_status);
            }
            return E2_HALTED;
        default:
            m->cpu.x[0] = (uint64_t)(int64_t)(-E2_ENOSYS);
            m->ctx.reason = "unimplemented syscall";
            return E2_OK;
    }
}
