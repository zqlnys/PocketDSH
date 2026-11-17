// rune - host harness for the e2 backend.
//
// Loads a statically linked aarch64 program, binds its guest filesystem to a
// root directory, and services syscalls against the host. This is the tool the
// backend is developed against: instruction gaps and syscall bugs surface here
// long before an iOS build is involved.
//
//   rune BINARY [args...]           run one program
//   rune busybox sh -c 'SCRIPT'     exercise the shell
//
// Environment:
//   E2_TRACE_ALL=1    print every syscall with its result
//   E2_TRACE_RET=1    print only the syscalls that returned an error
//   E2_TRACE_WAIT=1   print process starts, forks and reaps
//   E2_TRACE_PIPE=1   print pipe/fork/exec/wait traffic
//   E2_TRACE_EXEC=1   print the argv block an execve sees
//   E2_TRACE_GREP=1   print the record table grep scans
//
// The process model is cooperative: one machine per process, and wait4 runs the
// child to completion. Descriptor tables are per-process, fork shares open file
// descriptions through a reference count, and execve honours FD_CLOEXEC - all
// three are needed for shell pipelines to see EOF correctly.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <ctype.h>
#include <dirent.h>
#include <elf.h>
#include <time.h>
#include <unistd.h>
#include "e2/exec.h"
#include "e2/loader.h"
#include "e2/syscall.h"

static const char *ROOT = "/tmp/alpine-root";
// One descriptor table per process: fork must copy it and exec must keep it, so
// a single global table let a child's dup3 rewrite the parent's descriptors.
// Per-process facts /proc/self has to report. The harness runs one guest process at a
// time, so these track whichever is currently scheduled.
static int g_cur_ppid;
static char g_cur_cmdline[512];
// Length of the NUL-separated argv image, including the trailing NUL.
static size_t g_cur_cmdline_len;
static int g_proc_self_pid;
extern int g_dbg_pid;

typedef struct {
    int fdtab;
    // The guest's working directory, as it sees it. Lives here rather than in the
    // syscall env because that struct is rebuilt on every scheduling turn, so a
    // chdir recorded there was forgotten before the next file was opened.
    char cwd[1024];
} procctx;

// Children only run when someone calls run_proc for them. That used to happen
// exclusively inside wait4, which deadlocks the moment a parent reads a pipe
// before waiting - which is exactly what $(...) does.
// How many instructions a process may run before yielding to its siblings.
// Without this the scheduler runs each child to completion, which deadlocks any
// pipeline whose producer never finishes on its own - `yes | head -1` being the
// textbook case: head never runs, so the pipe never breaks.
// Preemption granularity. 20000 was too small to be correct: a pipeline whose
// consumer needs more than one read to drain its producer gets interleaved with
// the shell's next command, because wait4 returns as soon as the process it was
// waiting for exits and the surviving sibling has not finished. At 200000 every
// pipeline case in the sweeps comes out right and nothing hangs - `yes | head`
// still terminates, because waiting processes round-robin their siblings rather
// than relying on preemption.
//
// 2000000 rather than 200000: the smaller value still lost data once a pipeline
// carried more than about six thousand lines to wc, grep or awk, because the
// consumer could finish a read before the producer had written the rest. At this
// size every pipeline case in the sweeps is correct, including seq 20000 | wc -l,
// and yes | head -1 still terminates.
//
// This remains a mitigation, not the root fix: wait4 returns as soon as the
// process it waits for exits, so a surviving sibling can still be left behind.
// The window is now wide enough that the case does not arise in practice.
// Tracked in docs/E2-BACKEND.md.
#define PROC_QUANTUM_DEFAULT 2000000
static long proc_quantum(void) {
    static long q = -1;
    if (q < 0) {
        const char *e = getenv("E2_QUANTUM");
        q = e ? atol(e) : PROC_QUANTUM_DEFAULT;
    }
    return q;
}
static long g_proc_budget = -1;
// Counters rather than per-call logging: a fprintf inside the wait path changes
// the timing enough to hide the very bug being measured.
static int g_wait_calls, g_wait_ok, g_wait_echild;
// Counters rather than per-call logging: a fprintf inside the wait path changes
// the timing enough to hide the very bug being measured.
static int g_wait_calls, g_wait_ok, g_wait_echild;

static int pump_children_for_tab(int tab);
static void trace_flush(void);
// Writing to a pipe whose reader has gone raises SIGPIPE on a real system, and
// the default action terminates the writer. Without that, `yes | head -1` runs
// forever: the writer never learns the reader left.
static void note_write_error(int tab, int err);
static struct { int used, host; } g_fds[16][64];
static void *g_dirs[16][64];
static unsigned char g_cloexec[16][64];
#define TAB(c) (g_fds[((const procctx *)(c))->fdtab])
#define DIRTAB(c) (g_dirs[((const procctx *)(c))->fdtab])
static void fd_init_tab(int tab) {
    for (int i = 0; i < 64; i++) { g_fds[tab][i].used = 0; g_fds[tab][i].host = -1; }
    for (int i = 0; i < 3; i++) { g_fds[tab][i].used = 1; g_fds[tab][i].host = i; }
    for (int i = 0; i < 64; i++) { g_dirs[tab][i] = NULL; g_cloexec[tab][i] = 0; }
}
// fork shares the open file description, so a close in one process must not
// close the descriptor for its siblings. Refcounting host descriptors models
// that, and it is what lets a pipe reader see EOF once the writer exits.
static int g_refs[1024];
static void refs_init(void) {
    for (int i = 0; i < 1024; i++) g_refs[i] = 0;
    for (int i = 0; i < 3; i++) g_refs[i] = 1;
}
static void host_ref(int h) { if (h >= 0 && h < 1024) g_refs[h]++; }
static void host_unref(int h) { if (h >= 0 && h < 1024 && g_refs[h] > 0) g_refs[h]--; }
static void host_close_if_done(int h) {
    if (h >= 3 && h < 1024 && g_refs[h] == 0) close(h);
}
static void fd_init(void) { fd_init_tab(0); refs_init(); }
static int fd_alloc_tab(int tab, int host) {
    // POSIX says open returns the lowest free descriptor, and real kernels do.
    // Starting at 3 looked safe but is not: a program that closes stdin and then
    // opens a file expects to get 0 back, and hard-coding 3 breaks every tool
    // built that way - uniq was the one that showed it.
    for (int i = 0; i < 64; i++)
        if (!g_fds[tab][i].used) { g_fds[tab][i].used = 1; g_fds[tab][i].host = host; return i; }
    return -1;
}

static uint8_t *load_file(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long s = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)s);
    if (fread(b, 1, (size_t)s, f) != (size_t)s) { fclose(f); free(b); return NULL; }
    fclose(f); *n = (size_t)s; return b;
}
// The cwd of whichever process is running. host_path is reached from the file-op
// callbacks, which receive a procctx; setting this once per turn keeps relative
// paths resolving where the guest thinks they are.
// A copy, not a pointer: this used to alias whichever process ran last, so when a
// subshell exited and its slot was cleared, the parent's working directory became a
// buffer of zeros and every relative path stopped resolving.
static char g_cur_cwd[1024] = "/";
// Device nodes cannot be represented by files under the guest root: opening
// /dev/null there yields a regular file, so writes accumulate and reads return data.
// These map to the host's real devices instead.
static const char *host_device(const char *p) {
    static const char *const devs[] = {"/dev/null", "/dev/zero", "/dev/full",
                                       "/dev/random", "/dev/urandom", "/dev/tty"};
    for (size_t i = 0; i < sizeof(devs) / sizeof(devs[0]); i++) {
        if (strcmp(p, devs[i]) == 0) return devs[i];
    }
    return NULL;
}
static void host_path(char *out, size_t n, const char *p) {
    const char *dev = host_device(p);
    if (dev != NULL) {
        snprintf(out, n, "%s", dev);
        return;
    }
    if (p[0] == '/') {
        snprintf(out, n, "%s%s", ROOT, p);
    } else if (strcmp(p, ".") == 0) {
        snprintf(out, n, "%s%s", ROOT, g_cur_cwd);
    } else {
        snprintf(out, n, "%s%s/%s", ROOT, g_cur_cwd, p);
    }
}
// /dev/stdin, /dev/stdout, /dev/stderr and /dev/fd/N are descriptor aliases, not
// files. Under the guest root they resolve to nothing, so `cat /dev/stdin` failed.
// Returns the guest fd they name, or -1.
static int device_fd_alias(const char *path) {
    if (strcmp(path, "/dev/stdin") == 0) return 0;
    if (strcmp(path, "/dev/stdout") == 0) return 1;
    if (strcmp(path, "/dev/stderr") == 0) return 2;
    if (strncmp(path, "/dev/fd/", 8) == 0) {
        const char *num = path + 8;
        if (!isdigit((unsigned char)num[0])) return -1;
        return atoi(num);
    }
    return -1;
}

// /proc/self is per-process, so it cannot be a file seeded once at start-up. These
// three are what programs actually read: cmdline, stat and status. A real /proc also
// has a directory per pid; ps needs that, and it stays unsupported rather than
// half-implemented.
static bool proc_self_body(const char *path, char *out, size_t outlen, size_t *len_out) {
    const char *tail = NULL;
    if (strncmp(path, "/proc/self/", 11) == 0) tail = path + 11;
    else if (strncmp(path, "/proc/thread-self/", 18) == 0) tail = path + 18;
    if (tail == NULL) return false;
    const char *name = g_cur_cmdline[0] ? g_cur_cmdline : "e2";
    if (strcmp(tail, "cmdline") == 0) {
        // Already NUL separated and terminated; copy the image verbatim.
        size_t n = g_cur_cmdline_len;
        if (n > outlen) n = outlen;
        memcpy(out, g_cur_cmdline, n);
        *len_out = n;
        return true;
    }
    if (strcmp(tail, "stat") == 0) {
        const int n = snprintf(out, outlen,
                 "%d (%s) R %d 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0",
                 g_proc_self_pid, name, g_cur_ppid);
        *len_out = (n > 0) ? ((size_t)n < outlen ? (size_t)n : outlen) : 0;
        return true;
    }
    if (strcmp(tail, "status") == 0) {
        const int n = snprintf(out, outlen,
                 "Name:\t%s\nPid:\t%d\nPPid:\t%d\nUid:\t0\t0\t0\t0\n"
                 "Gid:\t0\t0\t0\t0\nThreads:\t1\n",
                 name, g_proc_self_pid, g_cur_ppid);
        *len_out = (n > 0) ? ((size_t)n < outlen ? (size_t)n : outlen) : 0;
        return true;
    }
    return false;
}

static int fs_open(void *c, const char *path, int fl, int m) {
    (void)m;
    // An alias names a descriptor already open; duplicate it rather than looking
    // for a file.
    {
        const int alias = device_fd_alias(path);
        if (alias >= 0) {
            if (alias >= 64 || !TAB(c)[alias].used) return -EBADF;
            const int nfd = fd_alloc_tab(((procctx *)c)->fdtab, TAB(c)[alias].host);
            if (nfd < 0) return -EMFILE;
            if (DIRTAB(c)[nfd] != NULL) { closedir((DIR *)DIRTAB(c)[nfd]);
                                         DIRTAB(c)[nfd] = NULL; }
            return nfd;
        }
    }
    // A /proc/self request is answered from the current process rather than from
    // the guest root, where no such file could exist.
    const char *use_path = path;
    char self_tmp[64];
    {
        char body[1024];
        size_t body_len = 0;
        if (proc_self_body(path, body, sizeof(body), &body_len)) {
            // One scratch file, reused: only the process being scheduled can be reading it,
            // and a per-pid name left one file behind for every pid ever used.
            snprintf(self_tmp, sizeof(self_tmp), "/tmp/.e2-proc-self");
            char self_host[2096];
            host_path(self_host, sizeof(self_host), self_tmp);
            FILE *f = fopen(self_host, "wb");
            if (f == NULL) return -ENOENT;
            fwrite(body, 1, body_len, f);
            fclose(f);
            use_path = self_tmp;
        }
    }
    char h[2048]; host_path(h, sizeof(h), use_path);
    // Translate the guest's aarch64 open flags into host ones. O_CREAT without
    // O_WRONLY/RDWR is meaningless, so only honour it when writing.
    int hf = 0;
    const int acc = fl & 3;
    if (acc == 0) hf = O_RDONLY;
    else if (acc == 1) hf = O_WRONLY;
    else hf = O_RDWR;
    if (fl & 0x40) hf |= O_CREAT;        // 0x40 = O_CREAT on aarch64
    if (fl & 0x200) hf |= O_TRUNC;       // 0x200 = O_TRUNC
    if (fl & 0x400) hf |= O_APPEND;      // 0x400 = O_APPEND
    if (fl & 0x4000) hf |= O_DIRECTORY;   // musl aarch64 O_DIRECTORY
    if (fl & 0x8000) hf |= O_NOFOLLOW;    // musl aarch64 O_NOFOLLOW
    // 0x20000 is O_LARGEFILE: meaningless on a 64-bit host, so it is dropped.
    int fd = open(h, hf, 0666);
    if (fd < 0) return -errno;
    host_ref(fd);
    int guest = fd_alloc_tab(((const procctx *)c)->fdtab, fd);
    if (guest < 0) { close(fd); return -24; }
    return guest;
}
static long fs_read(void *c, int fd, void *b, size_t n, long long o) {
    if (fd < 0 || fd >= 64 || !TAB(c)[fd].used) return -EBADF;
    for (;;) {
        ssize_t r = o < 0 ? read(TAB(c)[fd].host, b, n) : pread(TAB(c)[fd].host, b, n, o);
        if (r >= 0) {
            return (long)r;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) return -errno;
        // Nothing to read yet. A child can only make progress if we run it, so
        // do that and try again; when no child is left the pipe really is at
        // end of file.
        if (!pump_children_for_tab(((const procctx *)c)->fdtab)) return 0;
    }
}
static long fs_write(void *c, int fd, const void *b, size_t n, long long o) {
    (void)o;
    if (fd < 0 || fd >= 64 || !TAB(c)[fd].used) return -EBADF;
    if (getenv("E2_TRACE_WDATA")) {
        char wb[80];
        size_t wn = n < 60 ? n : 60;
        for (size_t z = 0; z < wn; z++)
            wb[z] = (((const char *)b)[z] == 0x0a) ? '|' : ((const char *)b)[z];
        wb[wn] = 0;
        fprintf(stderr, "[wr] p%d fd=%d len=%zu \"%s\"\n", g_dbg_pid, fd, n, wb);
    }
    ssize_t r = write(TAB(c)[fd].host, b, n);
    if (r < 0) {
        note_write_error(((const procctx *)c)->fdtab, errno);
        return -errno;
    }
    return (long)r;
}
static int fs_close(void *c, int fd) {
    if (fd < 0 || fd >= 64 || !TAB(c)[fd].used) return -EBADF;
    // A directory descriptor has no host fd of its own; it owns a DIR stream.
    if (DIRTAB(c)[fd] != NULL) {
        closedir((DIR *)DIRTAB(c)[fd]);
        DIRTAB(c)[fd] = NULL;
    } else {
        const int h = TAB(c)[fd].host;
        host_unref(h);
        host_close_if_done(h);
    }
    TAB(c)[fd].used = 0; TAB(c)[fd].host = -1; return 0;
}
static int fs_stat(void *c, int fd, void *buf, size_t n) {
    (void)n; struct stat st;
    if (fd < 0 || fd >= 64 || !TAB(c)[fd].used) return -EBADF;
    if (fstat(TAB(c)[fd].host, &st)) return -EBADF;
    memset(buf, 0, 128);
    // aarch64 struct stat: 64-bit st_dev/st_ino, st_mode at 16, st_size at 48.
    uint8_t *p = buf;
    *(uint64_t*)(p + 0)  = (uint64_t)st.st_dev;
    *(uint64_t*)(p + 8)  = (uint64_t)st.st_ino;
    *(uint32_t*)(p + 16) = (uint32_t)st.st_mode;
    *(uint32_t*)(p + 20) = (uint32_t)st.st_nlink;
    *(uint32_t*)(p + 24) = (uint32_t)st.st_uid;
    *(uint32_t*)(p + 28) = (uint32_t)st.st_gid;
    *(uint64_t*)(p + 32) = (uint64_t)st.st_rdev;
    *(int64_t*)(p + 48)  = (int64_t)st.st_size;
    *(int32_t*)(p + 56)  = 4096;
    *(int64_t*)(p + 64)  = (int64_t)st.st_blocks;
    *(int64_t*)(p + 72)  = (int64_t)st.st_atime;
    *(int64_t*)(p + 88)  = (int64_t)st.st_mtime;
    *(int64_t*)(p + 104) = (int64_t)st.st_ctime;
    return 0;
}
// /proc/self/fd is a directory of symlinks named after the open descriptors. There
// is no such directory under the guest root, so build one on demand and open that.
static void build_proc_self_fd(void *c, const char *dir_host) {
    mkdir(dir_host, 0755);
    for (int fd = 0; fd < 64; fd++) {
        if (!TAB(c)[fd].used) continue;
        char name[2200];
        snprintf(name, sizeof(name), "%s/%d", dir_host, fd);
        unlink(name);
        // The target is only ever read as text by tools like ls -l; the real
        // /proc/self/fd entries point at the open file, which this does not model.
        if (symlink("/dev/null", name) != 0) { /* best effort */ }
    }
}

static int fs_opendir(void *c, const char *path) {
    if (strcmp(path, "/proc/self/fd") == 0 || strcmp(path, "/dev/fd") == 0) {
            char dir_host[2048];
        snprintf(dir_host, sizeof(dir_host), "%s/proc/self/fd", ROOT);
        // Take the descriptor for the directory first: a real /proc/self/fd lists
        // the descriptor that is reading it, so the count would be one short
        // otherwise.
        const int pfd = fd_alloc_tab(((procctx *)c)->fdtab, -1);
        if (pfd < 0) return -EMFILE;
        build_proc_self_fd(c, dir_host);
        DIR *pd = opendir(dir_host);
        if (!pd) return -errno;
        DIRTAB(c)[pfd] = pd;
        return pfd;
    }
    char h[2048]; host_path(h, sizeof(h), path);
    DIR *d = opendir(h); if (!d) return -errno;
    // A directory is a descriptor like any other and must come from the same
    // table. Taking the first free slot of g_dirs handed back 0, 1 and 2 - so a
    // recursive walk silently installed its directories over stdin, stdout and
    // stderr, and the last write failed with EBADF.
    const int fd = fd_alloc_tab(((const procctx *)c)->fdtab, -1);
    if (fd < 0) { closedir(d); return -24; }
    DIRTAB(c)[fd] = d;
    return fd;
}
static long fs_readdir(void *c, int h, void *buf, size_t buf_size) {
    if (h < 0 || h >= 64 || !DIRTAB(c)[h]) return 0;
    struct dirent *e = readdir((DIR *)DIRTAB(c)[h]);
    if (!e) return 0;
    // getdents64 returns the length of the record, not a count. Returning 1 made
    // every recursive walk (find, ls -R) read a single byte per call and stop:
    // the top-level listing looked fine only because it asks once per entry.
    const size_t name_len = strlen(e->d_name) + 1;
    size_t reclen = 19 + name_len;          // 8 + 8 + 2 + 1 header
    reclen = (reclen + 7) & ~(size_t)7;     // 8-byte aligned
    if (reclen > buf_size) return 0;
    uint8_t *out = (uint8_t *)buf;
    memset(out, 0, reclen);
    uint64_t ino = (uint64_t)e->d_ino;
    int64_t off = (int64_t)e->d_off;
    uint16_t rl = (uint16_t)reclen;
    memcpy(out, &ino, 8);
    memcpy(out + 8, &off, 8);
    memcpy(out + 16, &rl, 2);
    out[18] = (uint8_t)e->d_type;
    memcpy(out + 19, e->d_name, name_len);
    return (long)reclen;
}
static long long fs_clock(void *c, int clock_id) {
    (void)c;
    struct timespec ts;
    // CLOCK_REALTIME is 0 and CLOCK_MONOTONIC is 1, the same numbering the guest
    // uses, so the id passes straight through.
    if (clock_gettime((clockid_t)clock_id, &ts) != 0) {
        clock_gettime(CLOCK_REALTIME, &ts);
    }
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}
static int fs_pipe(void *c, int fds[2]) {
    const int tab = ((const procctx *)c)->fdtab;
    int p[2];
    if (pipe(p) != 0) return -errno;
    // The read end never blocks the harness: fs_read turns EAGAIN into "run the
    // writer", which is what keeps a parent that reads before waiting alive.
    fcntl(p[0], F_SETFL, fcntl(p[0], F_GETFL, 0) | O_NONBLOCK);
    host_ref(p[0]);
    host_ref(p[1]);
    const int a = fd_alloc_tab(tab, p[0]);
    if (a < 0) { close(p[0]); close(p[1]); return -24; }
    const int b = fd_alloc_tab(tab, p[1]);
    if (b < 0) { close(p[0]); close(p[1]); g_fds[tab][a].used = 0; return -24; }
    fds[0] = a; fds[1] = b; return 0;
}
static int fs_dup3(void *c, int oldfd, int newfd, int flags) {
    (void)flags;
    if (oldfd < 0 || oldfd >= 64 || !TAB(c)[oldfd].used) return -EBADF;
    if (newfd < 0 || newfd >= 64) return -EBADF;
    if (oldfd == newfd) return newfd;
    const int h = TAB(c)[oldfd].host;
    if (TAB(c)[newfd].used) {
        host_unref(TAB(c)[newfd].host);
        host_close_if_done(TAB(c)[newfd].host);
    }
    host_ref(h);
    TAB(c)[newfd].used = 1; TAB(c)[newfd].host = h;
    return newfd;
}
static long fs_fcntl(void *c, int fd, int cmd, long arg) {
    const int tab = ((const procctx *)c)->fdtab;
    if (fd < 0 || fd >= 64 || !TAB(c)[fd].used) return -EBADF;
    switch (cmd) {
        case 0:      // F_DUPFD
        case 1030: { // F_DUPFD_CLOEXEC
            // Only the _CLOEXEC variant sets the flag; a plain F_DUPFD must
            // survive exec, which is how a shell restores a saved stdout.
            const int h = TAB(c)[fd].host;
            for (int i = (int)arg; i < 64; i++) {
                if (!g_fds[tab][i].used) {
                    host_ref(h);
                    g_fds[tab][i].used = 1;
                    g_fds[tab][i].host = h;
                    g_cloexec[tab][i] = (cmd == 1030) ? 1 : 0;
                    return i;
                }
            }
            return -24;
        }
        case 1: return g_cloexec[tab][fd] ? 1 : 0;   // F_GETFD
        case 2: g_cloexec[tab][fd] = (arg & 1) ? 1 : 0; return 0;   // F_SETFD
        case 3: return 0x8000; // F_GETFL (O_LARGEFILE only; callers just test bits)
        case 4: return 0;    // F_SETFL
        default: return -22; // EINVAL
    }
}

// readlinkat backs os.readlink, which Python uses while normalising sys.path.
static long fs_readlink(void *ctx, const char *path, char *out, size_t outlen) {
    (void)ctx;
    char h[2048];
    host_path(h, sizeof(h), path);
    const ssize_t n = readlink(h, out, outlen);
    if (n < 0) {
        return -errno;
    }
    return (long)n;
}

// Reports a descriptor as ready for whatever was asked. Regular files always
// are, and a pipe with data pending is too, which covers every case this
// sandbox produces. A poll implementation that returns ENOSYS is worse than this
// approximation: a shell's `while read` loop simply never runs.
static int fs_pollable(void *c, int fd, int events) {
    (void)c;
    if (fd < 0 || fd >= 64) return 0;
    return events;
}

// Filesystem mutations, resolved under the guest root. Without these mkdir, rm,
// ln, chmod and touch all failed together - they are the syscalls behind every
// one of those commands.
static int fs_pathop(void *c, int op, const char *a, const char *b, unsigned mode) {
    (void)c;
    char ha[2048], hb[2048];
    // A symlink's target is stored verbatim: it is a guest path, not something
    // to resolve now. Host-resolving it made readlink report /tmp/alpine-root/...
    if (op == E2_PATHOP_SYMLINK) {
        snprintf(ha, sizeof(ha), "%s", a);
    } else {
        host_path(ha, sizeof(ha), a);
    }
    if (b != NULL && b[0] != '\0') host_path(hb, sizeof(hb), b);

    switch (op) {
    case E2_PATHOP_MKDIR:
        if (mkdir(ha, mode ? mode : 0755) != 0) return -errno;
        return 0;
    case E2_PATHOP_UNLINK:
        if (unlink(ha) != 0) return -errno;
        return 0;
    case E2_PATHOP_RMDIR:
        if (rmdir(ha) != 0) return -errno;
        return 0;
    case E2_PATHOP_RENAME:
        if (rename(ha, hb) != 0) return -errno;
        return 0;
    case E2_PATHOP_SYMLINK:
        if (symlink(ha, hb) != 0) return -errno;
        return 0;
    case E2_PATHOP_LINK:
        if (link(ha, hb) != 0) return -errno;
        return 0;
    case E2_PATHOP_CHOWN:
        // The sandbox runs everything as one user, so ownership is already what it
        // is going to be. Report success rather than ENOSYS: scripts chown to
        // normalise permissions and should not stop on it.
        (void)mode;
        return 0;
    case E2_PATHOP_CHMOD:
        if (chmod(ha, mode) != 0) return -errno;
        return 0;
    case E2_PATHOP_ACCESS: {
        // faccessat's mode uses R_OK/W_OK/X_OK, which already match F_OK-style
        // bit values here; the host check is the same idea.
        if (access(ha, (int)mode) != 0) return -errno;
        return 0;
    }
    case E2_PATHOP_UTIME: {
        struct timespec ts[2];
        ts[0].tv_sec = (time_t)mode;
        ts[0].tv_nsec = 0;
        ts[1] = ts[0];
        if (utimensat(AT_FDCWD, ha, ts, 0) != 0) return -errno;
        return 0;
    }
    default:
        return -ENOSYS;
    }
}

// truncate/ftruncate. Both arrive here; a negative fd means the path form.
static int fs_truncate(void *c, int fd, const char *path, long long length) {
    if (fd >= 0) {
        if (fd >= 64 || !TAB(c)[fd].used) return -EBADF;
        return ftruncate(TAB(c)[fd].host, (off_t)length) == 0 ? 0 : -errno;
    }
    char h[2048];
    host_path(h, sizeof(h), path);
    return truncate(h, (off_t)length) == 0 ? 0 : -errno;
}


// statfs for df. Values come from the host filesystem that backs the root, so
// `df /` reports numbers that are actually true of the disk holding it.
static int fs_statfs(void *c, int fd, const char *path, void *out, size_t outlen) {
    (void)c;
    struct statvfs v;
    int rc;
    if (fd >= 0) {
        if (fd >= 64 || !TAB(c)[fd].used) return -EBADF;
        rc = fstatvfs(TAB(c)[fd].host, &v);
    } else {
        char h[2048];
        host_path(h, sizeof(h), path);
        rc = statvfs(h, &v);
    }
    if (rc != 0) return -errno;
    if (outlen < 128) return -EINVAL;
    uint64_t *w = (uint64_t *)out;
    memset(w, 0, 128);
    w[0] = 0xEF53;              // f_type: ext4's magic, so file(1)-style checks pass
    w[1] = v.f_bsize;
    w[2] = v.f_blocks;
    w[3] = v.f_bfree;
    w[4] = v.f_bavail;
    w[5] = v.f_files;
    w[6] = v.f_ffree;
    // w[7] is fsid, left zero.
    w[8] = v.f_namemax;
    w[9] = v.f_frsize;
    w[10] = v.f_flag;
    return 0;
}

// Repositions a descriptor. Returns the resulting offset so the guest can tell
// where it landed.
static long fs_seek(void *c, int fd, long long offset, int whence) {
    if (fd < 0 || fd >= 64 || !TAB(c)[fd].used) return -EBADF;
    off_t r = lseek(TAB(c)[fd].host, (off_t)offset, whence);
    if (r < 0) return -errno;
    return (long)r;
}

static const e2_file_ops FS = {fs_open, fs_read, fs_write, fs_close, fs_stat,
                               fs_opendir, fs_readdir, fs_clock, fs_pipe, fs_dup3,
                               fs_fcntl, fs_readlink, fs_pollable, fs_pathop, fs_truncate,
                               fs_statfs, fs_seek};



// The guest root has an empty /proc, so free, df, ps and uptime all fail on a
// missing file rather than on anything the interpreter got wrong. A real kernel
// synthesises these; the harness states them once at start-up for the same reason.
// Only written when the directory already exists, so a root without /proc is left
// exactly as it was.
static void seed_proc(const char *root) {
    static const struct { const char *path; const char *body; } files[] = {
        {"/proc/meminfo",
         "MemTotal:        1024000 kB\n"
         "MemFree:          512000 kB\n"
         "MemAvailable:     768000 kB\n"
         "Buffers:           10000 kB\n"
         "Cached:           120000 kB\n"
         "SwapTotal:             0 kB\n"
         "SwapFree:              0 kB\n"},
        {"/proc/uptime", "12345.67 98765.43\n"},
        {"/proc/version",
         "Linux version 6.1.0-e2 (e2@localhost) #1 SMP aarch64\n"},
        {"/proc/mounts", "/dev/root / ext4 rw,relatime 0 0\n"},
        {"/proc/cpuinfo",
         "processor\t: 0\nBogoMIPS\t: 100.00\n"
         "CPU implementer\t: 0x41\nCPU architecture: 8\n"
         "CPU variant\t: 0x0\nCPU part\t: 0xd0c\nCPU revision\t: 1\n"},
        {"/proc/filesystems", "\tnodev\nsysfs\nproc\next4\ntmpfs\n"},
    };
    char dir[2048];
    snprintf(dir, sizeof(dir), "%s/proc", root);
    struct stat st;
    if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) return;
    // /proc/self/fd must exist as a directory: ls stats the path before listing it,
    // and a directory that only materialises inside opendir is invisible to that.
    {
        char p1[2048], p2[2048];
        snprintf(p1, sizeof(p1), "%s/proc/self", root);
        snprintf(p2, sizeof(p2), "%s/proc/self/fd", root);
        mkdir(p1, 0555);
        mkdir(p2, 0555);
    }
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        char full[2048];
        snprintf(full, sizeof(full), "%s%s", root, files[i].path);
        // Do not clobber a file the guest has already produced.
        if (access(full, F_OK) == 0) continue;
        FILE *f = fopen(full, "wb");
        if (f == NULL) continue;
        fputs(files[i].body, f);
        fclose(f);
    }
}

// --- processes -----------------------------------------------------------

// Returns the PT_INTERP path of an ELF image, or NULL for a static binary.
static const char *elf_interp(const uint8_t *img, size_t n) {
    if (n < sizeof(Elf64_Ehdr)) return NULL;
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)img;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0) return NULL;
    if (eh->e_phoff == 0 || eh->e_phoff + (size_t)eh->e_phnum * sizeof(Elf64_Phdr) > n) return NULL;
    const Elf64_Phdr *ph = (const Elf64_Phdr *)(img + eh->e_phoff);
    for (unsigned i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_INTERP && ph[i].p_offset < n) {
            return (const char *)(img + ph[i].p_offset);
        }
    }
    return NULL;
}

// Builds the initial image in `m` exactly the way the Linux kernel does: the
// program at the low base, its interpreter above it, argv/envp/auxv on the new
// stack, and the entry point taken from the interpreter when there is one.
// Returns 0 on success, negative errno otherwise.
// 64 bytes per argument silently truncated anything longer - a long awk program, a
// long sed script, any command line past 63 characters. The limit was invisible
// until an awk program of 66 characters failed to parse.
#define ARG_MAX 1024
static int start_image(e2_machine *m, const char *path, char args[][ARG_MAX], int nargs,
                       char envs[][ARG_MAX], int nenv) {
    // /proc/self/cmdline reports argv[0], which is the applet name for busybox and
    // the program name otherwise.
    // /proc/self/cmdline is the whole argv, NUL separated and NUL terminated - some
    // tools read past the first entry, so storing only argv[0] would look plausible
    // and be wrong.
    {
        size_t off = 0;
        if (nargs > 0) {
            for (int i = 0; i < nargs && off + 1 < sizeof(g_cur_cmdline); i++) {
                const size_t n = strlen(args[i]);
                if (off + n + 1 >= sizeof(g_cur_cmdline)) break;
                memcpy(g_cur_cmdline + off, args[i], n);
                off += n;
                g_cur_cmdline[off++] = '\0';
            }
        } else {
            const char *base = strrchr(path, '/');
            const char *nm = base ? base + 1 : path;
            const size_t n = strlen(nm);
            memcpy(g_cur_cmdline, nm, n < sizeof(g_cur_cmdline) - 1 ? n : sizeof(g_cur_cmdline) - 1);
            off = n < sizeof(g_cur_cmdline) - 1 ? n : sizeof(g_cur_cmdline) - 1;
            g_cur_cmdline[off++] = '\0';
        }
        g_cur_cmdline_len = off;
    }
    size_t size = 0;
    uint8_t *img = load_file(path, &size);
    if (img == NULL) return -2;   // ENOENT

    e2_memory_destroy(&m->mem);
    e2_machine_init(m, 0, 1 << 20);

    const char *interp_path = elf_interp(img, size);
    e2_loaded_image app = {0}, ld = {0};
    e2_load_result r;
    uint64_t entry;
    if (interp_path != NULL) {
        char host[2048];
        host_path(host, sizeof(host), interp_path);
        size_t ld_size = 0;
        uint8_t *ld_img = load_file(host, &ld_size);
        if (ld_img == NULL) { free(img); return -2; }
        r = e2_load_elf_at(m, img, size, 0x40000000ULL, &app);
        if (r != E2_LOAD_OK) { free(img); free(ld_img); return -8; }
        if (getenv("E2_TRACE_LOAD"))
            fprintf(stderr, "[load] app ok bias=0x%llx entry=0x%llx ld_size=%zu\n",
                    (unsigned long long)app.load_bias, (unsigned long long)app.entry, ld_size);
        r = e2_load_elf_at(m, ld_img, ld_size, 0x50000000ULL, &ld);
        free(ld_img);
        if (getenv("E2_TRACE_LOAD"))
            fprintf(stderr, "[load] ld result=%s bias=0x%llx entry=0x%llx\n",
                    e2_load_result_name(r), (unsigned long long)ld.load_bias,
                    (unsigned long long)ld.entry);
        if (r != E2_LOAD_OK) { free(img); return -8; }
        entry = ld.entry;
    } else {
        r = e2_load_elf_full(m, img, size, true, &app);
        if (r != E2_LOAD_OK) { free(img); return -8; }
        entry = app.entry;
    }
    free(img);
    if (getenv("E2_TRACE_LOAD"))
        fprintf(stderr, "[load] interp=%s app_entry=0x%llx app_bias=0x%llx ld_entry=0x%llx ld_bias=0x%llx entry=0x%llx\n",
                interp_path ? interp_path : "-", (unsigned long long)app.entry,
                (unsigned long long)app.load_bias, (unsigned long long)ld.entry,
                (unsigned long long)ld.load_bias, (unsigned long long)entry);

    uint64_t top = m->cpu.sp - 64, strs = top - 2048, cur = strs;
    uint64_t argp[16], envp[16];
    // Strings are packed back to back, as a real kernel does. Padding each one
    // to 16 bytes changes their addresses, and an aligned address takes a
    // different branch inside musl's word-at-a-time strlen - which shows up as
    // a divergence in any trace comparison and is purely an artifact.
    for (int i = 0; i < nargs; i++) {
        size_t l = strlen(args[i]) + 1;
        e2_memory_write(&m->mem, cur, args[i], l);
        argp[i] = cur;
        cur += l;
    }
    for (int i = 0; i < nenv; i++) {
        size_t l = strlen(envs[i]) + 1;
        e2_memory_write(&m->mem, cur, envs[i], l);
        envp[i] = cur;
        cur += l;
    }
    // AT_RANDOM must point at 16 readable bytes. musl takes the stack canary
    // and the pointer guard from it; leaving the entry out means a zero canary,
    // which still "works" and is exactly the kind of silent difference that is
    // impossible to see later.
    uint64_t rnd = cur;
    {
        static const unsigned char seed[16] = {
            0x9e, 0x37, 0x79, 0xb9, 0x7f, 0x4a, 0x7c, 0x15,
            0x5a, 0x2f, 0xe4, 0x68, 0x3d, 0x2c, 0x0e, 0x61,
        };
        e2_memory_write(&m->mem, rnd, seed, sizeof(seed));
        cur += 16;
    }
    uint64_t sp = (strs - 512) & ~15ULL;
    // AT_* come from <elf.h>.
    uint64_t st[80];
    int k = 0;
    st[k++] = (uint64_t)nargs;
    for (int i = 0; i < nargs; i++) st[k++] = argp[i];
    st[k++] = 0;
    for (int i = 0; i < nenv; i++) st[k++] = envp[i];
    st[k++] = 0;
    st[k++] = AT_PAGESZ; st[k++] = 4096;
    st[k++] = AT_PHDR;   st[k++] = app.phdr_addr;
    st[k++] = AT_PHENT;  st[k++] = app.phentsize;
    st[k++] = AT_PHNUM;  st[k++] = app.phnum;
    st[k++] = AT_BASE;   st[k++] = ld.entry ? ld.load_bias : 0;
    st[k++] = AT_ENTRY;  st[k++] = app.entry;
    st[k++] = AT_EXECFN; st[k++] = argp[0];
    // The rest of the standard set. A program that reads one of these and finds
    // it absent gets zero, which may mean "no capability" - but it may also mean
    // "unknown", and the two lead to different paths.
    st[k++] = AT_HWCAP;   st[k++] = 0;
    st[k++] = AT_HWCAP2;  st[k++] = 0;
    st[k++] = AT_UID;     st[k++] = 0;
    st[k++] = AT_EUID;    st[k++] = 0;
    st[k++] = AT_GID;     st[k++] = 0;
    st[k++] = AT_EGID;    st[k++] = 0;
    st[k++] = AT_SECURE;  st[k++] = 0;
    st[k++] = AT_CLKTCK;  st[k++] = 100;
    st[k++] = AT_FLAGS;   st[k++] = 0;
    st[k++] = AT_RANDOM;  st[k++] = rnd;
    st[k++] = AT_NULL;    st[k++] = 0;
    e2_memory_write(&m->mem, sp, st, (size_t)k * 8);
    m->cpu.sp = sp;
    m->cpu.pc = entry;
    return 0;
}

typedef struct {
    procctx pc;   // must stay first: file ops cast ctx to procctx
    e2_machine m;
    int pid, ppid;
    int alive;
    int exit_status;
    int stopped;
    int used;
} proc_t;
static proc_t procs[16];
static int g_next_pid = 2;
// Shared with debug builds of the memory layer that need the faulting pc.
uint64_t g_dbg_pc;
// Also publish sp, so a debug build can express a stack watch as an offset from
// the current frame rather than an absolute address. Absolute addresses move
// between runs, which made several stack investigations report a write in one
// run and nothing in the next for the very same logical slot.
uint64_t g_dbg_sp;
// One counter across all processes. Per-process instruction counts cannot order
// events that happen in different processes, which is exactly what pipeline and
// scheduling questions need.
unsigned long long g_global_step;
int g_dbg_pid;
extern unsigned long long g_dbg_last_syscall;
uint64_t g_dbg_pc2;
static const char *g_self_path = NULL;

static int proc_slot(void) {
    for (int i = 1; i < 16; i++) if (!procs[i].used) return i;
    return -1;
}
// fork copies the descriptor table; dup() gives the child independent closes
// while pipe data stays shared, which is what the shell needs.
static void fd_table_copy(int dst, int src) {
    for (int i = 0; i < 64; i++) {
        g_dirs[dst][i] = NULL;
        g_cloexec[dst][i] = g_cloexec[src][i];
        if (g_fds[src][i].used) {
            g_fds[dst][i].used = 1;
            g_fds[dst][i].host = g_fds[src][i].host;
            host_ref(g_fds[dst][i].host);
        } else {
            g_fds[dst][i].used = 0;
            g_fds[dst][i].host = -1;
        }
    }
}

static int run_proc(int idx);

// A process's descriptors are released when it exits; without this the write
// end of a pipe stays referenced forever and its reader never sees EOF.
static void fd_table_release(int tab) {
    for (int i = 0; i < 64; i++) {
        if (!g_fds[tab][i].used) continue;
        host_unref(g_fds[tab][i].host);
        host_close_if_done(g_fds[tab][i].host);
        g_fds[tab][i].used = 0;
        g_fds[tab][i].host = -1;
        g_cloexec[tab][i] = 0;
        if (g_dirs[tab][i]) { closedir((DIR *)g_dirs[tab][i]); g_dirs[tab][i] = NULL; }
    }
}

static void p_exit(void *ctx, int status) {
    fd_table_release(((proc_t *)ctx)->pc.fdtab);
    proc_t *p = ctx;
    p->alive = 0;
    trace_flush();
    p->exit_status = status;
}

static long p_fork(void *ctx, const e2_machine *parent) {
    proc_t *self = ctx;
    const int slot = proc_slot();
    if (slot < 0) return -12;
    proc_t *c = &procs[slot];
    if (e2_machine_clone(parent, &c->m) != 0) return -12;
    c->pc.fdtab = slot;
    fd_table_copy(slot, self->pc.fdtab);
    c->pid = g_next_pid++;
    c->ppid = self->pid;
    // A child inherits the working directory; without this every forked command
    // started at / and relative paths resolved to the wrong place.
    snprintf(c->pc.cwd, sizeof(c->pc.cwd), "%s", self->pc.cwd);
    c->alive = 1;
    c->used = 1;
    c->exit_status = 0;
    // The child's fork() returns 0; the parent's return value is written by the
    // syscall layer after this callback returns.
    c->m.cpu.x[0] = 0;
    if (getenv("E2_TRACE_PROC"))
        fprintf(stderr, "[fork] parent=%d child=%d pc=0x%llx sp=0x%llx regions=%u\n",
                self->pid, c->pid, (unsigned long long)c->m.cpu.pc,
                (unsigned long long)c->m.cpu.sp, c->m.mem.count);
    return c->pid;
}

// Build a fresh address space for `path` and lay out argv/envp from the OLD
// machine's memory, which is about to be discarded.
static int p_execve(void *ctx, e2_machine *m, const char *path, uint64_t argv,
                    uint64_t envp) {
    char args[8][ARG_MAX]; int nargs = 0;
    for (int i = 0; i < 8; i++) {
        uint64_t ptr = 0;
        if (e2_memory_read(&m->mem, argv + (uint64_t)i * 8, &ptr, 8) != 0 || ptr == 0) break;
        if (e2_memory_read(&m->mem, ptr, args[nargs], sizeof(args[0])) != 0) break;
        nargs++;
    }
    // Same class of limit as ARG_MAX: 96 bytes per variable and eight variables
    // silently truncated a long PATH or dropped anything beyond the eighth.
    char envs[64][ARG_MAX]; int nenv = 0;
    for (int i = 0; i < 64; i++) {
        uint64_t ptr = 0;
        if (e2_memory_read(&m->mem, envp + (uint64_t)i * 8, &ptr, 8) != 0 || ptr == 0) break;
        if (e2_memory_read(&m->mem, ptr, envs[nenv], sizeof(envs[0])) != 0) break;
        nenv++;
    }

    // The guest's own /bin/busybox is dynamically linked and now loadable, so
    // resolve the real path first and fall back to the running binary only if
    // it is missing.
    char host[2048]; host_path(host, sizeof(host), path);
    // Prefer the image we are already running: it is the one whose behaviour the
    // backend has been verified against, and mixing in a dynamically linked
    // applet makes child failures harder to attribute.
    if (g_self_path != NULL && strcmp(g_self_path, host) != 0) {
        if (nargs == 0) {
            const char *base = strrchr(path, '/');
            snprintf(args[0], sizeof(args[0]), "%s", base ? base + 1 : path);
            nargs = 1;
        }
        snprintf(host, sizeof(host), "%s", g_self_path);
    }
    if (nargs == 0) {
        const char *base = strrchr(path, '/');
        snprintf(args[0], sizeof(args[0]), "%s", base ? base + 1 : path);
        nargs = 1;
    }

    // execve closes FD_CLOEXEC descriptors before the new image starts.
    {
        const int tab = ((const procctx *)ctx)->fdtab;
        for (int i = 0; i < 64; i++) {
            if (!g_fds[tab][i].used || !g_cloexec[tab][i]) continue;
            host_unref(g_fds[tab][i].host);
            host_close_if_done(g_fds[tab][i].host);
            g_fds[tab][i].used = 0;
            g_fds[tab][i].host = -1;
            g_cloexec[tab][i] = 0;
        }
    }
    return start_image(m, host, args, nargs, envs, nenv);
}

/// Terminates the writer when its reader has gone.
///
/// Only EPIPE is treated as fatal. A real SIGPIPE can be caught or ignored, but
/// nothing in this rootfs does that, and hanging forever is far worse than
/// being wrong about the rare handler case.
static void note_write_error(int tab, int err) {
    if (err != EPIPE) return;
    for (int i = 0; i < 16; i++) {
        if (!procs[i].used || procs[i].pc.fdtab != tab) continue;
        if (!procs[i].alive) return;
        procs[i].alive = 0;
        procs[i].exit_status = 141;  // 128 + SIGPIPE
        return;
    }
}

/// Runs the processes that could make progress on `tab`'s pipe, returning how
/// many ran.
///
/// Called from fs_read when a pipe has no data. It deliberately runs *peers*,
/// not just children: in `ls | wc -l` the reader has no children of its own, and
/// returning "none to run" there means reporting end-of-file on a pipe that the
/// sibling is about to write to.
static int pump_children_for_tab(int tab) {
    int ran = 0;
    for (int i = 0; i < 16; i++) {
        proc_t *c = &procs[i];
        if (!c->used || !c->alive) continue;
        if (c->pc.fdtab == tab) continue;   // that is us
        // run_proc here is nested inside the caller's own run_proc. Saving and
        // restoring the budget keeps the outer process bounded: resetting it to
        // -1 after the inner run left the caller unbounded, and preemption then
        // landed wherever it liked.
        const long outer_budget = g_proc_budget;
        g_proc_budget = proc_quantum();
        run_proc(i);
        g_proc_budget = outer_budget;
        ran++;
    }
    return ran;
}

static long p_wait4(void *ctx, int pid, uint64_t status_ptr, int options) {
    (void)options;
    g_wait_calls++;
    proc_t *self = ctx;
    for (int i = 1; i < 16; i++) {
        proc_t *c = &procs[i];
        if (!c->used || c->ppid != self->pid) continue;
        if (pid > 0 && c->pid != pid) continue;
        // Round-robin until this child exits. Running it for one quantum and
        // then reaping would reap a live process; running it to completion
        // starves its siblings, and a producer that never finishes on its own
        // then keeps the consumer from ever running - `yes | head -1` hangs
        // exactly that way.
        while (c->alive) {
            const long saved_budget = g_proc_budget;
            g_proc_budget = proc_quantum();
            run_proc(i);
            g_proc_budget = saved_budget;
            for (int j = 1; j < 16; j++) {
                if (j == i) continue;
                proc_t *other = &procs[j];
                if (!other->used || !other->alive || other->ppid != self->pid) continue;
                const long saved_budget2 = g_proc_budget;
                g_proc_budget = proc_quantum();
                run_proc(j);
                g_proc_budget = saved_budget2;
            }
        }
        const int code = c->exit_status & 0xFF;
        const uint32_t status = (uint32_t)(code << 8);
        uint8_t *dst = e2_memory_host_ptr(&self->m.mem, status_ptr, 4, E2_PROT_WRITE);
        if (dst) memcpy(dst, &status, 4);
        if (getenv("E2_TRACE_WAIT"))
            fprintf(stderr, "[wait] child=%d status=%d instr=%ld\n", c->pid, c->exit_status,
                    (long)c->m.instructions);
        g_wait_ok++;
        g_wait_ok++;
        const int reaped = c->pid;
        fd_table_release(c->pc.fdtab);
        e2_memory_destroy(&c->m.mem);
        memset(c, 0, sizeof(*c));
        return reaped;
    }
    g_wait_echild++;
    g_wait_echild++;
    return -10;   // ECHILD
}

static int g_fault = 0;
static char g_reason[512];

// --- shadow call stack ------------------------------------------------------
struct shadow_frame { uint64_t call_pc, ret_pc, x0; };
static struct shadow_frame g_shadow[48];
static int g_shadow_n;
static bool g_shadow_on;

static void dump_shadow(const char *why) {
    if (g_shadow_n == 0) {
        fprintf(stderr, "[stack] empty (%s)\n", why);
        return;
    }
    fprintf(stderr, "[stack] %d frames at %s (outermost first):\n", g_shadow_n, why);
    for (int i = 0; i < g_shadow_n; i++) {
        fprintf(stderr, "   #%-2d caller=0x%llx ret=0x%llx x0_at_entry=0x%llx\n",
                i, (unsigned long long)g_shadow[i].call_pc,
                (unsigned long long)g_shadow[i].ret_pc,
                (unsigned long long)g_shadow[i].x0);
    }
}

// Trace capture state lives at file scope so the exit path can flush a
// partial recording. Making it depend on reaching an exact length meant a
// run shorter than the requested limit printed nothing at all.
static uint64_t g_trace_buf[65536];
static int g_trace_n = 0;
static int g_trace_recording = 0;

static void trace_flush(void) {
    if (g_trace_recording != 1) {
        return;
    }
    g_trace_recording = 2;
    for (int q = 0; q < g_trace_n; q++) {
        fprintf(stdout, "%llx\n", (unsigned long long)g_trace_buf[q]);
    }
    fflush(stdout);
}

static int run_proc(int idx) {
    proc_t *p = &procs[idx];
    long spent = 0;
    e2_syscall_env env = {.exit = p_exit, .files = &FS, .ctx = p,
                          .fork = p_fork, .execve = p_execve, .wait4 = p_wait4};
    // A process starts at the root unless it has moved; E2_CWD still forces one.
    if (p->pc.cwd[0] == '\0') {
        snprintf(p->pc.cwd, sizeof(p->pc.cwd), "%s",
                 getenv("E2_CWD") ? getenv("E2_CWD") : "/");
    }
    snprintf(env.cwd, sizeof(env.cwd), "%s", p->pc.cwd);
    snprintf(g_cur_cwd, sizeof(g_cur_cwd), "%s", p->pc.cwd);
    env.pid = p->pid;
    env.ppid = p->ppid;
    if (getenv("E2_TRACE_WAIT"))
        fprintf(stderr, "[run] pid=%d pc=0x%llx alive=%d\n", p->pid,
                (unsigned long long)p->m.cpu.pc, p->alive);
    // Ring buffer of recent steps: when execution stops on a bad access, the
    // useful question is "how did this register get here", and that needs the
    // preceding instructions rather than the faulting one.
    struct { uint64_t pc, insn, x0, x1, x19, x20, x23; } hist[256];
    unsigned hist_at = 0, hist_n = 0;
    long syscalls_seen = 0;
    long guard = 0;
    while (p->alive) {
        if (g_proc_budget >= 0 && ++spent > g_proc_budget) break;
        g_dbg_pc = p->m.cpu.pc;
        g_dbg_sp = p->m.cpu.sp;
        g_dbg_pid = p->pid;
        g_proc_self_pid = p->pid;
        g_cur_ppid = p->ppid;
        g_dbg_pc2 = p->m.cpu.pc;
        hist[hist_at].pc = p->m.cpu.pc;
        // Read the word at pc rather than ctx.instruction: ctx carries the
        // instruction of the previous step, which put every history entry one
        // behind its own pc and made the trace read as if a branch had gone the
        // wrong way.
        {
            uint32_t word = 0;
            if (e2_memory_read(&p->m.mem, p->m.cpu.pc, &word, 4) != 0) word = 0;
            hist[hist_at].insn = word;
        }
        hist[hist_at].x0 = p->m.cpu.x[0];
        hist[hist_at].x1 = p->m.cpu.x[1];
        hist[hist_at].x19 = p->m.cpu.x[19];
        hist[hist_at].x20 = p->m.cpu.x[20];
        hist[hist_at].x23 = p->m.cpu.x[23];
        hist_at = (hist_at + 1) % 256;
        if (hist_n < 256) hist_n++;
        if (getenv("E2_TRACE_TRACE") &&
            p->m.cpu.pc >= 0x10161070 && p->m.cpu.pc < 0x101610f0) {
            static long nt = 0;
            if (nt < 80) {
                fprintf(stderr, "  T %ld pc=0x%llx x21=%llu x26=0x%llx w24=%llu\n", nt,
                        (unsigned long long)p->m.cpu.pc,
                        (unsigned long long)p->m.cpu.x[21],
                        (unsigned long long)p->m.cpu.x[26],
                        (unsigned long long)(uint32_t)p->m.cpu.x[24]);
                nt++;
            }
        }
        if (getenv("E2_TRACE_REGIONS") && p->m.cpu.pc == 0x10161074) {
            static int nr = 0;
            if (nr == 0) {
                fprintf(stderr, "[regions] count=%u\n", p->m.mem.count);
                for (unsigned k = 0; k < p->m.mem.count; k++)
                    fprintf(stderr, "   [%u] base=0x%llx size=0x%llx prot=%d\n", k,
                            (unsigned long long)p->m.mem.regions[k].base,
                            (unsigned long long)p->m.mem.regions[k].size,
                            p->m.mem.regions[k].prot);
                nr++;
            }
        }
        if (getenv("E2_TRACE_T3") && p->m.cpu.pc == 0x10163230) {
            static unsigned v3 = 0;
            static uint64_t prev = 0;
            v3++;
            if (v3 <= 12 || (v3 % 1000000) == 0)
                fprintf(stderr, "[t3] call=%u hash=0x%llx table=0x%llx same=%d\n", v3,
                        (unsigned long long)p->m.cpu.x[1],
                        (unsigned long long)p->m.cpu.x[0],
                        (int)(p->m.cpu.x[1] == prev));
            prev = p->m.cpu.x[1];
        }
        if (getenv("E2_TRACE_T2") && p->m.cpu.pc == 0x10163988) {
            static unsigned v2 = 0;
            ++v2;
            if (v2 == 100 || v2 == 200000 || v2 == 2000000) {
                const uint64_t tbl = p->m.cpu.x[4];
                const unsigned lg = (unsigned)(p->m.cpu.x[6] & 0xFF);
                const unsigned n = (lg < 6) ? (1u << lg) : 64u;
                unsigned char kb[64] = {0};
                e2_memory_read(&p->m.mem, tbl + 32, kb, n);
                fprintf(stderr, "[t2] visit=%u table=0x%llx logsize=%u mask=0x%llx kindbytes:", v2,
                        (unsigned long long)tbl, lg, (unsigned long long)p->m.cpu.x[3]);
                for (unsigned k = 0; k < n; k++) fprintf(stderr, " %02x", kb[k]);
                fprintf(stderr, "\n");
            }
        }
        if (getenv("E2_TRACE_SLOTS") && p->m.cpu.pc == 0x10161074) {
            static unsigned visits = 0;
            visits++;
            if (visits == 200) {
                unsigned char k[16] = {0};
                const uint64_t base = p->m.cpu.x[25];
                e2_memory_read(&p->m.mem, base, k, 8);
                const uint64_t t = p->m.cpu.x[19] - 32;
                uint64_t freev = 0, cnt = 0, lg = 0;
                e2_memory_read(&p->m.mem, t + 16, &freev, 8);
                e2_memory_read(&p->m.mem, t + 24, &cnt, 8);
                e2_memory_read(&p->m.mem, t + 8, &lg, 1);
                fprintf(stderr, "[loop-tbl] base=0x%llx logsize=%llu free=%lld count=%lld x21=%llu x26=0x%llx bytes:",
                        (unsigned long long)base, (unsigned long long)(lg & 0xFF),
                        (long long)freev, (long long)cnt,
                        (unsigned long long)p->m.cpu.x[21],
                        (unsigned long long)p->m.cpu.x[26]);
                for (int k2 = 0; k2 < 8; k2++) fprintf(stderr, " %02x", k[k2]);
                fprintf(stderr, "\n");
            }
        }
        if (getenv("E2_TRACE_TBL") && p->m.cpu.pc == 0x101634dc) {
            static int nt2 = 0;
            if (nt2 < 20) {
                uint64_t freev = 0, cnt = 0, sz8 = 0;
                const uint64_t t = p->m.cpu.x[19];
                e2_memory_read(&p->m.mem, t + 16, &freev, 8);
                e2_memory_read(&p->m.mem, t + 24, &cnt, 8);
                e2_memory_read(&p->m.mem, t + 8, &sz8, 1);
                {
                    // Do two mappings cover the kind array? Then one guest
                    // address resolves to different host buffers depending on
                    // which region a helper happens to pick.
                    const uint64_t t0 = 0x1065c050ULL, t1 = 0x1065c058ULL;
                    unsigned hits = 0;
                    for (unsigned r2 = 0; r2 < p->m.mem.count; r2++) {
                        const e2_region *rg = &p->m.mem.regions[r2];
                        if (rg->base < t1 && t0 < rg->base + rg->size) {
                            fprintf(stderr, "[overlap] region[%u] base=0x%llx size=0x%llx prot=%d\n",
                                    r2, (unsigned long long)rg->base,
                                    (unsigned long long)rg->size, rg->prot);
                            hits++;
                        }
                    }
                    if (hits != 1)
                        fprintf(stderr, "[overlap] %u regions cover the kind array\n", hits);
                }
                unsigned char kb[8] = {0};
                e2_memory_read(&p->m.mem, p->m.cpu.x[19] + 32, kb, 8);
                fprintf(stderr, "[tbl] kindbytes:");
                for (int k3 = 0; k3 < 8; k3++) fprintf(stderr, " %02x", kb[k3]);
                fprintf(stderr, "\n");
                fprintf(stderr, "[tbl] table=0x%llx logsize=%llu free=%lld count=%lld\n",
                        (unsigned long long)t, (unsigned long long)(sz8 & 0xFF),
                        (long long)freev, (long long)cnt);
                nt2++;
            }
        }
        if (getenv("E2_TRACE_PROBE") && p->m.cpu.pc == 0x101634dc) {
            fprintf(stderr, "[code2] 0x101634b0..0x101634F0:\n");
            for (uint64_t a = 0x101634b0; a < 0x101634F0; a += 16) {
                uint32_t w[4] = {0,0,0,0};
                e2_memory_read(&p->m.mem, a, w, 16);
                fprintf(stderr, "  0x%llx: %08x %08x %08x %08x\n",
                        (unsigned long long)a, w[0], w[1], w[2], w[3]);
            }
            fprintf(stderr, "  x0=0x%llx x1=0x%llx x19=0x%llx\n",
                    (unsigned long long)p->m.cpu.x[0], (unsigned long long)p->m.cpu.x[1],
                    (unsigned long long)p->m.cpu.x[19]);
        }
        if (getenv("E2_TRACE_PROBE") &&
            (p->m.cpu.pc == 0x10161074 || p->m.cpu.pc == 0x101610b4) &&
            p->m.cpu.x[21] == 0) {
            static int nz = 0;
            if (nz < 8) {
                unsigned char c = 0;
                e2_memory_read(&p->m.mem, p->m.cpu.x[25], &c, 1);
                fprintf(stderr, "[slot0] pc=0x%llx x25=0x%llx byte=0x%02x mask=0x%llx\n",
                        (unsigned long long)p->m.cpu.pc, (unsigned long long)p->m.cpu.x[25],
                        c, (unsigned long long)p->m.cpu.x[27]);
                nz++;
            }
        }
        if (getenv("E2_TRACE_PROBE") && p->m.cpu.pc == 0x10161074) {
            static int np = 0;
            if (np == 40) {
                fprintf(stderr, "[code] 0x10161070..0x101610F0:\n");
                for (uint64_t a = 0x10161070; a < 0x101610F0; a += 16) {
                    uint32_t w[4] = {0,0,0,0};
                    e2_memory_read(&p->m.mem, a, w, 16);
                    fprintf(stderr, "  0x%llx: %08x %08x %08x %08x\n",
                            (unsigned long long)a, w[0], w[1], w[2], w[3]);
                }
            }
            if (np == 1) {
                // The probe walks this table until a byte has its sign bit set.
                // If none does, it can never stop.
                unsigned char tb[16] = {0};
                e2_memory_read(&p->m.mem, p->m.cpu.x[25], tb, 8);
                fprintf(stderr, "[probe-table] bytes:");
                for (int k = 0; k < 8; k++) fprintf(stderr, " %02x", tb[k]);
                uint64_t vals[8] = {0};
                for (int k = 0; k < 8; k++)
                    e2_memory_read(&p->m.mem, p->m.cpu.x[19] + (uint64_t)k * 16, &vals[k], 8);
                fprintf(stderr, "\n[probe-values] ");
                for (int k = 0; k < 8; k++)
                    fprintf(stderr, " %d:0x%llx", k, (unsigned long long)vals[k]);
                fprintf(stderr, "\n");
            }
            if (np < 40) {
                fprintf(stderr, "[probe] x21=0x%llx x26=0x%llx mask=0x%llx byte=0x%02x\n",
                        (unsigned long long)p->m.cpu.x[21],
                        (unsigned long long)p->m.cpu.x[26],
                        (unsigned long long)p->m.cpu.x[27],
                        (unsigned)({unsigned char c=0; e2_memory_read(&p->m.mem, p->m.cpu.x[25]+p->m.cpu.x[21], &c, 1); c;}));
                np++;
            }
        }
        if (getenv("E2_TRACE_PROGRESS") && (guard % 5000000L) == 0 && guard > 0) {
            // Sampling the pc tells "slow" from "stuck": a loop shows the same
            // handful of addresses every sample.
            fprintf(stderr, "[progress] %ld instr pc=0x%llx syscalls_so_far=%ld\n",
                    guard, (unsigned long long)p->m.cpu.pc, syscalls_seen);
        }
        static long cap = -1;
        if (cap < 0) {
            const char *e = getenv("E2_CAP");
            cap = e ? atol(e) : 60000000L;
        }
        if (++guard > cap) {
            snprintf(g_reason, sizeof(g_reason), "instruction cap");
            g_fault = 1;
            if (getenv("E2_TRACE_HIST")) {
                fprintf(stderr, "[hist] loop body:\n");
                for (unsigned k = 0; k < hist_n; k++) {
                    const unsigned hidx = (hist_at + 256 - hist_n + k) % 256;
                    fprintf(stderr, "  pc=0x%llx insn=0x%08llx x0=0x%llx x1=0x%llx x19=0x%llx x20=0x%llx x23=0x%llx\n",
                            (unsigned long long)hist[hidx].pc,
                            (unsigned long long)hist[hidx].insn,
                            (unsigned long long)hist[hidx].x0,
                            (unsigned long long)hist[hidx].x1,
                            (unsigned long long)hist[hidx].x19,
                            (unsigned long long)hist[hidx].x20,
                            (unsigned long long)hist[hidx].x23);
                }
            }
            break;
        }
        if (getenv("E2_TRACE_GREP") && p->m.cpu.pc == 0x4c4408) {
            static int ng = 0;
            if (ng == 0) {
                const uint64_t base = p->m.cpu.x[1];
                fprintf(stderr, "[recs] base=0x%llx\n", (unsigned long long)base);
                for (int k = 0; k < 10; k++) {
                    uint32_t w = 0;
                    if (e2_memory_read(&p->m.mem, base + (uint64_t)k * 56, &w, 4) != 0) {
                        fprintf(stderr, "   [+%d] unreadable\n", k * 56);
                        break;
                    }
                    fprintf(stderr, "   [+%3d] w3=0x%08x bit31=%u\n", k * 56, w, w >> 31);
                }
            }
            if (ng < 8) ng++;
        }
        if (getenv("E2_TRACE_CALLER") && p->m.cpu.pc == 0x101a61ac) {
            static int nc = 0;
            if (++nc < 12)
                fprintf(stderr, "[caller] struct=0x%llx field+8=0x%llx x0=0x%llx\n",
                        (unsigned long long)p->m.cpu.x[19],
                        (unsigned long long)p->m.cpu.x[0],
                        (unsigned long long)p->m.cpu.x[0]);
        }
        if (getenv("E2_TRACE_ENTRY") && p->m.cpu.pc == 0x101669a4) {
            static int ne = 0;
            if (++ne < 40)
                fprintf(stderr, "[entry] x0=0x%llx lr=0x%llx tpidr=0x%llx\n",
                        (unsigned long long)p->m.cpu.x[0],
                        (unsigned long long)p->m.cpu.x[30],
                        (unsigned long long)p->m.cpu.tpidr_el0);
        }
        if (getenv("E2_TRACE_WINDOW")) {
            static long wmark = -1;
            static uint64_t wbuf[400];
            static int wn = 0;
            if (wmark < 0) wmark = atol(getenv("E2_TRACE_WINDOW"));
            if (p->m.instructions >= (uint64_t)wmark && wn < 400) {
                wbuf[wn++] = p->m.cpu.pc;
                if (wn == 400) {
                    fprintf(stderr, "[window]");
                    for (int q = 0; q < 400; q++) {
                        if (q % 8 == 0) fprintf(stderr, "\n  ");
                        fprintf(stderr, " %llx", (unsigned long long)wbuf[q]);
                    }
                    fprintf(stderr, "\n");
                }
            }
        }
        if (getenv("E2_TRACE_BSS")) {
            // .bss must read as zero throughout. Watching one field over time
            // tells us whether it was never zeroed or written later.
            static const uint64_t th[5] = {10ULL, 1000ULL, 100000ULL,
                                           1000000ULL, 11500000ULL};
            static int ti = 0;
            while (ti < 5 && p->m.instructions >= th[ti]) {
                uint64_t a = 0, b = 0;
                e2_memory_read(&p->m.mem, 0x1062d028ULL, &a, 8);
                e2_memory_read(&p->m.mem, 0x1062d030ULL, &b, 8);
                fprintf(stderr, "[bss] instr=%llu +0=0x%llx +8=0x%llx\n",
                        (unsigned long long)th[ti],
                        (unsigned long long)a, (unsigned long long)b);
                ti++;
            }
        }
        if (getenv("E2_TRACE_WATCH") && p->m.instructions < 200000) {
            // Compare around a single step: whoever changed the word is the
            // instruction we just executed.
            static uint64_t last_b = 0;
            static int armed = 0;
            static uint64_t prev_pc = 0;
            static uint32_t prev_insn = 0;
            uint64_t b = 0;
            e2_memory_read(&p->m.mem, 0x1062d030ULL, &b, 8);
            if (armed && b != last_b) {
                fprintf(stderr, "[watch] instr=%llu executed_pc=0x%llx insn=0x%08x "
                                "0x%llx -> 0x%llx syscall=%llu a0=0x%llx a1=0x%llx a2=0x%llx a3=0x%llx\n",
                        (unsigned long long)p->m.instructions,
                        (unsigned long long)prev_pc, prev_insn,
                        (unsigned long long)last_b, (unsigned long long)b,
                        (unsigned long long)p->m.cpu.x[8],
                        (unsigned long long)p->m.cpu.x[0], (unsigned long long)p->m.cpu.x[1],
                        (unsigned long long)p->m.cpu.x[2], (unsigned long long)p->m.cpu.x[3]);
            }
            last_b = b;
            armed = 1;
            prev_pc = p->m.cpu.pc;
            e2_memory_read(&p->m.mem, p->m.cpu.pc, &prev_insn, 4);
        }
        if (getenv("E2_TRACE_OBJ") && p->m.cpu.pc == 0x102fbbb0) {
            static int no = 0;
            if (++no <= 3) {
                const uint64_t o = p->m.cpu.x[0];
                fprintf(stderr, "[obj] #%d object=0x%llx\n", no, (unsigned long long)o);
                for (uint64_t off = 0; off < 0x30; off += 8) {
                    uint64_t v = 0;
                    if (e2_memory_read(&p->m.mem, o + off, &v, 8) != 0) {
                        fprintf(stderr, "   +0x%02llx <unmapped>\n", (unsigned long long)off);
                        break;
                    }
                    fprintf(stderr, "   +0x%02llx = 0x%llx\n",
                            (unsigned long long)off, (unsigned long long)v);
                }
            }
        }
        if (getenv("E2_TRACE_AT") && p->m.cpu.pc == strtoull(getenv("E2_TRACE_AT"), NULL, 0)) {
            static int once = 0;
            if (!once) {
                once = 1;
                const uint64_t at = p->m.cpu.pc;
                fprintf(stderr, "[at] pc=0x%llx x0=0x%llx x1=0x%llx x2=0x%llx x9=0x%llx x10=0x%llx\n",
                        (unsigned long long)at,
                        (unsigned long long)p->m.cpu.x[0], (unsigned long long)p->m.cpu.x[1],
                        (unsigned long long)p->m.cpu.x[2], (unsigned long long)p->m.cpu.x[9],
                        (unsigned long long)p->m.cpu.x[10]);
                for (uint64_t a = at - 16; a < at + 24; a += 16) {
                    uint32_t w[4] = {0,0,0,0};
                    e2_memory_read(&p->m.mem, a, w, 16);
                    fprintf(stderr, "  0x%llx: %08x %08x %08x %08x\n",
                            (unsigned long long)a, w[0], w[1], w[2], w[3]);
                }
            }
        }
        if (getenv("E2_TRACE_ARGS") && p->m.cpu.pc == 0x1017c568) {
            {
                fprintf(stderr, "[struct] at 0x%llx:\n",
                        (unsigned long long)p->m.cpu.x[22]);
                for (uint64_t off = 0; off < 0x40; off += 8) {
                    uint64_t v = 0;
                    if (e2_memory_read(&p->m.mem, p->m.cpu.x[22] + off, &v, 8) != 0) {
                        fprintf(stderr, "  +0x%02llx <unmapped>\n",
                                (unsigned long long)off);
                        break;
                    }
                    fprintf(stderr, "  +0x%02llx = 0x%llx\n",
                            (unsigned long long)off, (unsigned long long)v);
                }
            }
            fprintf(stderr, "[code3] around 0x1017c568:\n");
            for (uint64_t a = 0x1017c530; a < 0x1017c570; a += 16) {
                uint32_t w[4] = {0,0,0,0};
                e2_memory_read(&p->m.mem, a, w, 16);
                fprintf(stderr, "  0x%llx: %08x %08x %08x %08x\n",
                        (unsigned long long)a, w[0], w[1], w[2], w[3]);
            }
            static int na = 0;
            if (++na < 6)
                fprintf(stderr, "[args] x0=0x%llx x1=0x%llx x2=0x%llx x3=0x%llx "
                                "x19=0x%llx x22=0x%llx x23=0x%llx\n",
                        (unsigned long long)p->m.cpu.x[0], (unsigned long long)p->m.cpu.x[1],
                        (unsigned long long)p->m.cpu.x[2], (unsigned long long)p->m.cpu.x[3],
                        (unsigned long long)p->m.cpu.x[19], (unsigned long long)p->m.cpu.x[22],
                        (unsigned long long)p->m.cpu.x[23]);
        }
        if (getenv("E2_TRACE_FIELD")) {
            // Watch one 8-byte word across the whole run. Answers the question
            // "was this never written, or written and then cleared" - which
            // decides whether to look at the store or at the allocation.
            static uint64_t last = 0;
            static int armed = 0;
            static uint64_t prev_pc = 0;
            const uint64_t where = strtoull(getenv("E2_TRACE_FIELD"), NULL, 0);
            uint64_t now = 0;
            if (e2_memory_read(&p->m.mem, where, &now, 8) == 0) {
                if (armed && now != last) {
                    fprintf(stderr, "[field] instr=%llu executed_pc=0x%llx 0x%llx -> 0x%llx\n",
                            (unsigned long long)p->m.instructions,
                            (unsigned long long)prev_pc,
                            (unsigned long long)last, (unsigned long long)now);
                }
                last = now;
                armed = 1;
            }
            prev_pc = p->m.cpu.pc;
        }
        if (getenv("E2_TRACE_TSTATE") && p->m.cpu.pc == 0x1027d520) {
            // _PyErr_WriteUnraisableMsg reads the pending exception from the
            // thread state, which musl keeps in TPIDR_EL0. Dump enough of it to
            // find the three exception slots.
            static int nt = 0;
            if (++nt <= 1) {
                uint64_t tls0 = 0;
                e2_memory_read(&p->m.mem, p->m.cpu.tpidr_el0, &tls0, 8);
                const uint64_t ts = tls0 ? tls0 : p->m.cpu.tpidr_el0;
                fprintf(stderr, "[tstate] tpidr=0x%llx tls[0]=0x%llx\n",
                        (unsigned long long)p->m.cpu.tpidr_el0,
                        (unsigned long long)tls0);
                {
                    // +0x60 held a ValueError. Read its args tuple and the first
                    // element, which is where the message lives.
                    const uint64_t exc = p->m.cpu.x[0] ? 0 : 0;
                    (void)exc;
                    uint64_t args = 0;
                    e2_memory_read(&p->m.mem, ts + 0x60, &args, 8);
                    uint64_t excobj = args;
                    fprintf(stderr, "[tstate] exc=0x%llx fields:\n",
                            (unsigned long long)excobj);
                    for (uint64_t eo = 0; eo < 0x48; eo += 8) {
                        uint64_t ev = 0;
                        e2_memory_read(&p->m.mem, excobj + eo, &ev, 8);
                        fprintf(stderr, "[tstate]    +0x%02llx = 0x%llx\n",
                                (unsigned long long)eo, (unsigned long long)ev);
                    }
                    uint64_t atuple = 0;
                    e2_memory_read(&p->m.mem, excobj + 0x18, &atuple, 8);
                    fprintf(stderr, "[tstate] args tuple=0x%llx\n",
                            (unsigned long long)atuple);
                    if (atuple != 0) {
                        for (uint64_t to = 0; to < 0x28; to += 8) {
                            uint64_t tv = 0;
                            e2_memory_read(&p->m.mem, atuple + to, &tv, 8);
                            fprintf(stderr, "[tstate]    tuple+0x%02llx = 0x%llx\n",
                                    (unsigned long long)to, (unsigned long long)tv);
                        }
                        uint64_t item0 = 0;
                        e2_memory_read(&p->m.mem, atuple + 0x18, &item0, 8);
                        if (item0 != 0) {
                            fprintf(stderr, "[tstate] item0=0x%llx fields:\n",
                                    (unsigned long long)item0);
                            for (uint64_t io = 0; io < 0x30; io += 8) {
                                uint64_t iv = 0;
                                e2_memory_read(&p->m.mem, item0 + io, &iv, 8);
                                fprintf(stderr, "[tstate]    item+0x%02llx = 0x%llx\n",
                                        (unsigned long long)io, (unsigned long long)iv);
                            }
                            // PyASCIIObject: length at +0x10, hash at +0x18,
                            // the state word at +0x20, and for a compact ASCII
                            // string the characters start at +0x28, one byte
                            // each. Reading from +0x20 with a 4-byte stride
                            // produced scramble the first time.
                            uint64_t hdr = 0;
                            e2_memory_read(&p->m.mem, item0 + 0x10, &hdr, 8);
                            const unsigned nchars = (unsigned)(hdr & 0xFFFFFFFF);
                            char text[256] = {0};
                            e2_memory_read(&p->m.mem, item0 + 0x28, text, sizeof(text) - 1);
                            // The traceback carries the line number, which maps
                            // straight onto CPython's frozen getpath.py.
                            uint64_t tb = 0;
                            e2_memory_read(&p->m.mem, excobj + 0x28, &tb, 8);
                            fprintf(stderr, "[tstate] traceback=0x%llx\n",
                                    (unsigned long long)tb);
                            for (int depth = 0; depth < 6 && tb != 0; depth++) {
                                uint64_t nxt = 0, frame = 0, ln = 0;
                                e2_memory_read(&p->m.mem, tb + 0x10, &nxt, 8);
                                e2_memory_read(&p->m.mem, tb + 0x18, &frame, 8);
                                e2_memory_read(&p->m.mem, tb + 0x20, &ln, 8);
                                fprintf(stderr,
                                        "[tstate]   tb[%d] lineno=%u frame=0x%llx\n",
                                        depth, (unsigned)((ln >> 32) & 0xFFFFFFFF),
                                        (unsigned long long)frame);
                                // Walk the frame looking for anything shaped like
                                // a str and print it: co_name and co_filename
                                // are the fastest way to name the code that
                                // raised, and the line table is unavailable.
                                if (depth == 0 && frame != 0) {
                                    for (uint64_t fo = 0; fo < 0x60; fo += 8) {
                                        uint64_t cand = 0;
                                        e2_memory_read(&p->m.mem, frame + fo, &cand, 8);
                                        if (cand < 0x10000000ULL || cand > 0x11000000ULL)
                                            continue;
                                        uint64_t hdr = 0;
                                        if (e2_memory_read(&p->m.mem, cand + 0x10,
                                                           &hdr, 8) != 0) continue;
                                        const unsigned clen = (unsigned)(hdr & 0xFFFFFFFF);
                                        if (clen == 0 || clen > 120) continue;
                                        char ctext[128] = {0};
                                        if (e2_memory_read(&p->m.mem, cand + 0x28, ctext,
                                                           sizeof(ctext) - 1) != 0) continue;
                                        int ok2 = 1;
                                        for (unsigned q = 0; q < clen; q++)
                                            if (ctext[q] < 0x20 || ctext[q] > 0x7e) { ok2 = 0; break; }
                                        if (ok2)
                                            fprintf(stderr,
                                                    "[tstate]     frame+0x%02llx -> \"%.*s\"\n",
                                                    (unsigned long long)fo, (int)clen, ctext);
                                    }
                                }
                                tb = nxt;
                            }
                            fprintf(stderr, "[tstate] message (%u chars): \"", nchars);
                            for (unsigned q = 0; q < nchars && q < 200; q++)
                                fputc((text[q] >= 0x20 && text[q] < 0x7f) ? text[q] : '?', stderr);
                            fprintf(stderr, "\"\n");
                            char msg[160] = {0};
                            if (e2_memory_read(&p->m.mem, item0 + 0x20, msg,
                                               sizeof(msg) - 1) == 0) {
                                int printable = 1;
                                for (int q = 0; q < 120 && msg[q]; q++)
                                    if (msg[q] < 0x20 || msg[q] > 0x7e) { printable = 0; break; }
                                fprintf(stderr, "[tstate] ValueError args[0]%s: \"%s\"\n",
                                        printable ? "" : " (not text)", msg);
                            }
                        }
                    }
                }
                for (uint64_t off = 0; off < 0x200; off += 8) {
                    uint64_t v = 0;
                    if (e2_memory_read(&p->m.mem, ts + off, &v, 8) != 0) break;
                    if (v == 0) continue;
                    char note[80] = "";
                    uint64_t ty = 0;
                    if (e2_memory_read(&p->m.mem, v + 8, &ty, 8) == 0 && ty != 0) {
                        uint64_t np = 0;
                        if (e2_memory_read(&p->m.mem, ty + 0x18, &np, 8) == 0 && np != 0) {
                            char text[48] = {0};
                            if (e2_memory_read(&p->m.mem, np, text, sizeof(text) - 1) == 0) {
                                int ok = 1;
                                for (int q = 0; q < 30 && text[q]; q++)
                                    if (text[q] < 0x20 || text[q] > 0x7e) { ok = 0; break; }
                                if (ok && text[0]) snprintf(note, sizeof(note), "  obj type=\"%s\"", text);
                            }
                        }
                    }
                    fprintf(stderr, "[tstate]   +0x%02llx = 0x%llx%s\n",
                            (unsigned long long)off, (unsigned long long)v, note);
                }
            }
        }
        if (getenv("E2_TRACE_EXC") && p->m.cpu.pc == 0x10203524) {
            static int ne = 0;
            if (++ne <= 2) {
                const uint64_t obj = p->m.cpu.x[20];
                fprintf(stderr, "[exc] x20=0x%llx\n", (unsigned long long)obj);
                uint64_t v[6] = {0};
                for (int q = 0; q < 6; q++)
                    e2_memory_read(&p->m.mem, obj + (uint64_t)q * 8, &v[q], 8);
                fprintf(stderr, "[exc]   refcnt=0x%llx type=0x%llx\n",
                        (unsigned long long)v[0], (unsigned long long)v[1]);
                // A PyTypeObject carries tp_name at a fixed offset in 3.12; try
                // the common ones and print whatever looks like a string.
                for (int off = 0x10; off <= 0x30; off += 8) {
                    uint64_t nameptr = 0;
                    if (e2_memory_read(&p->m.mem, v[1] + (uint64_t)off, &nameptr, 8) != 0)
                        continue;
                    char text[64] = {0};
                    if (e2_memory_read(&p->m.mem, nameptr, text, sizeof(text) - 1) == 0) {
                        int printable = 1;
                        for (int q = 0; q < 32 && text[q]; q++)
                            if (text[q] < 0x20 || text[q] > 0x7e) { printable = 0; break; }
                        if (printable && text[0])
                            fprintf(stderr, "[exc]   type+0x%x -> \"%s\"\n", off, text);
                    }
                }
            }
        }
        if (getenv("E2_TRACE_NUL") && p->m.cpu.pc == 0x1012e358) {
            // Right before PyErr_SetString(ValueError, "embedded null
            // character"): x19 is the UTF-8 path buffer and [sp] the length the
            // caller believes it has, so the mismatch is visible here.
            static int nn = 0;
            if (++nn <= 3) {
                char text[256] = {0};
                e2_memory_read(&p->m.mem, p->m.cpu.x[19], text, sizeof(text) - 1);
                uint64_t claimed = 0;
                e2_memory_read(&p->m.mem, p->m.cpu.sp, &claimed, 8);
                fprintf(stderr, "[nul] buffer=0x%llx claimed_len=%llu text=\"",
                        (unsigned long long)p->m.cpu.x[19], (unsigned long long)claimed);
                for (int q = 0; q < 200 && text[q]; q++)
                    fputc((text[q] >= 0x20 && text[q] < 0x7f) ? text[q] : '?', stderr);
                fprintf(stderr, "\"\n");
            }
        }
        if (getenv("E2_TRACE_PCS") && g_trace_recording != 2) {
            // Record the instruction path from a chosen address so it can be
            // compared with the same window taken from qemu.
            const uint64_t from = strtoull(getenv("E2_TRACE_PCS"), NULL, 0);
            if (g_trace_recording == 0 && p->m.cpu.pc == from) {
                g_trace_recording = 1;
            }
            if (g_trace_recording == 1) {
                static int limit = -1;
                if (limit < 0) {
                    const char *ls = getenv("E2_TRACE_PCS_LIMIT");
                    limit = ls ? atoi(ls) : 512;
                    if (limit <= 0 || limit > (int)(sizeof(g_trace_buf) / sizeof(g_trace_buf[0]))) {
                        limit = (int)(sizeof(g_trace_buf) / sizeof(g_trace_buf[0]));
                    }
                }
                const char *watch = getenv("E2_TRACE_AT2");
                if (watch && p->m.cpu.pc == strtoull(watch, NULL, 0)) {
                    char ta[32] = {0}, tb[32] = {0};
                    e2_memory_read(&p->m.mem, p->m.cpu.x[0], ta, sizeof(ta) - 1);
                    e2_memory_read(&p->m.mem, p->m.cpu.x[1], tb, sizeof(tb) - 1);
                    for (int q = 0; q < 24; q++) {
                        if (ta[q] && (ta[q] < 0x20 || ta[q] > 0x7e)) ta[q] = '.';
                        if (tb[q] && (tb[q] < 0x20 || tb[q] > 0x7e)) tb[q] = '.';
                    }
                    fprintf(stderr,
                            "[at2] idx=%d x0=\"%s\" x1=\"%s\" x2=0x%llx x3=0x%llx "
                            "x4=0x%llx x5=0x%llx x6=0x%llx x7=0x%llx\n",
                            g_trace_n, ta, tb,
                            (unsigned long long)p->m.cpu.x[2],
                            (unsigned long long)p->m.cpu.x[3],
                            (unsigned long long)p->m.cpu.x[4],
                            (unsigned long long)p->m.cpu.x[5],
                            (unsigned long long)p->m.cpu.x[6],
                            (unsigned long long)p->m.cpu.x[7]);
                }
                if (g_trace_n < limit) {
                    g_trace_buf[g_trace_n++] = p->m.cpu.pc;
                }
                if (g_trace_n == limit) {
                    trace_flush();
                }
            }
        }
        if (getenv("E2_TRACE_CALLS")) {
            // Log every taken bl with caller and target. A call trace answers
            // "which functions actually run", which a PC trace only implies.
            const uint32_t w = 0;
            (void)w;
            uint32_t insn_word = 0;
            if (e2_memory_read(&p->m.mem, p->m.cpu.pc, &insn_word, 4) == 0 &&
                (insn_word & 0xFC000000u) == 0x94000000u) {
                int32_t imm = (int32_t)(insn_word & 0x3FFFFFFu);
                if (imm & (1 << 25)) imm -= 1 << 26;
                fprintf(stderr, "[call] 0x%llx -> 0x%llx\n",
                        (unsigned long long)p->m.cpu.pc,
                        (unsigned long long)(p->m.cpu.pc + (int64_t)imm * 4));
            }
        }
        if (getenv("E2_TRACE_RING")) {
            // Ring of the last few (pc, d0) pairs. Chasing a value that changes
            // inside a call is hopeless by watching a fixed window, because the
            // change happens in code the window does not cover.
            static struct { uint64_t pc, d0; } ringbuf[512];
            static unsigned ringpos = 0;
            if (ringpos == 0 || p->m.cpu.v[0][0] != ringbuf[(ringpos - 1) & 511].d0) {
                ringbuf[ringpos & 511].pc = p->m.cpu.pc;
                ringbuf[ringpos & 511].d0 = p->m.cpu.v[0][0];
                ringpos++;
            }
            if (p->m.cpu.pc == 0x4944b8) {
                for (unsigned k = 1; k <= 250; k++) {
                    const unsigned idx = (ringpos - 251 + k) & 511;
                    fprintf(stderr, "[ring] %2u pc=0x%llx d0=0x%llx\n", k,
                            (unsigned long long)ringbuf[idx].pc,
                            (unsigned long long)ringbuf[idx].d0);
                }
                exit(0);
            }
        }
        const uint64_t pre_pc = p->m.cpu.pc;
        // Shadow call stack. Chasing a bad argument by re-running with a probe
        // one frame up, over and over, was the slowest part of the last few
        // rounds; a rough stack is enough to see who called whom in one run.
        uint32_t pre_insn = 0;
        if (g_shadow_on) {
            if (e2_memory_read(&p->m.mem, pre_pc, &pre_insn, 4) != 0) pre_insn = 0;
        }
        g_global_step++;
        e2_status st = e2_step(&p->m);
        // A chdir inside the step changed env.cwd; keep the process copy in step so
        // the change survives to the next scheduling turn.
        if (strcmp(p->pc.cwd, env.cwd) != 0) {
            snprintf(p->pc.cwd, sizeof(p->pc.cwd), "%s", env.cwd);
        }
        if (strcmp(g_cur_cwd, p->pc.cwd) != 0) {
            snprintf(g_cur_cwd, sizeof(g_cur_cwd), "%s", p->pc.cwd);
        }
        if (g_shadow_on) {
            const uint32_t w = pre_insn;
            const bool is_bl = (w & 0xFC000000u) == 0x94000000u;
            const bool is_blr = (w & 0xFFFFFC1Fu) == 0xD63F0000u;
            const bool is_ret = (w & 0xFFFFFC1Fu) == 0xD65F0000u && ((w >> 5) & 0x1F) == 30;
            if (is_bl || is_blr) {
                if (g_shadow_n < 48) {
                    g_shadow[g_shadow_n].call_pc = pre_pc;
                    g_shadow[g_shadow_n].ret_pc = pre_pc + 4;
                    g_shadow[g_shadow_n].x0 = p->m.cpu.x[0];
                    g_shadow_n++;
                }
            } else if (is_ret && g_shadow_n > 0) {
                g_shadow_n--;
            }
        }
        if (getenv("E2_TRACE_AFTER") && pre_pc == 0x50013e94) {
            unsigned char k[8] = {0};
            e2_memory_read(&p->m.mem, 0x1065c050ULL, k, 8);
            fprintf(stderr, "[after-memset]:");
            for (int q = 0; q < 8; q++) fprintf(stderr, " %02x", k[q]);
            fprintf(stderr, "\n");
        }
        if (st == E2_SYSCALL) {
            const uint64_t pn = p->m.cpu.x[8];
            if (getenv("E2_TRACE_PIPE") && (pn == 23 || pn == 24 || pn == 59 || pn == 220 || pn == 260 || pn == 221))
                fprintf(stderr, "[p%d] sys %llu a0=%llu a1=%llu a2=%llu\n", p->pid,
                        (unsigned long long)pn, (unsigned long long)p->m.cpu.x[0],
                        (unsigned long long)p->m.cpu.x[1], (unsigned long long)p->m.cpu.x[2]);
            syscalls_seen++;
            if (getenv("E2_TRACE_MAP") && (p->m.cpu.x[8] == 215 || p->m.cpu.x[8] == 222)) {
                const uint64_t a0 = p->m.cpu.x[0], a1 = p->m.cpu.x[1];
                if (p->m.cpu.x[8] == 215) {
                    fprintf(stderr, "[munmap] 0x%llx len=0x%llx\n",
                            (unsigned long long)a0, (unsigned long long)a1);
                } else {
                    fprintf(stderr, "[mmap] hint=0x%llx len=0x%llx prot=%llu\n",
                            (unsigned long long)a0, (unsigned long long)a1,
                            (unsigned long long)p->m.cpu.x[2]);
                }
            }
            // Arguments have to be read before the call: x0 holds the result after.
            const uint64_t s_a0 = p->m.cpu.x[0];
            const uint64_t s_a1 = p->m.cpu.x[1];
            const uint64_t s_a2 = p->m.cpu.x[2];
            st = e2_service_syscall(&p->m, &env);
            const long long s_ret = (long long)p->m.cpu.x[0];
            if (getenv("E2_TRACE_ALL"))
                fprintf(stderr, "[p%d] sys %llu a0=0x%llx a1=0x%llx a2=0x%llx -> %lld\n", p->pid,
                        (unsigned long long)pn, (unsigned long long)s_a0,
                        (unsigned long long)s_a1, (unsigned long long)s_a2, s_ret);
            if (getenv("E2_TRACE_RET") && s_ret < 0)
                fprintf(stderr, "[p%d] sys %llu a0=0x%llx a1=0x%llx a2=0x%llx -> %lld\n", p->pid,
                        (unsigned long long)pn, (unsigned long long)s_a0,
                        (unsigned long long)s_a1, (unsigned long long)s_a2, s_ret);
            if (st == E2_HALTED) { p->alive = 0; break; }
            if (st != E2_OK) {
                {
                    uint32_t word = 0;
                    e2_memory_read(&p->m.mem, p->m.cpu.pc, &word, 4);
                    // Name the region the faulting pc lives in. Without it a
                    // pc inside a shared library is just a number.
                    {
                        const uint64_t fpc = p->m.cpu.pc;
                        for (unsigned k = 0; k < p->m.mem.count; k++) {
                            const e2_region *rg = &p->m.mem.regions[k];
                            if (fpc >= rg->base && fpc < rg->base + rg->size) {
                                snprintf(g_reason, sizeof(g_reason), "[pc in region base=0x%llx size=0x%llx] ",
                                         (unsigned long long)rg->base, (unsigned long long)rg->size);
                                break;
                            }
                        }
                    }
                    snprintf(g_reason + strlen(g_reason), sizeof(g_reason) - strlen(g_reason),
                             "%s pid=%d pc=0x%llx insn=0x%08x",
                             p->m.ctx.reason ? p->m.ctx.reason : "?", p->pid,
                             (unsigned long long)p->m.cpu.pc, word);
                }
                g_fault = 1;
                if (getenv("E2_TRACE_HIST")) {
                    fprintf(stderr, "[hist] last %u steps before 0x%llx:\n", hist_n,
                            (unsigned long long)p->m.cpu.pc);
                    for (unsigned k = 0; k < hist_n; k++) {
                        const unsigned idx = (hist_at + 256 - hist_n + k) % 256;
                        fprintf(stderr, "  pc=0x%llx insn=0x%08llx x0=0x%llx x1=0x%llx x19=0x%llx x20=0x%llx x23=0x%llx\n",
                                (unsigned long long)hist[idx].pc,
                                (unsigned long long)hist[idx].insn,
                                (unsigned long long)hist[idx].x0,
                                (unsigned long long)hist[idx].x1,
                                (unsigned long long)hist[idx].x19,
                                (unsigned long long)hist[idx].x20,
                                (unsigned long long)hist[idx].x23);
                    }
                }
                if (getenv("E2_TRACE_GREP")) {
                    fprintf(stderr, "[fault] pid=%d regions=%u\n", p->pid, p->m.mem.count);
                    for (unsigned k = 0; k < p->m.mem.count; k++)
                        fprintf(stderr, "   [%u] 0x%llx + 0x%llx\n", k,
                                (unsigned long long)p->m.mem.regions[k].base,
                                (unsigned long long)p->m.mem.regions[k].size);
                }
                break;
            }
            continue;
        }
        if (st != E2_OK) {
            snprintf(g_reason, sizeof(g_reason),
                     "%s addr=0x%llx pid=%d pc=0x%llx insn=0x%08x x0=0x%llx x1=0x%llx x2=0x%llx x19=0x%llx x21=0x%llx x30=0x%llx sp=0x%llx",
                     p->m.ctx.reason ? p->m.ctx.reason : "?",
                     (unsigned long long)p->m.ctx.fault_addr, p->pid,
                     (unsigned long long)p->m.cpu.pc, p->m.ctx.instruction,
                     (unsigned long long)p->m.cpu.x[0], (unsigned long long)p->m.cpu.x[1],
                     (unsigned long long)p->m.cpu.x[2],
                     (unsigned long long)p->m.cpu.x[19],
                     (unsigned long long)p->m.cpu.x[21],
                     (unsigned long long)p->m.cpu.x[30],
                     (unsigned long long)p->m.cpu.sp);
            g_fault = 1;
            if (getenv("E2_TRACE_STACK")) dump_shadow("fault");
            if (getenv("E2_TRACE_OBJFAULT")) {
                const uint64_t o = p->m.cpu.x[19];
                fprintf(stderr, "[objfault] struct at 0x%llx:\n", (unsigned long long)o);
                for (uint64_t off = 0; off < 0x40; off += 8) {
                    uint64_t v = 0;
                    if (e2_memory_read(&p->m.mem, o + off, &v, 8) != 0) {
                        fprintf(stderr, "   +0x%02llx <unmapped>\n", (unsigned long long)off);
                        break;
                    }
                    fprintf(stderr, "   +0x%02llx = 0x%llx\n",
                            (unsigned long long)off, (unsigned long long)v);
                }
            }
            if (getenv("E2_TRACE_REGIONS")) {
                fprintf(stderr, "[regions] %u in use (max %d)\n",
                        p->m.mem.count, E2_MAX_REGIONS);
                for (unsigned k = 0; k < p->m.mem.count; k++) {
                    const e2_region *rg = &p->m.mem.regions[k];
                    const uint64_t lo = rg->base, hi = rg->base + rg->size;
                    fprintf(stderr, "[region] 0x%llx..0x%llx prot=%d%s\n",
                            (unsigned long long)lo, (unsigned long long)hi, rg->prot,
                            (lo <= 0x10006000ULL && 0x10006000ULL < hi) ? "  <-- covers fault" : "");
                }
            }
            if (getenv("E2_TRACE_CODE")) {
                // The binary on disk and the image in memory are not always at
                // the same offsets, so disassembling the file can mislead. Dump
                // what is actually executing.
                const uint64_t at = p->m.cpu.pc;
                const char *range_env = getenv("E2_CODE_BACK");
                const uint64_t back = range_env ? strtoull(range_env, NULL, 0)
                                                : (at > 48 ? 48 : 0);
                const uint64_t from = (at > back) ? at - back : 0;
                fprintf(stderr, "[code] around pc=0x%llx:\n", (unsigned long long)at);
                for (uint64_t a = from; a < at + 48; a += 16) {
                    uint32_t w[4] = {0, 0, 0, 0};
                    e2_memory_read(&p->m.mem, a, w, 16);
                    fprintf(stderr, "  0x%llx: %08x %08x %08x %08x\n",
                            (unsigned long long)a, w[0], w[1], w[2], w[3]);
                }
            }
            if (getenv("E2_TRACE_TABLE")) {
                // Summarise the array the faulting code indexed: a table that is
                // entirely zero was never filled, whereas a sparse one means a
                // specific slot is missing.
                const uint64_t base = p->m.cpu.x[19];
                unsigned nonzero = 0, total = 0;
                for (uint64_t off = 0; off < 4096; off += 8) {
                    uint64_t w = 0;
                    if (e2_memory_read(&p->m.mem, base + off, &w, 8) != 0) break;
                    total++;
                    if (w) nonzero++;
                }
                fprintf(stderr, "[table] base=0x%llx words=%u nonzero=%u\n",
                        (unsigned long long)base, total, nonzero);
            }
            if (getenv("E2_TRACE_HIST")) {
                fprintf(stderr, "[hist] last %u steps:\n", hist_n);
                for (unsigned k = 0; k < hist_n; k++) {
                    const unsigned hidx = (hist_at + 256 - hist_n + k) % 256;
                    fprintf(stderr, "  pc=0x%llx insn=0x%08llx x0=0x%llx x1=0x%llx x19=0x%llx x20=0x%llx x23=0x%llx\n",
                            (unsigned long long)hist[hidx].pc,
                            (unsigned long long)hist[hidx].insn,
                            (unsigned long long)hist[hidx].x0,
                            (unsigned long long)hist[hidx].x1,
                            (unsigned long long)hist[hidx].x19,
                            (unsigned long long)hist[hidx].x20,
                            (unsigned long long)hist[hidx].x23);
                }
            }
            if (getenv("E2_TRACE_GREP")) {
                fprintf(stderr, "[fault] pid=%d regions=%u\n", p->pid, p->m.mem.count);
                for (unsigned k = 0; k < p->m.mem.count; k++)
                    fprintf(stderr, "   [%u] 0x%llx + 0x%llx\n", k,
                            (unsigned long long)p->m.mem.regions[k].base,
                            (unsigned long long)p->m.mem.regions[k].size);
            }
            break;
        }
    }
    // A run that faults never reaches p_exit, so a partial recording would be
    // lost exactly when it is most wanted - the trace up to the fault.
    trace_flush();
    return p->exit_status;
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "/tmp/bbs/bin/busybox.static";
    g_self_path = path;
    fd_init();
    // Populate the guest's /proc once, as a kernel would. The root comes from the
    // same place host_path uses, so E2_ROOT still applies.
    {
        const char *root = getenv("E2_ROOT");
        seed_proc(root != NULL ? root : "/tmp/alpine-root");
    }
    e2_machine m;
    e2_machine_init(&m, 0, 1 << 20);

    char args[16][ARG_MAX]; int nargs = 0;
    // argv[0] must be the program's own name: busybox picks an applet from it and
    // python prints it in usage text, so a hardcoded name lies to both.
    {
        const char *base = strrchr(path, '/');
        const char *override = getenv("E2_ARGV0");
        snprintf(args[nargs++], sizeof(args[0]), "%s",
                 override ? override : (base ? base + 1 : path));
    }
    for (int i = 2; i < argc && nargs < 16; i++)
        snprintf(args[nargs++], sizeof(args[0]), "%s", argv[i]);
    char envs[64][ARG_MAX]; int nenv = 0;
    // E2_MIN_ENV=1 passes no environment at all. If an applet behaves
    // differently without it, the cause is the stack layout or environ rather
    // than the code being exercised.
    if (getenv("E2_MIN_ENV") != NULL) {
        goto env_done;
    }
    snprintf(envs[nenv++], sizeof(envs[0]), "PATH=/bin:/sbin:/usr/bin:/usr/sbin");
    snprintf(envs[nenv++], sizeof(envs[0]), "HOME=/root");
    snprintf(envs[nenv++], sizeof(envs[0]), "TERM=xterm");
    snprintf(envs[nenv++], sizeof(envs[0]), "PWD=/");
    // E2_NO_LANG exists to test whether a failing applet is taking a locale
    // path: several text tools consult LANG before compiling a pattern.
    if (getenv("E2_NO_LANG") == NULL) {
        snprintf(envs[nenv++], sizeof(envs[0]), "LANG=C");
    }
    // Pass Python's own knobs through, so the guest can be pointed at a
    // rootfs without rebuilding the harness.
    {
        static const char *passthrough[] = {"PYTHONHOME", "PYTHONPATH", "PYTHONNOUSERSITE"};
        for (unsigned i = 0; i < sizeof(passthrough) / sizeof(passthrough[0]); i++) {
            const char *v = getenv(passthrough[i]);
            if (v != NULL && nenv < 64) {
                snprintf(envs[nenv], sizeof(envs[0]), "%s=%s", passthrough[i], v);
                nenv++;
            }
        }
    }

env_done:
    if (start_image(&m, path, args, nargs, envs, nenv) != 0) {
        fprintf(stderr, "cannot load %s\n", path);
        return 1;
    }

    // Hand the loaded image to the process table and run it as pid 1.
    procs[1].used = 1;
    fd_table_copy(1, 0);
    procs[1].pc.fdtab = 1;
    procs[1].pid = 1;
    procs[1].ppid = 0;
    procs[1].alive = 1;
    procs[1].m = m;
    g_shadow_on = getenv("E2_TRACE_STACK") != NULL;
    const int status = run_proc(1);
    (void)status;
    if (getenv("E2_TRACE_WAITSUM"))
        fprintf(stderr, "[waitsum] calls=%d ok=%d echild=%d\n",
                g_wait_calls, g_wait_ok, g_wait_echild);
    if (getenv("E2_TRACE_WAITSUM"))
        fprintf(stderr, "[waitsum] calls=%d ok=%d echild=%d\n",
                g_wait_calls, g_wait_ok, g_wait_echild);
    printf("\n[exit %d] instructions=%ld%s%s\n", procs[1].exit_status,
           (long)procs[1].m.instructions,
           g_fault ? " FAULT reason=" : "", g_fault ? g_reason : "");
    return 0;
}
