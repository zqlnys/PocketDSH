#include "RuntimeExecProbe.h"

#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <libkern/OSCacheControl.h>
#endif

/// aarch64: `mov w0, #42 ; ret`
///
/// 0x52800540  mov w0, #42
/// 0xD65F03C0  ret
///
/// Little endian, so the file contains the bytes below in this order.
static const uint8_t kReturnFortyTwo[] = {
    0x40, 0x05, 0x80, 0x52,
    0xC0, 0x03, 0x5F, 0xD6,
};

static sigjmp_buf g_jump;
static volatile sig_atomic_t g_caught_signal;

/// Execution of loaded code may fault on any of these. The handler records the
/// signal and returns control to the probe instead of taking the process down.
static void probe_signal_handler(int sig) {
    g_caught_signal = sig;
    siglongjmp(g_jump, 1);
}

static int install_signal_guards(struct sigaction old_actions[3]) {
    const int signals[3] = { SIGSEGV, SIGBUS, SIGILL };
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = probe_signal_handler;
    sigemptyset(&action.sa_mask);
    // SA_NODEFER so a secondary fault inside the handler cannot deadlock.
    action.sa_flags = SA_NODEFER;

    for (int i = 0; i < 3; i++) {
        if (sigaction(signals[i], &action, &old_actions[i]) != 0) {
            // Restore whatever was already installed.
            for (int j = 0; j < i; j++) {
                sigaction(signals[j], &old_actions[j], NULL);
            }
            return -errno;
        }
    }
    return 0;
}

static void restore_signal_guards(const struct sigaction old_actions[3]) {
    const int signals[3] = { SIGSEGV, SIGBUS, SIGILL };
    for (int i = 0; i < 3; i++) {
        sigaction(signals[i], &old_actions[i], NULL);
    }
}

static int probe_anonymous_exec(pocket_exec_probe_result *out) {
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) {
        out->anonymous_exec_errno = EINVAL;
        return -1;
    }

    void *mapping = mmap(NULL, (size_t)page,
                         PROT_READ | PROT_WRITE | PROT_EXEC,
                         MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mapping == MAP_FAILED) {
        out->anonymous_exec_errno = errno;
        return 0;
    }

    out->anonymous_exec_errno = 0;
    munmap(mapping, (size_t)page);
    return 0;
}

/// Writes the machine code next to the temporary directory and maps it
/// read-only. Returns the mapping, or MAP_FAILED with *stage set.
static void *map_code_file(long page, char *path_out, size_t path_size,
                           int *map_errno, int *protect_errno) {
    const char *tmp = getenv("TMPDIR");
    if (tmp == NULL || tmp[0] == '\0') {
        tmp = "/tmp";
    }
    snprintf(path_out, path_size, "%s/pocketdsh-exec-probe.bin", tmp);

    int fd = open(path_out, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (fd < 0) {
        *map_errno = errno;
        return MAP_FAILED;
    }
    ssize_t written = write(fd, kReturnFortyTwo, sizeof(kReturnFortyTwo));
    close(fd);
    if (written != (ssize_t)sizeof(kReturnFortyTwo)) {
        *map_errno = EIO;
        return MAP_FAILED;
    }

    fd = open(path_out, O_RDONLY);
    if (fd < 0) {
        *map_errno = errno;
        return MAP_FAILED;
    }

    // Map read-only first: an executable request at mmap time is a different
    // code path from promoting an existing mapping.
    void *mapping = mmap(NULL, (size_t)page, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (mapping == MAP_FAILED) {
        *map_errno = errno;
        return MAP_FAILED;
    }
    *map_errno = 0;

    if (mprotect(mapping, (size_t)page, PROT_READ | PROT_EXEC) != 0) {
        *protect_errno = errno;
        return mapping;
    }
    *protect_errno = 0;

    // Instruction cache must be flushed before executing freshly written code.
#if defined(__APPLE__)
    sys_icache_invalidate(mapping, (size_t)page);
#else
    __builtin___clear_cache((char *)mapping, (char *)mapping + page);
#endif
    return mapping;
}

int pocket_exec_probe(pocket_exec_probe_result *out) {
    if (out == NULL) {
        return -EINVAL;
    }
    memset(out, 0, sizeof(*out));
    out->execute_status = -1;

    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) {
        snprintf(out->detail, sizeof(out->detail), "page size unavailable");
        return -EINVAL;
    }

    probe_anonymous_exec(out);

    char path[1024];
    int map_errno = 0;
    int protect_errno = 0;
    void *mapping = map_code_file(page, path, sizeof(path), &map_errno, &protect_errno);
    out->file_map_errno = map_errno;
    out->file_protect_errno = protect_errno;

    if (mapping == MAP_FAILED || protect_errno != 0) {
        if (mapping != MAP_FAILED) {
            munmap(mapping, (size_t)page);
        }
        unlink(path);
        snprintf(out->detail, sizeof(out->detail),
                 "anon_exec=%d file_map=%d file_protect=%d execution=skipped",
                 out->anonymous_exec_errno, out->file_map_errno,
                 out->file_protect_errno);
        return 0;
    }

    struct sigaction old_actions[3];
    if (install_signal_guards(old_actions) != 0) {
        munmap(mapping, (size_t)page);
        unlink(path);
        snprintf(out->detail, sizeof(out->detail),
                 "ran with anon_exec=%d file_map=%d file_protect=%d, but signal guards could not be installed",
                 out->anonymous_exec_errno, out->file_map_errno,
                 out->file_protect_errno);
        return 0;
    }

    g_caught_signal = 0;
    if (sigsetjmp(g_jump, 1) == 0) {
        int (*code)(void) = (int (*)(void))mapping;
        int value = code();
        out->execute_status = 0;
        out->execute_value = value;
    } else {
        out->execute_status = (int)g_caught_signal;
    }

    restore_signal_guards(old_actions);
    munmap(mapping, (size_t)page);
    unlink(path);

    if (out->execute_status == 0) {
        snprintf(out->detail, sizeof(out->detail),
                 "anon_exec=%d file_map=%d file_protect=%d executed=ok value=%d",
                 out->anonymous_exec_errno, out->file_map_errno,
                 out->file_protect_errno, out->execute_value);
    } else {
        snprintf(out->detail, sizeof(out->detail),
                 "anon_exec=%d file_map=%d file_protect=%d executed=signal:%d",
                 out->anonymous_exec_errno, out->file_map_errno,
                 out->file_protect_errno, out->execute_status);
    }
    return 0;
}
