// Tests for the e2 interpreter core.
//
// Everything here runs on the host: the point of keeping the interpreter
// platform-independent is that instruction bugs get caught in seconds instead of
// waiting for a simulator build.

#include <stdio.h>
#include <string.h>

#include "e2/exec.h"
#include "e2/loader.h"
#include "e2/syscall.h"

#include <stdlib.h>

// --- Encoders -----------------------------------------------------------------
// Hand-assembled from the AArch64 encoding tables, so the tests do not depend on
// an external assembler being installed.

#define MOVZ_X(rd, imm16) (0xD2800000u | ((uint32_t)(imm16) << 5) | (rd))
#define MOVZ_W(rd, imm16) (0x52800000u | ((uint32_t)(imm16) << 5) | (rd))
#define MOVN_X(rd, imm16) (0x92800000u | ((uint32_t)(imm16) << 5) | (rd))
#define MOVK_X(rd, imm16, shift) \
    (0xF2800000u | (((uint32_t)(shift) / 16) << 21) | ((uint32_t)(imm16) << 5) | (rd))

#define ADD_XI(rd, rn, imm) (0x91000000u | ((uint32_t)(imm) << 10) | ((rn) << 5) | (rd))
#define SUB_XI(rd, rn, imm) (0xD1000000u | ((uint32_t)(imm) << 10) | ((rn) << 5) | (rd))
#define SUBS_XI(rd, rn, imm) (0xF1000000u | ((uint32_t)(imm) << 10) | ((rn) << 5) | (rd))
#define ADD_WI(rd, rn, imm) (0x11000000u | ((uint32_t)(imm) << 10) | ((rn) << 5) | (rd))

#define ADD_XR(rd, rn, rm) (0x8B000000u | ((rm) << 16) | ((rn) << 5) | (rd))
#define SUB_XR(rd, rn, rm) (0xCB000000u | ((rm) << 16) | ((rn) << 5) | (rd))

#define RET() 0xD65F03C0u
#define NOP() 0xD503201Fu
#define SVC(n) (0xD4000001u | ((uint32_t)(n) << 5))

// imm19 is the branch offset in instructions.
#define B(offset_insns) (0x14000000u | ((uint32_t)(offset_insns) & 0x3FFFFFFu))
#define B_COND(cond, offset_insns) \
    (0x54000000u | (((uint32_t)(offset_insns) & 0x7FFFFu) << 5) | (cond))
#define CBZ_X(rt, offset_insns) \
    (0xB4000000u | (((uint32_t)(offset_insns) & 0x7FFFFu) << 5) | (rt))

#define STR_X(rt, rn, off) (0xF9000000u | (((uint32_t)(off) / 8) << 10) | ((rn) << 5) | (rt))
#define LDR_X(rt, rn, off) (0xF9400000u | (((uint32_t)(off) / 8) << 10) | ((rn) << 5) | (rt))
#define STR_W(rt, rn, off) (0xB9000000u | (((uint32_t)(off) / 4) << 10) | ((rn) << 5) | (rt))
#define LDR_W(rt, rn, off) (0xB9400000u | (((uint32_t)(off) / 4) << 10) | ((rn) << 5) | (rt))

// Conditions
#define COND_EQ 0u
#define COND_NE 1u
#define COND_LT 11u
#define COND_GT 12u

// --- Program runner with syscall servicing ------------------------------------

typedef struct {
    char out[512];
    size_t out_len;
    int exited;
    int status;
    int syscall_count;
} capture;

/// Memory-backed stand-in for the host file layer. Output goes to the capture
/// buffer; input files are not needed by these tests yet.
static long test_file_write(void *ctx, int fd, const void *buf, size_t len, long long offset) {
    (void)fd; (void)offset;
    capture *cap = (capture *)ctx;
    if (len > sizeof(cap->out) - 1 - cap->out_len) {
        len = sizeof(cap->out) - 1 - cap->out_len;
    }
    memcpy(cap->out + cap->out_len, buf, len);
    cap->out_len += len;
    cap->out[cap->out_len] = 0;
    cap->syscall_count++;
    return (long)len;
}
static long test_file_read(void *ctx, int fd, void *buf, size_t len, long long offset) {
    (void)ctx; (void)fd; (void)buf; (void)len; (void)offset;
    return 0;
}
static int test_file_open(void *ctx, const char *path, int flags, int mode) {
    (void)ctx; (void)path; (void)flags; (void)mode;
    return -2;  // ENOENT
}
static int test_file_close(void *ctx, int fd) { (void)ctx; (void)fd; return 0; }
// Only /bin exists in this mock. chdir asks before accepting a path, so a host that
// answers "yes" to everything makes `cd` report success for directories that are not
// there - which is the thing worth testing.
// A path that only resolves without following: the shape of a dangling symlink. lstat
// succeeds, stat does not. This is what rm needs before it will unlink a name.
// Records how truncate was asked to work: descriptor form passes a path of NULL,
// path form passes -1 as the descriptor. Getting the two mixed up is the mistake this
// mock exists to catch.
static int g_trunc_fd;
static char g_trunc_path[64];
static long long g_trunc_len;
// Fills the guest layout with recognisable numbers so the test can check that each
// field lands at the offset the aarch64 ABI specifies, not merely that a buffer was
// written.
static int g_statfs_fd;
static char g_statfs_path[64];
static int test_statfs(void *ctx, int fd, const char *path, void *out, size_t outlen) {
    (void)ctx;
    g_statfs_fd = fd;
    snprintf(g_statfs_path, sizeof(g_statfs_path), "%s", path != NULL ? path : "");
    if (fd >= 0 && fd != 3) return -9;
    if (path != NULL && strcmp(path, "/bin") != 0) return -2;
    if (outlen < 128) return -22;
    uint64_t *w = (uint64_t *)out;
    memset(out, 0, 128);
    w[0] = 0xEF53;   // f_type
    w[1] = 4096;     // f_bsize
    w[2] = 1000;     // f_blocks
    w[8] = 255;      // f_namelen
    w[9] = 4096;     // f_frsize
    return 0;
}
static int test_truncate(void *ctx, int fd, const char *path, long long length) {
    (void)ctx;
    g_trunc_fd = fd;
    snprintf(g_trunc_path, sizeof(g_trunc_path), "%s", path != NULL ? path : "");
    g_trunc_len = length;
    if (fd >= 0 && fd != 3) return -9;          // EBADF for an unexpected descriptor
    if (path != NULL && strcmp(path, "/bin") != 0) return -2;  // ENOENT
    return 0;
}
static int test_stat_path(void *ctx, const char *path, void *out, size_t outlen, int follow) {
    (void)ctx;
    if (outlen < 128) return -22;
    if (strcmp(path, "/dangling") == 0 && follow) return -2;  // ENOENT
    if (strcmp(path, "/dangling") != 0 && strcmp(path, "/bin") != 0) return -2;
    memset(out, 0, 128);
    *(uint32_t *)((uint8_t *)out + 16) = 0120777;  // S_IFLNK | 0777
    return 0;
}
static int test_opendir(void *ctx, const char *path) {
    (void)ctx;
    if (strcmp(path, "/bin") == 0 || strcmp(path, "/") == 0) return 3;
    return -2;  // ENOENT
}
// A seek that keeps a position, so lseek can be exercised. Without it every lseek
// returned ENOSYS here and the syscall was tested only by the differential suite.
static long long g_test_pos;
static long test_file_seek(void *ctx, int fd, long long offset, int whence) {
    (void)ctx; (void)fd;
    switch (whence) {
        case 0: g_test_pos = offset; break;          // SEEK_SET
        case 1: g_test_pos += offset; break;         // SEEK_CUR
        case 2: g_test_pos = 1000 + offset; break;   // SEEK_END against a fixed size
        default: return -22;                         // EINVAL
    }
    return (long)g_test_pos;
}
static int test_file_stat(void *ctx, int fd, void *buf, size_t buflen) {
    (void)ctx; (void)fd; (void)buf; (void)buflen;
    return -9;  // EBADF
}
unsigned long long g_global_step;
int g_dbg_pid;
// Different answers per clock id, so a syscall that ignores the argument and serves one
// clock for everything - which put `date` at 1970 - is visible rather than plausible.
static long long test_clock_ns(void *ctx, int clock_id) {
    (void)ctx;
    switch (clock_id) {
        case 0: return 1700000000123456789LL;  // CLOCK_REALTIME
        case 1: return 5000000000LL;           // CLOCK_MONOTONIC
        default: return 0;
    }
}

static const e2_file_ops test_files = {
    .open = test_file_open,
    .read = test_file_read,
    .write = test_file_write,
    .close = test_file_close,
    .stat = test_file_stat,
    .clock_ns = test_clock_ns,
    .seek = test_file_seek,
    .opendir = test_opendir,
    .stat_path = test_stat_path,
    .truncate_file = test_truncate,
    .statfs_of = test_statfs,
};


static void capture_exit(void *ctx, int status) {
    capture *c = (capture *)ctx;
    c->exited = 1;
    c->status = status;
}

/// Steps the machine, servicing syscalls as they arrive. This is the shape the
/// iOS layer will use too: the interpreter has no idea what a file descriptor is.
static e2_status run_program(e2_machine *m, e2_syscall_env *env, uint64_t budget) {
    for (uint64_t i = 0; i < budget; i++) {
        e2_status st = e2_step(m);
        if (st == E2_SYSCALL) {
            if (env != NULL) {
                st = e2_service_syscall(m, env);
                if (st != E2_OK) {
                    return st;
                }
            } else {
                return st;
            }
            continue;
        }
        if (st != E2_OK) {
            return st;
        }
    }
    return E2_FAULT;
}

// --- Harness ------------------------------------------------------------------

static int failures;
static int checks;

#define CHECK(cond, ...)                                              \
    do {                                                              \
        checks++;                                                     \
        if (!(cond)) {                                                \
            failures++;                                               \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);             \
            printf(__VA_ARGS__);                                      \
            printf("\n");                                             \
        }                                                             \
    } while (0)

#define CHECK_REG(machine, reg, expected)                                          \
    do {                                                                           \
        uint64_t got_ = (machine).cpu.x[(reg)];                                    \
        uint64_t want_ = (uint64_t)(expected);                                     \
        CHECK(got_ == want_, "x%u = 0x%llx, want 0x%llx", (reg),                   \
              (unsigned long long)got_, (unsigned long long)want_);                \
    } while (0)

static uint64_t load_program(e2_machine *machine, const uint32_t *code, size_t count) {
    uint64_t base = 0;
    e2_memory_map_any(&machine->mem, count * 4, E2_PROT_READ | E2_PROT_EXEC | E2_PROT_WRITE,
                      &base);
    e2_memory_write(&machine->mem, base, code, count * 4);
    machine->cpu.pc = base;
    (void)e2_memory_map_any(&machine->mem, 8192, E2_PROT_READ | E2_PROT_WRITE, &base);
    machine->cpu.sp = base + 8192;
    return machine->cpu.pc;
}

static void test_movz_and_ret(void) {
    printf("movz + ret\n");
    e2_machine m;
    e2_machine_init(&m, 0, 8192);
    const uint32_t code[] = {MOVZ_X(0, 42), RET()};
    load_program(&m, code, 2);
    e2_status st = e2_run(&m, 100);
    CHECK(st == E2_FAULT, "expected fault on RET to LR=0, got %s", e2_status_name(st));
    CHECK_REG(m, 0, 42);
    e2_machine_destroy(&m);
}

static void test_add_sub(void) {
    printf("add/sub immediate and register\n");
    e2_machine m;
    e2_machine_init(&m, 0, 8192);
    const uint32_t code[] = {
        MOVZ_X(0, 10),
        ADD_XI(0, 0, 5),      // x0 = 15
        MOVZ_X(1, 3),
        ADD_XR(0, 0, 1),      // x0 = 18
        SUB_XI(0, 0, 8),      // x0 = 10
        SUB_XR(0, 0, 1),      // x0 = 7
        RET(),
    };
    load_program(&m, code, sizeof(code) / sizeof(code[0]));
    e2_run(&m, 100);
    CHECK_REG(m, 0, 7);
    e2_machine_destroy(&m);
}

static void test_movn_movk(void) {
    printf("movn + movk\n");
    e2_machine m;
    e2_machine_init(&m, 0, 8192);
    const uint32_t code[] = {
        MOVZ_X(0, 0),
        MOVK_X(0, 0x1234, 16),   // x0 = 0x12340000
        MOVN_X(1, 0),            // x1 = ~0
        RET(),
    };
    load_program(&m, code, sizeof(code) / sizeof(code[0]));
    e2_run(&m, 100);
    CHECK_REG(m, 0, 0x12340000ULL);
    CHECK_REG(m, 1, ~0ULL);
    e2_machine_destroy(&m);
}

static void test_loop_with_branch(void) {
    printf("counter loop (cmp/b.ne)\n");
    e2_machine m;
    e2_machine_init(&m, 0, 8192);
    const uint32_t code[] = {
        MOVZ_X(0, 0),              // 0: x0 = 0
        ADD_XI(0, 0, 1),           // 1: loop: x0++
        SUBS_XI(31, 0, 5),         // 2: cmp x0, #5
        B_COND(COND_NE, -2),       // 3: b.ne loop
        RET(),                     // 4
    };
    load_program(&m, code, sizeof(code) / sizeof(code[0]));
    e2_run(&m, 1000);
    CHECK_REG(m, 0, 5);
    e2_machine_destroy(&m);
}

static void test_flags_and_condition(void) {
    printf("flag semantics (lt/gt)\n");
    e2_machine m;
    e2_machine_init(&m, 0, 8192);
    const uint32_t code[] = {
        MOVZ_X(0, 3),
        MOVZ_X(1, 7),
        SUBS_XI(31, 0, 7),         // 3 - 7 -> negative, borrow
        CBZ_X(2, 2),               // not taken (flags untouched by CBZ)
        MOVZ_X(2, 111),            // skipped if CBZ taken
        B_COND(COND_LT, 1),        // taken: 3 < 7
        MOVZ_X(2, 222),            // skipped
        MOVZ_X(3, 333),            // executes
        RET(),
    };
    load_program(&m, code, sizeof(code) / sizeof(code[0]));
    e2_run(&m, 100);
    CHECK_REG(m, 2, 222);
    CHECK_REG(m, 3, 333);
    e2_machine_destroy(&m);
}

static void test_load_store(void) {
    printf("ldr/str\n");
    e2_machine m;
    e2_machine_init(&m, 0, 8192);
    uint64_t data_base = 0;
    e2_memory_map_any(&m.mem, 4096, E2_PROT_READ | E2_PROT_WRITE, &data_base);

    const uint32_t code[] = {
        MOVZ_X(0, 0x5678),
        MOVK_X(0, 0x1234, 16),
        // Address goes in x1 via the data region base; the test patches it in.
        STR_X(0, 1, 0),
        LDR_X(2, 1, 0),
        RET(),
    };
    uint64_t prog = load_program(&m, code, sizeof(code) / sizeof(code[0]));
    // Patch "movz x1, #imm" style by loading the base directly into the register.
    m.cpu.x[1] = data_base;
    (void)prog;
    e2_run(&m, 100);
    CHECK_REG(m, 2, 0x12345678ULL);
    e2_machine_destroy(&m);
}

static void test_svc(void) {
    printf("svc surface\n");
    e2_machine m;
    e2_machine_init(&m, 0, 8192);
    const uint32_t code[] = {SVC(93), MOVZ_X(0, 1), RET()};
    load_program(&m, code, sizeof(code) / sizeof(code[0]));
    uint64_t entry = m.cpu.pc;
    e2_status st = e2_step(&m);
    CHECK(st == E2_SYSCALL, "expected syscall status, got %s", e2_status_name(st));
    CHECK(m.ctx.svc_number == 93, "svc number %u", m.ctx.svc_number);
    CHECK(m.cpu.pc == entry + 4, "pc should advance past svc");
    e2_machine_destroy(&m);
}

static void test_hint_space_is_noop(void) {
    printf("hint space: BTI, PACIASP and friends are no-ops\n");
    e2_machine m;
    e2_machine_init(&m, 0, 8192);
    // BTI was the one that mattered: a binary built with -mbranch-protection starts
    // every function with it, so an unimplemented BTI faults on entry - apk did.
    const uint32_t code[] = {
        0xD503245Fu,  // BTI c
        0xD503233Fu,  // PACIASP
        0xD50323BFu,  // AUTIASP
        0xD503201Fu,  // NOP
        MOVZ_X(7, 33),
        RET()
    };
    load_program(&m, code, sizeof(code) / sizeof(code[0]));
    e2_run(&m, 100);
    CHECK_REG(m, 7, 33);
    e2_machine_destroy(&m);
}

static void test_xtn_narrows_lanes(void) {
    printf("XTN keeps the low half of each lane\n");
    e2_machine m;
    e2_machine_init(&m, 0, 8192);
    // V1.4S = {1, 300, 3, 70000}. Narrowing to 4H keeps 1 and 300 as they are and
    // truncates 70000 to its low 16 bits, which is what distinguishes a narrowing
    // instruction from a copy.
    m.cpu.v[1][0] = 1 | (300ULL << 32);
    m.cpu.v[1][1] = 3 | (70000ULL << 32);
    // XTN V0.4H, V1.4S: size=1 (16-bit destination) in 23:22, rn=1, rd=0.
    const uint32_t code[] = {0x0E612820u, RET()};
    load_program(&m, code, 2);
    e2_run(&m, 100);
    const uint16_t *h = (const uint16_t *)&m.cpu.v[0][0];
    CHECK(h[0] == 1 && h[1] == 300 && h[2] == 3 && h[3] == (70000 & 0xFFFF),
          "xtn gave {%u,%u,%u,%u}", h[0], h[1], h[2], h[3]);
    e2_machine_destroy(&m);
}

static void test_jump_table_matches_a_real_run(void) {
    printf("scaled jump-table load plus BR reaches the same target as the hardware\n");
    // CPython dispatches bytecode with `ldr x1, [x0, x1, lsl #3]` followed by `br x1`. A
    // wrong scale or a mis-set PC there runs the wrong opcode handler, which would look
    // like corrupt data rather than like a branch bug.
    e2_machine m;
    e2_machine_init(&m, 0, 1 << 20);
    FILE *f = fopen("tests/guest/jump.elf", "rb");
    CHECK(f != NULL, "jump.elf missing - run make first");
    if (f == NULL) { e2_machine_destroy(&m); return; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *image = (uint8_t *)malloc((size_t)size);
    CHECK(image != NULL, "out of memory");
    if (image == NULL) { fclose(f); e2_machine_destroy(&m); return; }
    if (fread(image, 1, (size_t)size, f) != (size_t)size) { /* short read still usable */ }
    fclose(f);

    e2_loaded_image info = {0};
    e2_load_result lr = e2_load_elf_full(&m, image, (size_t)size, true, &info);
    CHECK(lr == E2_LOAD_OK, "load: %s", e2_load_result_name(lr));
    if (lr == E2_LOAD_OK) {
        capture cap = {0};
        e2_syscall_env env = {.exit = capture_exit, .files = &test_files, .ctx = &cap};
        e2_status st = run_program(&m, &env, 2000000);
        CHECK(st == E2_HALTED, "expected halt, got %s", e2_status_name(st));
        // qemu prints 0x2a here, the value targ stores; an earlier expectation of 0x21 was the
        // print routine mis-rendering the digit a as 1.
        CHECK(strstr(cap.out, "000000000000002a") != NULL,
              "output was \"%.40s\"", cap.out);
    }
    free(image);
    e2_machine_destroy(&m);
}

static void test_bitwise_select_matches_a_real_run(void) {
    printf("BSL / BIT / BIF place their mask where qemu does\n");
    // These three take the mask from different registers, and the names misled twice: BSL
    // masks with Vd, BIT and BIF with Vm. Earlier tests reused one register for two roles, so
    // the error cancelled; this ELF keeps all three distinct.
    e2_machine m;
    e2_machine_init(&m, 0, 1 << 20);
    FILE *f = fopen("tests/guest/bitops.elf", "rb");
    CHECK(f != NULL, "bitops.elf missing - run make first");
    if (f == NULL) { e2_machine_destroy(&m); return; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *image = (uint8_t *)malloc((size_t)size);
    CHECK(image != NULL, "out of memory");
    if (image == NULL) { fclose(f); e2_machine_destroy(&m); return; }
    if (fread(image, 1, (size_t)size, f) != (size_t)size) { /* short read still usable */ }
    fclose(f);

    e2_loaded_image info = {0};
    e2_load_result lr = e2_load_elf_full(&m, image, (size_t)size, true, &info);
    CHECK(lr == E2_LOAD_OK, "load: %s", e2_load_result_name(lr));
    if (lr == E2_LOAD_OK) {
        capture cap = {0};
        e2_syscall_env env = {.exit = capture_exit, .files = &test_files, .ctx = &cap};
        e2_status st = run_program(&m, &env, 2000000);
        CHECK(st == E2_HALTED, "expected halt, got %s", e2_status_name(st));
        CHECK(strstr(cap.out, "cacacacacacacaca") != NULL, "BSL wrong: %.60s", cap.out);
        CHECK(strstr(cap.out, "d8d8d8d8d8d8d8d8") != NULL, "BIT wrong: %.60s", cap.out);
        CHECK(strstr(cap.out, "e4e4e4e4e4e4e4e4") != NULL, "BIF wrong: %.60s", cap.out);
    }
    free(image);
    e2_machine_destroy(&m);
}

static void test_simd_matches_a_real_run(void) {
    printf("CNT, ADDV, REV, EXT, TBL, DUP, UMOV and SMOV match what the hardware produced\n");
    // Eight instructions added long after the backend first worked, each verified against
    // qemu-aarch64 running the same ELF. They are the kind that look right and are not: a byte
    // reverse that reverses the wrong half, a table lookup that uses the wrong index register,
    // a lane read that returns the low half of the vector regardless of the lane asked for.
    e2_machine m;
    e2_machine_init(&m, 0, 1 << 20);
    FILE *f = fopen("tests/guest/simdext.elf", "rb");
    CHECK(f != NULL, "simdext.elf missing - run make first");
    if (f == NULL) { e2_machine_destroy(&m); return; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *image = (uint8_t *)malloc((size_t)size);
    CHECK(image != NULL, "out of memory");
    if (image == NULL) { fclose(f); e2_machine_destroy(&m); return; }
    if (fread(image, 1, (size_t)size, f) != (size_t)size) { /* short read still usable */ }
    fclose(f);

    e2_loaded_image info = {0};
    e2_load_result lr = e2_load_elf_full(&m, image, (size_t)size, true, &info);
    CHECK(lr == E2_LOAD_OK, "load: %s", e2_load_result_name(lr));
    if (lr == E2_LOAD_OK) {
        capture cap = {0};
        e2_syscall_env env = {.exit = capture_exit, .files = &test_files, .ctx = &cap};
        e2_status st = run_program(&m, &env, 2000000);
        CHECK(st == E2_HALTED, "expected halt, got %s", e2_status_name(st));
        CHECK(strstr(cap.out, "0101040404040404") != NULL, "CNT wrong: %.80s", cap.out);
        CHECK(strstr(cap.out, "000000000000007e") != NULL, "ADDV wrong: %.80s", cap.out);
        CHECK(strstr(cap.out, "0011223344556677") != NULL, "REV64 wrong: %.80s", cap.out);
        CHECK(strstr(cap.out, "ffeeddccbbaa9988") != NULL, "EXT wrong: %.80s", cap.out);
        CHECK(strstr(cap.out, "4455667700112233") != NULL, "TBL wrong: %.80s", cap.out);
        CHECK(strstr(cap.out, "8001aa558001aa55") != NULL, "DUP wrong: %.80s", cap.out);
        CHECK(strstr(cap.out, "0000000000000011") != NULL, "UMOV/SMOV wrong: %.80s", cap.out);
    }
    free(image);
    e2_machine_destroy(&m);
}

static void test_permutes_match_a_real_run(void) {
    printf("UZP, TRN, ZIP, XTN, LD1R and SHL match what the hardware produced\n");
    // UZP was implemented as an interleave and ZIP as a concatenation - exactly swapped - and it
    // survived because nothing compared them. A program that gathers even bytes and reads them
    // back gets plausible values either way; only the order shows, and the order always matters.
    e2_machine m;
    e2_machine_init(&m, 0, 1 << 20);
    FILE *f = fopen("tests/guest/permute.elf", "rb");
    CHECK(f != NULL, "permute.elf missing - run make first");
    if (f == NULL) { e2_machine_destroy(&m); return; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *image = (uint8_t *)malloc((size_t)size);
    CHECK(image != NULL, "out of memory");
    if (image == NULL) { fclose(f); e2_machine_destroy(&m); return; }
    if (fread(image, 1, (size_t)size, f) != (size_t)size) { /* short read still usable */ }
    fclose(f);

    e2_loaded_image info = {0};
    e2_load_result lr = e2_load_elf_full(&m, image, (size_t)size, true, &info);
    CHECK(lr == E2_LOAD_OK, "load: %s", e2_load_result_name(lr));
    if (lr == E2_LOAD_OK) {
        capture cap = {0};
        e2_syscall_env env = {.exit = capture_exit, .files = &test_files, .ctx = &cap};
        e2_status st = run_program(&m, &env, 2000000);
        CHECK(st == E2_HALTED, "expected halt, got %s", e2_status_name(st));
        // Every even byte first, then every odd: an interleave would give these the other way
        // round, which is the mistake this was written to catch.
        CHECK(strstr(cap.out, "0e0c0a0806040200") != NULL, "UZP1 wrong: %.120s", cap.out);
        CHECK(strstr(cap.out, "0f0d0b0907050301") != NULL, "UZP2 wrong: %.120s", cap.out);
        CHECK(strstr(cap.out, "1606140412021000") != NULL, "TRN1 wrong: %.120s", cap.out);
        CHECK(strstr(cap.out, "1707150513031101") != NULL, "TRN2 wrong: %.120s", cap.out);
        CHECK(strstr(cap.out, "1303120211011000") != NULL, "ZIP1 wrong: %.120s", cap.out);
        CHECK(strstr(cap.out, "1b0b1a0a19091808") != NULL, "ZIP2 wrong: %.120s", cap.out);
        CHECK(strstr(cap.out, "44332211f0bc7834") != NULL, "XTN wrong: %.120s", cap.out);
        CHECK(strstr(cap.out, "5a5a5a5a5a5a5a5a") != NULL, "LD1R wrong: %.120s", cap.out);
        CHECK(strstr(cap.out, "0000008040201008") != NULL, "SHL wrong: %.120s", cap.out);
    }
    free(image);
    e2_machine_destroy(&m);
}

static void test_division_and_shifts_match_a_real_run(void) {
    printf("SDIV, UDIV and the variable shifts match what the hardware produced\n");
    // A mutation check on SDIV's overflow guard reported NOT CAUGHT, which is why this exists:
    // nothing exercised INT64_MIN / -1, division by zero, or a rounding that goes the wrong way.
    e2_machine m;
    e2_machine_init(&m, 0, 1 << 20);
    FILE *f = fopen("tests/guest/divshift.elf", "rb");
    CHECK(f != NULL, "divshift.elf missing - run make first");
    if (f == NULL) { e2_machine_destroy(&m); return; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *image = (uint8_t *)malloc((size_t)size);
    CHECK(image != NULL, "out of memory");
    if (image == NULL) { fclose(f); e2_machine_destroy(&m); return; }
    if (fread(image, 1, (size_t)size, f) != (size_t)size) { /* short read still usable */ }
    fclose(f);

    e2_loaded_image info = {0};
    e2_load_result lr = e2_load_elf_full(&m, image, (size_t)size, true, &info);
    CHECK(lr == E2_LOAD_OK, "load: %s", e2_load_result_name(lr));
    if (lr == E2_LOAD_OK) {
        capture cap = {0};
        e2_syscall_env env = {.exit = capture_exit, .files = &test_files, .ctx = &cap};
        e2_status st = run_program(&m, &env, 2000000);
        CHECK(st == E2_HALTED, "expected halt, got %s", e2_status_name(st));
        CHECK(strstr(cap.out, "0000000000000032") != NULL, "SDIV 100/2 wrong: %.160s", cap.out);
        CHECK(strstr(cap.out, "fffffffffffffffd") != NULL, "SDIV -7/2 wrong: %.160s", cap.out);
        CHECK(strstr(cap.out, "8000000000000000") != NULL,
              "SDIV INT64_MIN/-1 wrong: %.160s", cap.out);
        CHECK(strstr(cap.out, "7ffffffffffffffc") != NULL, "UDIV wrong: %.160s", cap.out);
        CHECK(strstr(cap.out, "870e961da52cb438") != NULL, "LSLV wrong: %.160s", cap.out);
        CHECK(strstr(cap.out, "87f0e1d2c3b4a596") != NULL, "RORV wrong: %.160s", cap.out);
    }
    free(image);
    e2_machine_destroy(&m);
}

static void test_ldp_stp_match_a_real_run(void) {
    printf("LDP/STP with pre- and post-index match what the hardware produced\n");
    // Expected values came from running the same ELF under qemu-aarch64. The CALL path in
    // CPython's evaluator shuffles arguments with exactly these addressing modes, and a
    // wrong writeback there puts a frame address where an object belongs.
    e2_machine m;
    e2_machine_init(&m, 0, 1 << 20);
    FILE *f = fopen("tests/guest/ldp.elf", "rb");
    CHECK(f != NULL, "ldp.elf missing - run make first");
    if (f == NULL) { e2_machine_destroy(&m); return; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *image = (uint8_t *)malloc((size_t)size);
    CHECK(image != NULL, "out of memory");
    if (image == NULL) { fclose(f); e2_machine_destroy(&m); return; }
    if (fread(image, 1, (size_t)size, f) != (size_t)size) { /* short read still usable */ }
    fclose(f);

    e2_loaded_image info = {0};
    e2_load_result lr = e2_load_elf_full(&m, image, (size_t)size, true, &info);
    CHECK(lr == E2_LOAD_OK, "load: %s", e2_load_result_name(lr));
    if (lr == E2_LOAD_OK) {
        capture cap = {0};
        e2_syscall_env env = {.exit = capture_exit, .files = &test_files, .ctx = &cap};
        e2_status st = run_program(&m, &env, 2000000);
        CHECK(st == E2_HALTED, "expected halt, got %s", e2_status_name(st));
        CHECK(strstr(cap.out, "0000000000000011") != NULL,
              "the reloaded first word was not 0x11: \"%.40s\"", cap.out);
        CHECK(strstr(cap.out, "0000000000000033") != NULL,
              "the pre-index store did not read back as 0x33: \"%.40s\"", cap.out);
    }
    free(image);
    e2_machine_destroy(&m);
}

static void test_bitfield_matches_a_real_run(void) {
    printf("bitfield instructions match what the hardware produced\n");
    // The expected lines below were produced by running the same ELF under qemu-aarch64,
    // not by re-deriving the architecture's bit-mask rules. That distinction matters: an
    // earlier attempt replaced the bitfield code with a reimplementation checked against a
    // reference model I had written myself, and the model was wrong - C and Python agreed
    // with each other and both disagreed with the hardware.
    e2_machine m;
    e2_machine_init(&m, 0, 1 << 20);
    FILE *f = fopen("tests/guest/bitfield.elf", "rb");
    CHECK(f != NULL, "bitfield.elf missing - run make first");
    if (f == NULL) { e2_machine_destroy(&m); return; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *image = (uint8_t *)malloc((size_t)size);
    CHECK(image != NULL, "out of memory");
    if (image == NULL) { fclose(f); e2_machine_destroy(&m); return; }
    if (fread(image, 1, (size_t)size, f) != (size_t)size) { /* short read is still usable */ }
    fclose(f);

    e2_loaded_image info = {0};
    e2_load_result lr = e2_load_elf_full(&m, image, (size_t)size, true, &info);
    CHECK(lr == E2_LOAD_OK, "load: %s", e2_load_result_name(lr));
    if (lr == E2_LOAD_OK) {
        capture cap = {0};
        e2_syscall_env env = {.exit = capture_exit, .files = &test_files, .ctx = &cap};
        e2_status st = run_program(&m, &env, 2000000);
        CHECK(st == E2_HALTED, "expected halt, got %s", e2_status_name(st));
        // First four lines, as qemu printed them.
        CHECK(strncmp(cap.out, "0000000000000001\n0000000000000001\n",
                      34) == 0,
              "first two lines were \"%.40s\"", cap.out);
    }
    free(image);
    e2_machine_destroy(&m);
}

static void test_ushll2_widens_from_the_upper_half(void) {
    printf("USHLL2 takes the upper half of the source and widens it to lane 0 onwards\n");
    // The destination index used to be computed from the source lane, so with Q set the
    // first result landed at half=2, past the end of the register. Nothing was written and
    // callers read whatever had been there. CPython widens its ASCII strings to wchar_t
    // with exactly this instruction, so the four bytes "/sur" ended up in one wchar_t.
    const uint32_t code[] = {0x6F10A421u, RET()};   // ushll2 v1.4s, v1.8h, #0
    e2_machine m;
    e2_machine_init(&m, 0, 8192);
    e2_memory_map_at(&m.mem, 0x1000, 0x1000, E2_PROT_READ | E2_PROT_WRITE);
    e2_memory_write(&m.mem, 0x1000, code, sizeof(code));
    // v1.8h: halfwords 0..7 = 0,1,2,3 in the low 64 bits and 4,5,6,7 in the high.
    m.cpu.v[1][0] = ((uint64_t)3 << 48) | ((uint64_t)2 << 32) | ((uint64_t)1 << 16);
    m.cpu.v[1][1] = ((uint64_t)7 << 48) | ((uint64_t)6 << 32) | ((uint64_t)5 << 16) | 4ULL;
    m.cpu.pc = 0x1000;
    e2_step(&m);
    // Four 32-bit lanes: 4, 5, 6, 7 across the 128-bit register.
    CHECK(m.cpu.v[1][0] == 0x0000000500000004ULL,
          "low half should be lanes 4 and 5, got 0x%016llx",
          (unsigned long long)m.cpu.v[1][0]);
    CHECK(m.cpu.v[1][1] == 0x0000000700000006ULL,
          "high half should be lanes 6 and 7, got 0x%016llx",
          (unsigned long long)m.cpu.v[1][1]);
    e2_machine_destroy(&m);
}

static void test_movi_64bit_immediate(void) {
    printf("MOVI's 64-bit form fills one byte per set bit of imm8\n");
    // `movi d0, #0x000000ffffffff` must give 0x00000000ffffffff. The old expansion
    // replicated the whole byte and produced 0x0f0f0f0f0f0f0f0f - the register that
    // PyCode_Addr2Line writes over its address-range struct, which is why CPython's
    // start-up spent 150 million instructions walking backwards before faulting.
    struct { uint32_t insn; uint64_t want; } cases[] = {
        {0x2F00E5E0u, 0x00000000FFFFFFFFULL},
        {0x2F00E420u, 0x00000000000000FFULL},
        {0x2F00E460u, 0x000000000000FFFFULL},
        {0x2F02E6A0u, 0x00FF00FF00FF00FFULL},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const uint32_t code[] = {cases[i].insn, RET()};
        e2_machine m;
        e2_machine_init(&m, 0, 8192);
        e2_memory_map_at(&m.mem, 0x1000, 0x1000, E2_PROT_READ | E2_PROT_WRITE);
        e2_memory_write(&m.mem, 0x1000, code, sizeof(code));
        m.cpu.pc = 0x1000;
        e2_step(&m);
        CHECK(m.cpu.v[0][0] == cases[i].want,
              "0x%08x should give 0x%016llx, got 0x%016llx", cases[i].insn,
              (unsigned long long)cases[i].want, (unsigned long long)m.cpu.v[0][0]);
        e2_machine_destroy(&m);
    }
}

static void test_ldrsb_and_tbz_together(void) {
    printf("LDRSB sign-extends a byte and TBZ tests bit 31 of the result\n");
    // This pair is what CPython's backward line-table scan is built from, and a wrong
    // sign-extension would make the scan run backwards through unmapped memory instead of
    // stopping at the first leading byte. Verified separately because "the disassembly looks
    // right" is not the same as "the simulation is right".
    const uint32_t code[] = {0x39C00024u,   // ldrsb w4, [x1]
                             0x36F80064u,   // tbz   w4, #31, +0x10
                             0x52800020u,   // mov   w0, #1   (bit was set: fell through)
                             0x14000002u,   // b     end
                             0x52800040u,   // mov   w0, #2   (bit was clear: branched)
                             0xD65F03C0u};  // ret
    const uint64_t bytes = 0x30000000ULL;
    for (int pass = 0; pass < 2; pass++) {
        e2_machine m;
        e2_machine_init(&m, 0, 65536);
        e2_memory_map_at(&m.mem, 0x1000, 0x1000, E2_PROT_READ | E2_PROT_WRITE);
        e2_memory_write(&m.mem, 0x1000, code, sizeof(code));
        e2_memory_map_at(&m.mem, bytes, 0x1000, E2_PROT_READ | E2_PROT_WRITE);
        const uint8_t byte = pass ? 0xFF : 0x7F;
        e2_memory_write(&m.mem, bytes, &byte, 1);
        m.cpu.pc = 0x1000;
        m.cpu.x[1] = bytes;
        for (int i = 0; i < 8; i++) {
            if (m.cpu.pc < 0x1000 || m.cpu.pc >= 0x1000 + sizeof(code)) break;
            if (e2_step(&m) != E2_OK) break;
        }
        CHECK((m.cpu.x[0] & 0xFF) == (pass ? 1 : 2),
              "byte 0x%02x should give w0=%d, got %d", byte, pass ? 1 : 2,
              (int)(m.cpu.x[0] & 0xFF));
        e2_machine_destroy(&m);
    }
}

static void test_ld1_lane_loads_one_lane(void) {
    printf("LD1 loads a single lane from memory and leaves the rest alone\n");
    // Neither unit-tested nor present in any binary this project runs, so before this it
    // had no coverage of any kind. The encoding family is 0x0D008000, with bit 22 selecting
    // load and bits 11:10 the element size.
    e2_machine m;
    e2_machine_init(&m, 0, 8192);
    const uint64_t data = 0x30000000ULL;
    e2_memory_map_at(&m.mem, data, 0x1000, E2_PROT_READ | E2_PROT_WRITE);
    const uint32_t src[4] = {0xAAAA1111u, 0xBBBB2222u, 0xCCCC3333u, 0xDDDD4444u};
    e2_memory_write(&m.mem, data, src, sizeof(src));

    // Pre-fill V0 so an untouched lane is distinguishable from a written one.
    m.cpu.v[0][0] = 0x1111111111111111ULL;
    m.cpu.v[0][1] = 0x2222222222222222ULL;

    // LD1 {V0.S}[0], [x1] -> 0x0D408020: load bit 22, size 0 (32-bit lanes), Q=0, S=0.
    // S is inside the guarded mask, so this form can only select lane 0 or 2; lane 1 would
    // need a different encoding, which is worth knowing before writing a test against it.
    const uint32_t code[] = {0x0D408020u, RET()};
    load_program(&m, code, 2);
    m.cpu.x[1] = data;
    e2_run(&m, 100);

    const uint32_t lane0 = (uint32_t)m.cpu.v[0][0];
    const uint32_t lane1 = (uint32_t)(m.cpu.v[0][0] >> 32);
    CHECK(lane0 == 0xAAAA1111u, "lane 0 should hold the loaded word, got 0x%08x", lane0);
    CHECK(lane1 == 0x11111111u, "lane 1 should be untouched, got 0x%08x", lane1);
    e2_machine_destroy(&m);
}

static void test_sysinfo_writes_the_struct(void) {
    printf("sysinfo fills the struct rather than returning success and writing nothing\n");
    // Absent entirely until it was noticed that busybox `free` printed a row of
    // thirty-digit numbers: it calls sysinfo first and, when the call has no handler,
    // prints a struct it never initialised. Returning 0 without writing would have the
    // same effect, so both are checked here.
    e2_machine m;
    e2_machine_init(&m, 0, 1 << 20);
    const uint64_t buf = 0x30000000ULL;
    e2_memory_map_at(&m.mem, buf, 0x1000, E2_PROT_READ | E2_PROT_WRITE);
    e2_syscall_env env = {0};

    // Pre-fill with a marker so "written nothing" is distinguishable from "wrote zeros".
    uint8_t marker[112];
    memset(marker, 0xAB, sizeof(marker));
    e2_memory_write(&m.mem, buf, marker, sizeof(marker));

    m.cpu.x[8] = 179;   // sysinfo
    m.cpu.x[0] = buf;
    CHECK(e2_service_syscall(&m, &env) == E2_OK, "sysinfo should complete");
    CHECK_REG(m, 0, 0);

    uint8_t got[112];
    e2_memory_read(&m.mem, buf, got, sizeof(got));
    int untouched = 0;
    for (int i = 0; i < 112; i++) if (got[i] == 0xAB) untouched++;
    CHECK(untouched == 0, "sysinfo left %d byte(s) unwritten", untouched);

    uint64_t totalram = 0;
    memcpy(&totalram, got + 32, 8);
    CHECK(totalram == 1024000ULL * 1024, "totalram %llu should match meminfo",
          (unsigned long long)totalram);
    e2_machine_destroy(&m);
}

static void test_barriers_retire(void) {
    printf("DSB, DMB and ISB are accepted and change nothing\n");
    // Execution here is strictly sequential and the host provides the ordering, so these
    // retire without effect. They still have to be recognised: an atomic sequence that
    // hits an unimplemented barrier stops there, and the symptom looks like a hang in
    // whatever was being locked rather than a missing instruction.
    const uint32_t barriers[] = {
        0xD5033F9Fu,  // DSB SY
        0xD5033FBFu,  // DMB SY
        0xD5033FDFu,  // ISB
        0xD503349Fu,  // DSB ISH
    };
    for (size_t i = 0; i < sizeof(barriers) / sizeof(barriers[0]); i++) {
        e2_machine m;
        e2_machine_init(&m, 0, 8192);
        m.cpu.x[0] = 0x1234;
        const uint32_t code[] = {barriers[i], 0xD2800021u /* movz x1,#1 */, RET()};
        load_program(&m, code, 3);
        // The RET at the end jumps to lr, which is 0 here, so the run ends in a fault.
        // What matters is that execution reached past the barrier at all.
        e2_run(&m, 100);
        // The instruction after the barrier must have run.
        CHECK(m.cpu.x[1] == 1, "barrier 0x%08x: x1 = 0x%llx, want 1", barriers[i],
              (unsigned long long)m.cpu.x[1]);
        CHECK(m.cpu.x[0] == 0x1234, "barrier 0x%08x disturbed x0", barriers[i]);
        e2_machine_destroy(&m);
    }
}

static void test_ldrsb_backward_scan(void) {
    printf("ldrsb sign-extends, which is what stops a backward UTF-8 scan\n");
    // 0x38DFF0C1 is `ldrsb w1, [x6, #-1]` - unscaled, no writeback, as CPython's scan
    // uses it. A byte of 0xFF must arrive as -1 so that testing bit 31 says "stop". If
    // it arrives as 255 the bit is clear and the loop never ends, which is exactly the
    // shape of the hang being investigated.
    struct { uint8_t byte; int64_t expect; } cases[] = {
        {0xFF, -1},
        {0x80, -128},
        {0x7F, 127},
        {0xC3, -61},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        e2_machine m;
        e2_machine_init(&m, 0, 8192);
        const uint64_t data = 0x30000000ULL;
        e2_memory_map_at(&m.mem, data, 0x1000, E2_PROT_READ | E2_PROT_WRITE);
        e2_memory_write(&m.mem, data, &cases[i].byte, 1);
        const uint32_t code[] = {0x38DFF0C1u, RET()};
        load_program(&m, code, 2);
        m.cpu.x[6] = data + 1;   // one past the byte
        e2_run(&m, 100);
        // A 32-bit destination: the value is sign-extended to 32 bits, so 0xFF
        // arrives as 0xFFFFFFFF with bit 31 set - not as 0xFFFF_FFFFFFFF. The bit is
        // what the scan tests, so that representation is the correct one.
        CHECK((int32_t)m.cpu.x[1] == (int32_t)cases[i].expect,
              "byte 0x%02x gave %d, want %d", cases[i].byte,
              (int)(int32_t)m.cpu.x[1], (int)(int32_t)cases[i].expect);
        e2_machine_destroy(&m);
    }
}

static void test_frame_pointer_prologue(void) {
    printf("the aarch64 frame prologue saves and restores x29 through SP\n");
    // Every non-leaf function on aarch64 starts with
    //     stp x29, x30, [sp, #-16]! ; mov x29, sp
    // and ends with
    //     ldp x29, x30, [sp], #16 ; ret
    // CPython builds its traceback by walking the x29 chain, so if the pre-indexed
    // store or the post-indexed load writes the wrong slot, that chain becomes a cycle
    // and walking it never terminates. This checks the four values involved.
    e2_machine m;
    e2_machine_init(&m, 0, 8192);
    m.cpu.sp = 0x7000;
    m.cpu.x[29] = 0x1234;   // caller's frame
    m.cpu.x[30] = 0x5678;   // return address
    const uint32_t code[] = {
        0xA9BF7BFDu,  // stp x29, x30, [sp, #-16]!
        0x910003FD,  // mov x29, sp
        0xA8C17BFDu,  // ldp x29, x30, [sp], #16
        RET()
    };
    load_program(&m, code, 4);
    // load_program sets sp; put it back to the value this test wants.
    m.cpu.sp = 0x7000;
    e2_run(&m, 100);
    CHECK(m.cpu.x[29] == 0x1234, "x29 restored to 0x%llx, want 0x1234",
          (unsigned long long)m.cpu.x[29]);
    CHECK(m.cpu.x[30] == 0x5678, "x30 restored to 0x%llx, want 0x5678",
          (unsigned long long)m.cpu.x[30]);
    CHECK(m.cpu.sp == 0x7000, "sp back to 0x%llx, want 0x7000",
          (unsigned long long)m.cpu.sp);
    e2_machine_destroy(&m);
}

static void test_tbz_and_tbnz(void) {
    printf("TBZ branches on zero, TBNZ on one\n");
    // This is the instruction the original BRE investigation stopped at: a `tbz` whose
    // condition differed, with bit 31 of the tested word at stake. Three things are
    // easy to get wrong - which bit is tested, whether the branch is taken on a set or
    // a clear bit, and whether the value is read as 32 or 64 bits.
    struct { uint64_t value; uint32_t tbz; uint32_t tbnz; const char *label; } cases[] = {
        {0,   1, 2, "bit clear"},
        {1,   2, 1, "bit set"},
        {0x80000000ULL, 1, 2, "bit 31 set, bit 0 clear"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        for (int mode = 0; mode < 2; mode++) {
            e2_machine m;
            e2_machine_init(&m, 0, 8192);
            m.cpu.x[0] = cases[i].value;
            // movz x1,#1 ; TBZ/TBNZ x0,#0,+8 ; movz x1,#2 ; ret
            // 0x36000040 is TBZ x0,#0,+8 and 0x37000040 is TBNZ; only bit 24 differs.
            const uint32_t code[] = {0xD2800021u,
                                     mode == 0 ? 0x36000040u : 0x37000040u,
                                     0xD2800041u, RET()};
            load_program(&m, code, 4);
            e2_run(&m, 100);
            const uint64_t want = (mode == 0) ? cases[i].tbz : cases[i].tbnz;
            (void)0;
            CHECK(m.cpu.x[1] == want, "%s %s: x1=%llu want %llu", cases[i].label,
                  mode == 0 ? "TBZ" : "TBNZ", (unsigned long long)m.cpu.x[1],
                  (unsigned long long)want);
            e2_machine_destroy(&m);
        }
    }
}

static void test_tbz_on_bit_31(void) {
    printf("TBZ can test bit 31, which lives in the instruction's own bit 31\n");
    // The tested bit number is split across two fields: bit 5 of the number comes from
    // bit 31 of the instruction, bits 4:0 from bits 23:19. Reading only the low field
    // makes every test_high_bit look like a test of a low bit - which is the shape of
    // the disagreement the BRE investigation originally hit.
    struct { uint64_t value; uint64_t want; } cases[] = {
        {0x80000000ULL, 2},   // bit 31 set: TBZ tests zero, so it does not branch
        {0x7FFFFFFFULL, 1},   // bit 31 clear: TBZ branches
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        e2_machine m;
        e2_machine_init(&m, 0, 8192);
        m.cpu.x[0] = cases[i].value;
        // TBZ x0, #31, +8 -> 0x36F80040: the bit number 31 is b5=0 plus b40=31, not
        // bit 31 of the instruction - setting that bit would ask for bit 63.
        const uint32_t code[] = {0xD2800021u, 0x36F80040u, 0xD2800041u, RET()};
        load_program(&m, code, 4);
        e2_run(&m, 100);
        CHECK(m.cpu.x[1] == cases[i].want, "value 0x%llx gave x1=%llu want %llu",
              (unsigned long long)cases[i].value, (unsigned long long)m.cpu.x[1],
              (unsigned long long)cases[i].want);
        e2_machine_destroy(&m);
    }
}

static void test_widen_multiply(void) {
    printf("SMADDL and UMADDL treat the same operands differently\n");
    // W1 = 0xFFFFFFFF, W2 = 2, X3 = 0. Signed that is -1 * 2 = -2; unsigned it is
    // 4294967295 * 2 = 8589934590. The decoder selects signedness with bit 23 and
    // takes Ra from bits 14:10 (bit 15 is the subtract flag), so writing the encoding
    // from the wrong field positions produces a subtract against a nonsense register
    // and a number that looks like an ordinary add.
    struct { uint32_t insn; const char *name; uint64_t expect; } cases[] = {
        {0x9B220C20u, "SMADDL", (uint64_t)(int64_t)-2},   // bits 23-21 = 001
        {0x9BA20C20u, "UMADDL", 8589934590ULL},           // bits 23-21 = 101
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        e2_machine m;
        e2_machine_init(&m, 0, 8192);
        m.cpu.x[1] = 0xFFFFFFFFULL;
        m.cpu.x[2] = 2;
        m.cpu.x[3] = 0;
        const uint32_t code[] = {cases[i].insn, RET()};
        load_program(&m, code, 2);
        e2_run(&m, 100);
        CHECK(m.cpu.x[0] == cases[i].expect, "%s gave 0x%llx, want 0x%llx",
              cases[i].name, (unsigned long long)m.cpu.x[0],
              (unsigned long long)cases[i].expect);
        e2_machine_destroy(&m);
    }
}

static void test_saturating_narrow(void) {
    printf("SQXTN and UQXTN clamp instead of truncating\n");
    // The clamping is the whole point of these two: XTN above truncates, these clamp.
    // Values either side of both boundaries, so a range check that is one off - or
    // that uses the wrong signedness - lands on a different lane.
    struct { uint32_t insn; const char *name; uint16_t expect[4]; } cases[] = {
        // V1.4S = {1, 40000, -40000, 70000} -> signed 16-bit
        {0x0E614820u, "SQXTN", {1, 32767, (uint16_t)-32768, 32767}},
        // Same input, unsigned 16-bit. 40000 is already in range so it survives;
        // the negative lane reads as a huge unsigned number and clamps to 65535,
        // as does 70000.
        {0x2E614820u, "UQXTN", {1, 40000, 65535, 65535}},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        e2_machine m;
        e2_machine_init(&m, 0, 8192);
        m.cpu.v[1][0] = (uint32_t)1 | ((uint64_t)(uint32_t)40000 << 32);
        m.cpu.v[1][1] = (uint32_t)(-40000) | ((uint64_t)(uint32_t)70000 << 32);
        const uint32_t code[] = {cases[i].insn, RET()};
        load_program(&m, code, 2);
        e2_run(&m, 100);
        // Read as unsigned: a signed read would turn a correct 40000 into -25536
        // and make a passing result look like a failure.
        const uint16_t *h = (const uint16_t *)&m.cpu.v[0][0];
        CHECK(h[0] == cases[i].expect[0] && h[1] == cases[i].expect[1] &&
              h[2] == cases[i].expect[2] && h[3] == cases[i].expect[3],
              "%s gave {%u,%u,%u,%u}", cases[i].name, h[0], h[1], h[2], h[3]);
        e2_machine_destroy(&m);
    }
}

static void test_simd_permute(void) {
    printf("UZP1, TRN1 and ZIP1 place lanes where the encoding says\n");
    // V1.4S = {1,2,3,4}, V2.4S = {5,6,7,8}. The opcode sits in bits 15:12 (1 UZP1,
    // 2 TRN1, 3 ZIP1), rm in 20:16, rn in 9:5, rd in 4:0, size in 23:22, Q in bit 30.
    // All three are decoded by the same rule, so checking each against its own result
    // is what shows the opcode is actually consulted.
    struct { uint32_t insn; unsigned l0, l1, l2, l3; const char *name; } cases[] = {
        // UZP concatenates the even elements of A then of B; interleaving them is TRN1,
        // and the expectation here had encoded the interleaved form, so the bug was
        // pinned in place by its own test.
        {0x4E821820u, 1, 3, 5, 7, "UZP1"},
        {0x4E822820u, 1, 5, 3, 7, "TRN1"},   // a0 b0 a2 b2 for 4S
        // ZIP1 interleaves the low halves; it had been tested as if it concatenated them.
        {0x4E823820u, 1, 5, 2, 6, "ZIP1"},
        {0x4E825820u, 2, 4, 6, 8, "UZP2"},
        {0x4E826820u, 2, 6, 4, 8, "TRN2"},   // a1 b1 a3 b3
        {0x4E827820u, 3, 7, 4, 8, "ZIP2"},   // a2 a3 b2 b3
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        e2_machine m;
        e2_machine_init(&m, 0, 8192);
        m.cpu.v[1][0] = 1 | (2ULL << 32);
        m.cpu.v[1][1] = 3 | (4ULL << 32);
        m.cpu.v[2][0] = 5 | (6ULL << 32);
        m.cpu.v[2][1] = 7 | (8ULL << 32);
        const uint32_t code[] = {cases[i].insn, RET()};
        load_program(&m, code, 2);
        e2_run(&m, 100);
        const uint32_t a = (uint32_t)m.cpu.v[0][0];
        const uint32_t b = (uint32_t)(m.cpu.v[0][0] >> 32);
        const uint32_t c = (uint32_t)m.cpu.v[0][1];
        const uint32_t d = (uint32_t)(m.cpu.v[0][1] >> 32);
        CHECK(a == cases[i].l0 && b == cases[i].l1 && c == cases[i].l2 && d == cases[i].l3,
              "%s gave {%u,%u,%u,%u}, want {%u,%u,%u,%u}", cases[i].name, a, b, c, d,
              cases[i].l0, cases[i].l1, cases[i].l2, cases[i].l3);
        e2_machine_destroy(&m);
    }
}

static void test_clock_gettime_honours_the_id(void) {
    printf("clock_gettime serves the clock that was asked for\n");
    e2_machine m;
    e2_machine_init(&m, 0, 1 << 20);
    const uint64_t ts = 0x30000000ULL;
    e2_memory_map_at(&m.mem, ts, 0x1000, E2_PROT_READ | E2_PROT_WRITE);
    e2_syscall_env env = {0};
    env.files = &test_files;

    // CLOCK_REALTIME (0): seconds must reflect the realtime value, not the monotonic one.
    m.cpu.x[8] = E2_SYS_clock_gettime;
    m.cpu.x[0] = 0;
    m.cpu.x[1] = ts;
    CHECK(e2_service_syscall(&m, &env) == E2_OK, "clock_gettime should complete");
    int64_t got[2] = {0, 0};
    e2_memory_read(&m.mem, ts, got, sizeof(got));
    CHECK(got[0] == 1700000000LL, "realtime seconds %lld", (long long)got[0]);

    // CLOCK_MONOTONIC (1): a different clock means different seconds. Serving one clock
    // for every id is exactly the bug that made date print 1970.
    m.cpu.x[8] = E2_SYS_clock_gettime;
    m.cpu.x[0] = 1;
    m.cpu.x[1] = ts;
    CHECK(e2_service_syscall(&m, &env) == E2_OK, "clock_gettime should complete");
    e2_memory_read(&m.mem, ts, got, sizeof(got));
    CHECK(got[0] == 5, "monotonic seconds %lld", (long long)got[0]);

    e2_machine_destroy(&m);
}

static void test_statfs_layout(void) {
    printf("statfs writes each field at its aarch64 offset\n");
    e2_machine m;
    e2_machine_init(&m, 0, 1 << 20);
    const uint64_t path_addr = 0x30000000ULL, buf = 0x30001000ULL;
    e2_memory_map_at(&m.mem, path_addr, 0x1000, E2_PROT_READ | E2_PROT_WRITE);
    e2_memory_map_at(&m.mem, buf, 0x1000, E2_PROT_READ | E2_PROT_WRITE);
    e2_syscall_env env = {0};
    env.files = &test_files;

    const char p[] = "/bin";
    e2_memory_write(&m.mem, path_addr, p, sizeof(p));
    m.cpu.x[8] = 43;             // statfs(path, buf)
    m.cpu.x[0] = path_addr;
    m.cpu.x[1] = buf;
    CHECK(e2_service_syscall(&m, &env) == E2_OK, "statfs should complete");
    CHECK_REG(m, 0, 0);

    // Deliberately not asserting the field offsets here. The struct is filled by the
    // host layer (tools/rune.c), not by the syscall layer, and the mock above stands in
    // for that layer - so checking the values would only be checking the mock against
    // itself. What this test can establish is the routing: the path reached the host
    // and the buffer was accepted. The layout is covered by `df` in the differential
    // suite, where a real filesystem fills it.
    uint8_t got[128] = {0};
    e2_memory_read(&m.mem, buf, got, sizeof(got));
    CHECK(*(uint64_t *)(got + 0) == 0xEF53, "the host's data should reach the buffer");
    CHECK(g_statfs_fd == -1, "statfs should pass fd -1, got %d", g_statfs_fd);
    CHECK(strcmp(g_statfs_path, "/bin") == 0, "statfs path %s", g_statfs_path);

    e2_machine_destroy(&m);
}

static void test_truncate_forms(void) {
    printf("truncate and ftruncate reach the host as different calls\n");
    e2_machine m;
    e2_machine_init(&m, 0, 1 << 20);
    const uint64_t path_addr = 0x30000000ULL;
    e2_memory_map_at(&m.mem, path_addr, 0x1000, E2_PROT_READ | E2_PROT_WRITE);
    e2_syscall_env env = {0};
    env.files = &test_files;

    // ftruncate(3, 1024): descriptor form, no path.
    m.cpu.x[8] = 46;
    m.cpu.x[0] = 3;
    m.cpu.x[1] = 1024;
    CHECK(e2_service_syscall(&m, &env) == E2_OK, "ftruncate should complete");
    CHECK_REG(m, 0, 0);
    CHECK(g_trunc_fd == 3, "ftruncate should pass fd 3, got %d", g_trunc_fd);
    CHECK(g_trunc_path[0] == '\0', "ftruncate should pass no path");
    CHECK(g_trunc_len == 1024, "ftruncate length %lld", g_trunc_len);

    // truncate("/bin", 7): path form, descriptor -1. Passing the path in the fd slot
    // would still "work" against a lenient host and be wrong.
    const char p[] = "/bin";
    e2_memory_write(&m.mem, path_addr, p, sizeof(p));
    m.cpu.x[8] = 45;
    m.cpu.x[0] = path_addr;
    m.cpu.x[1] = 7;
    CHECK(e2_service_syscall(&m, &env) == E2_OK, "truncate should complete");
    CHECK_REG(m, 0, 0);
    CHECK(g_trunc_fd == -1, "truncate should pass fd -1, got %d", g_trunc_fd);
    CHECK(strcmp(g_trunc_path, "/bin") == 0, "truncate path %s", g_trunc_path);
    CHECK(g_trunc_len == 7, "truncate length %lld", g_trunc_len);

    e2_machine_destroy(&m);
}

static void test_newfstatat_honours_no_follow(void) {
    printf("newfstatat can stat a dangling symlink when told not to follow\n");
    e2_machine m;
    e2_machine_init(&m, 0, 1 << 20);
    const uint64_t path_addr = 0x30000000ULL, buf = 0x30001000ULL;
    e2_memory_map_at(&m.mem, path_addr, 0x1000, E2_PROT_READ | E2_PROT_WRITE);
    e2_memory_map_at(&m.mem, buf, 0x1000, E2_PROT_READ | E2_PROT_WRITE);
    e2_syscall_env env = {0};
    env.files = &test_files;

    const char p[] = "/dangling";
    e2_memory_write(&m.mem, path_addr, p, sizeof(p));

    // AT_SYMLINK_NOFOLLOW (0x100) in x3: the link itself is what gets reported, so a
    // dangling target does not matter. Without this, rm cannot remove such a name.
    m.cpu.x[8] = 79;
    m.cpu.x[0] = (uint64_t)-100;   // AT_FDCWD
    m.cpu.x[1] = path_addr;
    m.cpu.x[2] = buf;
    m.cpu.x[3] = 0x100;
    CHECK(e2_service_syscall(&m, &env) == E2_OK, "newfstatat should complete");
    CHECK_REG(m, 0, 0);

    // And without the flag it must follow, which fails for a dangling link.
    m.cpu.x[8] = 79;
    m.cpu.x[0] = (uint64_t)-100;
    m.cpu.x[1] = path_addr;
    m.cpu.x[2] = buf;
    m.cpu.x[3] = 0;
    CHECK(e2_service_syscall(&m, &env) == E2_OK, "newfstatat should complete");
    CHECK((int64_t)m.cpu.x[0] == -2, "following a dangling link must fail, got %lld",
          (long long)(int64_t)m.cpu.x[0]);

    e2_machine_destroy(&m);
}

static void test_chdir_checks_the_directory_exists(void) {
    printf("chdir refuses a path that is not a directory\n");
    e2_machine m;
    e2_machine_init(&m, 0, 1 << 20);
    // The path has to live in guest memory, and it has to be mapped: the syscall reads
    // it through the page tables, so an unmapped address answers EFAULT.
    const uint64_t path_addr = 0x30000000ULL;
    e2_memory_map_at(&m.mem, path_addr, 0x1000, E2_PROT_READ | E2_PROT_WRITE);
    e2_syscall_env env = {0};
    env.files = &test_files;
    snprintf(env.cwd, sizeof(env.cwd), "/");
    const char ok_path[] = "/bin";
    e2_memory_write(&m.mem, path_addr, ok_path, sizeof(ok_path));

    m.cpu.x[8] = E2_SYS_chdir;
    m.cpu.x[0] = path_addr;
    CHECK(e2_service_syscall(&m, &env) == E2_OK, "chdir /bin should succeed");
    CHECK_REG(m, 0, 0);
    CHECK(strcmp(env.cwd, "/bin") == 0, "cwd should be /bin, got %s", env.cwd);

    const char bad_path[] = "/nope";
    e2_memory_write(&m.mem, path_addr, bad_path, sizeof(bad_path));
    m.cpu.x[8] = E2_SYS_chdir;
    m.cpu.x[0] = path_addr;
    CHECK(e2_service_syscall(&m, &env) == E2_OK, "chdir should complete");
    CHECK((int64_t)m.cpu.x[0] == -2, "chdir to a missing directory must fail, got %lld",
          (long long)(int64_t)m.cpu.x[0]);
    CHECK(strcmp(env.cwd, "/bin") == 0, "a failed chdir must not move cwd");

    e2_machine_destroy(&m);
}

static void test_lseek_reports_the_new_position(void) {
    printf("lseek moves the cursor and returns where it landed\n");
    e2_machine m;
    e2_machine_init(&m, 0, 8192);
    e2_syscall_env env = {0};
    env.files = &test_files;

    g_test_pos = 0;
    // lseek(3, 100, SEEK_SET) -> 100
    m.cpu.x[8] = E2_SYS_lseek;
    m.cpu.x[0] = 3;
    m.cpu.x[1] = 100;
    m.cpu.x[2] = 0;
    CHECK(e2_service_syscall(&m, &env) == E2_OK, "lseek should succeed");
    CHECK_REG(m, 0, 100);

    // lseek(3, 25, SEEK_CUR) -> 125; a syscall that returned `offset` instead of the
    // resulting position would answer 25 here.
    m.cpu.x[8] = E2_SYS_lseek;
    m.cpu.x[0] = 3;
    m.cpu.x[1] = 25;
    m.cpu.x[2] = 1;
    CHECK(e2_service_syscall(&m, &env) == E2_OK, "lseek SEEK_CUR should succeed");
    CHECK_REG(m, 0, 125);

    e2_machine_destroy(&m);
}

static void test_nop(void) {
    printf("nop\n");
    e2_machine m;
    e2_machine_init(&m, 0, 8192);
    const uint32_t code[] = {NOP(), NOP(), MOVZ_X(5, 9), RET()};
    load_program(&m, code, sizeof(code) / sizeof(code[0]));
    e2_run(&m, 100);
    CHECK_REG(m, 5, 9);
    e2_machine_destroy(&m);
}

static void test_unimplemented(void) {
    printf("unimplemented instruction is reported, not mis-executed\n");
    e2_machine m;
    e2_machine_init(&m, 0, 8192);
    const uint32_t code[] = {0x00000000u};  // UDF #0
    load_program(&m, code, 1);
    e2_status st = e2_step(&m);
    CHECK(st == E2_UNIMPL, "expected unimplemented, got %s", e2_status_name(st));
    e2_machine_destroy(&m);
}

static void test_hello_world(void) {
    printf("hello world through SVC write + exit\n");
    e2_machine m;
    e2_machine_init(&m, 0, 8192);

    // mov x0, #1        ; fd 1
    // adr x1, msg       ; buffer, 28 bytes ahead
    // mov x2, #6        ; length
    // mov x8, #64       ; SYS_write
    // svc #0
    // mov x0, #0
    // mov x8, #93       ; SYS_exit
    // svc #0
    // msg: "hello\n"
    const uint32_t code[] = {
        MOVZ_X(0, 1),
        0x100000E1u,          // adr x1, +28
        MOVZ_X(2, 6),
        MOVZ_X(8, E2_SYS_write),
        SVC(0),
        MOVZ_X(0, 0),
        MOVZ_X(8, E2_SYS_exit),
        SVC(0),
        // "hello\n" packed little-endian: h e l l  /  o \n
        0x6C6C6568u, 0x00000A6Fu,
    };
    load_program(&m, code, sizeof(code) / sizeof(code[0]));

    capture cap = {0};
    e2_syscall_env env = {.exit = capture_exit, .files = &test_files, .ctx = &cap};
    e2_status st = run_program(&m, &env, 100);

    CHECK(st == E2_HALTED, "expected halt, got %s", e2_status_name(st));
    CHECK(cap.exited == 1, "exit hook not called");
    CHECK(cap.status == 0, "exit status %d", cap.status);
    CHECK(strcmp(cap.out, "hello\n") == 0, "output was \"%s\"", cap.out);
    e2_machine_destroy(&m);
}

static void test_exit_status(void) {
    printf("exit status propagates\n");
    e2_machine m;
    e2_machine_init(&m, 0, 8192);
    const uint32_t code[] = {MOVZ_X(0, 7), MOVZ_X(8, E2_SYS_exit), SVC(0)};
    load_program(&m, code, sizeof(code) / sizeof(code[0]));

    capture cap = {0};
    e2_syscall_env env = {.exit = capture_exit, .files = &test_files, .ctx = &cap};

    e2_status st = run_program(&m, &env, 100);
    CHECK(st == E2_HALTED, "expected halt, got %s", e2_status_name(st));
    CHECK(m.exit_status == 7, "machine exit_status %d", m.exit_status);
    CHECK(cap.status == 7, "captured status %d", cap.status);
    e2_machine_destroy(&m);
}

static void test_unknown_syscall(void) {
    printf("unknown syscall returns -ENOSYS instead of crashing\n");
    e2_machine m;
    e2_machine_init(&m, 0, 8192);
    const uint32_t code[] = {MOVZ_X(8, 1234), SVC(0), MOVZ_X(1, 1), RET()};
    load_program(&m, code, sizeof(code) / sizeof(code[0]));

    capture cap = {0};
    e2_syscall_env env = {.exit = capture_exit, .files = &test_files, .ctx = &cap};
    e2_status st = run_program(&m, &env, 10);
    CHECK(st != E2_HALTED, "should keep running after ENOSYS");
    CHECK((int64_t)m.cpu.x[0] == -38, "x0 = %lld, want -ENOSYS(-38)",
          (long long)(int64_t)m.cpu.x[0]);
    CHECK_REG(m, 1, 1);
    e2_machine_destroy(&m);
}

static void test_load_real_elf(void) {
    printf("load and run a real aarch64 ELF\n");
    FILE *f = fopen("tests/guest/hello.elf", "rb");
    if (f == NULL) {
        printf("  SKIP (tests/guest/hello.elf not built)\n");
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *image = (uint8_t *)malloc((size_t)size);
    if (image == NULL || fread(image, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        free(image);
        CHECK(0, "could not read hello.elf");
        return;
    }
    fclose(f);

    e2_machine m;
    e2_machine_init(&m, 0, 16384);
    e2_load_result lr = e2_load_elf(&m, image, (size_t)size);
    CHECK(lr == E2_LOAD_OK, "load result: %s", e2_load_result_name(lr));

    if (lr == E2_LOAD_OK) {
        capture cap = {0};
        e2_syscall_env env = {.exit = capture_exit, .files = &test_files, .ctx = &cap};
        e2_status st = run_program(&m, &env, 1000);
        CHECK(st == E2_HALTED, "expected halt, got %s (%s)", e2_status_name(st),
              m.ctx.reason ? m.ctx.reason : "-");
        CHECK(strcmp(cap.out, "hello\n") == 0, "output was \"%s\"", cap.out);
        CHECK(cap.status == 0, "exit status %d", cap.status);
    }

    e2_machine_destroy(&m);
    free(image);
}

static void test_recursive_fib_elf(void) {
    printf("recursive fib through a real ELF (frame, BL/RET, branches)\n");
    FILE *f = fopen("tests/guest/fib.elf", "rb");
    if (f == NULL) {
        printf("  SKIP (tests/guest/fib.elf not built)\n");
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *image = (uint8_t *)malloc((size_t)size);
    if (image == NULL || fread(image, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        free(image);
        CHECK(0, "could not read fib.elf");
        return;
    }
    fclose(f);

    e2_machine m;
    e2_machine_init(&m, 0, 65536);
    e2_load_result lr = e2_load_elf(&m, image, (size_t)size);
    CHECK(lr == E2_LOAD_OK, "load: %s", e2_load_result_name(lr));

    if (lr == E2_LOAD_OK) {
        capture cap = {0};
        e2_syscall_env env = {.exit = capture_exit, .files = &test_files, .ctx = &cap};
        e2_status st = run_program(&m, &env, 2000000);
        CHECK(st == E2_HALTED, "expected halt, got %s (%s)", e2_status_name(st),
              m.ctx.reason ? m.ctx.reason : "-");
        CHECK(cap.status == 0, "fib(10) != 55, exit status %d", cap.status);
        CHECK(strcmp(cap.out, "ok\n") == 0, "output \"%s\"", cap.out);
        printf("  (%llu instructions)\n", (unsigned long long)m.instructions);
    }

    e2_machine_destroy(&m);
    free(image);
}

static void test_simd_memset_elf(void) {
    printf("vector memset through a real ELF (dup/stp/ldrb)\n");
    FILE *f = fopen("tests/guest/memset.elf", "rb");
    if (f == NULL) {
        printf("  SKIP (tests/guest/memset.elf not built)\n");
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *image = (uint8_t *)malloc((size_t)size);
    if (image == NULL || fread(image, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        free(image);
        CHECK(0, "could not read memset.elf");
        return;
    }
    fclose(f);

    e2_machine m;
    e2_machine_init(&m, 0, 65536);
    e2_load_result lr = e2_load_elf(&m, image, (size_t)size);
    CHECK(lr == E2_LOAD_OK, "load: %s", e2_load_result_name(lr));

    if (lr == E2_LOAD_OK) {
        capture cap = {0};
        e2_syscall_env env = {.exit = capture_exit, .files = &test_files, .ctx = &cap};
        e2_status st = run_program(&m, &env, 10000);
        CHECK(st == E2_HALTED, "expected halt, got %s (%s)", e2_status_name(st),
              m.ctx.reason ? m.ctx.reason : "-");
        CHECK(cap.status == 0, "vector fill verification failed, exit %d", cap.status);
        CHECK(strcmp(cap.out, "ok\n") == 0, "output \"%s\"", cap.out);
    }

    e2_machine_destroy(&m);
    free(image);
}

/// Builds a synthetic ET_DYN image with one RELATIVE relocation and checks the
/// loader applies the bias. Kept synthetic so the test does not depend on a
/// downloaded libc.
/// Runs a freestanding C program compiled by clang, so the instruction mix is
/// whatever the compiler really emits rather than what a hand-written test
/// assumes. Checks the observable output only.
static void test_compiled_c_program(void) {
    printf("compiler-generated C program\n");
    FILE *f = fopen("tests/guest/cprog.elf", "rb");
    if (f == NULL) {
        printf("  SKIP (tests/guest/cprog.elf not built)\n");
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *image = (uint8_t *)malloc((size_t)size);
    if (image == NULL || fread(image, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        free(image);
        CHECK(0, "could not read cprog.elf");
        return;
    }
    fclose(f);

    e2_machine m;
    e2_machine_init(&m, 0, 1 << 20);
    e2_loaded_image info = {0};
    e2_load_result lr = e2_load_elf_full(&m, image, (size_t)size, true, &info);
    CHECK(lr == E2_LOAD_OK, "load: %s", e2_load_result_name(lr));

    if (lr == E2_LOAD_OK) {
        capture cap = {0};
        e2_syscall_env env = {.exit = capture_exit, .files = &test_files, .ctx = &cap};
        e2_status st = run_program(&m, &env, 500000);
        CHECK(st == E2_HALTED, "expected halt, got %s (%s)", e2_status_name(st),
              m.ctx.reason ? m.ctx.reason : "-");
        CHECK(cap.status == 0, "exit status %d", cap.status);
        // The struct-by-value path is the interesting part: make_point returns a
        // two-int struct in registers and point_sum receives it back.
        CHECK(strstr(cap.out, "sum=42") != NULL, "output was \"%s\"", cap.out);
        CHECK(strstr(cap.out, "idx=3568") != NULL, "output was \"%s\"", cap.out);
        CHECK(strstr(cap.out, "checksum=58992") != NULL, "output was \"%s\"", cap.out);
        CHECK(strstr(cap.out, "d0=3") != NULL, "output was \"%s\"", cap.out);
        CHECK(strstr(cap.out, "d1=10") != NULL, "output was \"%s\"", cap.out);
    }

    e2_machine_destroy(&m);
    free(image);
}

/// Exercises argument passing, nested calls, and callee-saved registers. A
/// program that renders digits from the wrong register usually turns out to be
/// a call-path bug rather than an arithmetic one, so this isolates it.
static void test_function_calls(void) {
    printf("function calls, arguments, callee-saved registers\n");
    FILE *f = fopen("tests/guest/calls.elf", "rb");
    if (f == NULL) {
        printf("  SKIP (tests/guest/calls.elf not built)\n");
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *image = (uint8_t *)malloc((size_t)size);
    if (image == NULL || fread(image, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        free(image);
        CHECK(0, "could not read calls.elf");
        return;
    }
    fclose(f);

    e2_machine m;
    e2_machine_init(&m, 0, 1 << 20);
    e2_loaded_image info = {0};
    e2_load_result lr = e2_load_elf_full(&m, image, (size_t)size, true, &info);
    CHECK(lr == E2_LOAD_OK, "load: %s", e2_load_result_name(lr));

    if (lr == E2_LOAD_OK) {
        capture cap = {0};
        e2_syscall_env env = {.exit = capture_exit, .files = &test_files, .ctx = &cap};
        e2_status st = run_program(&m, &env, 100000);
        CHECK(st == E2_HALTED, "expected halt, got %s (%s)", e2_status_name(st),
              m.ctx.reason ? m.ctx.reason : "-");
        CHECK(cap.status == 0, "exit status %d (the mismatching value)", cap.status);
        CHECK(strcmp(cap.out, "ok\n") == 0, "output \"%s\"", cap.out);
    }

    e2_machine_destroy(&m);
    free(image);
}

/// Multiplication families: 32-bit wraparound, 64-bit product, multiply-add,
/// multiply-subtract, and widen-multiply. The widen case is what caught the
/// signed/unsigned selector being the wrong bit.
static void test_multiplication(void) {
    printf("multiplication families\n");
    FILE *f = fopen("tests/guest/mul.elf", "rb");
    if (f == NULL) {
        printf("  SKIP (tests/guest/mul.elf not built)\n");
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *image = (uint8_t *)malloc((size_t)size);
    if (image == NULL || fread(image, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        free(image);
        CHECK(0, "could not read mul.elf");
        return;
    }
    fclose(f);

    e2_machine m;
    e2_machine_init(&m, 0, 1 << 20);
    e2_loaded_image info = {0};
    e2_load_result lr = e2_load_elf_full(&m, image, (size_t)size, true, &info);
    CHECK(lr == E2_LOAD_OK, "load: %s", e2_load_result_name(lr));

    if (lr == E2_LOAD_OK) {
        capture cap = {0};
        e2_syscall_env env = {.exit = capture_exit, .files = &test_files, .ctx = &cap};
        e2_status st = run_program(&m, &env, 100000);
        CHECK(st == E2_HALTED, "expected halt, got %s (%s)", e2_status_name(st),
              m.ctx.reason ? m.ctx.reason : "-");
        CHECK(cap.status == 0, "failing case index %d", cap.status);
    }

    e2_machine_destroy(&m);
    free(image);
}

static void test_pie_relocation(void) {
    printf("PIE load bias and RELATIVE relocation\n");
    e2_machine m;
    e2_machine_init(&m, 0, 8192);

    // Layout at bias 0: one LOAD (R+W+X, 0x200 bytes), PT_DYNAMIC at 0x100.
    // The dynamic table holds DT_RELA -> 0x180, DT_RELASZ -> 24, DT_RELAENT -> 24.
    // One RELA entry rewrites [0x200] with bias + 0x2A.
    enum { kImageSize = 0x400, kPhdrOff = 64, kLoadOff = 0x0, kDynOff = 0x100,
           kRelaOff = 0x180, kTargetOff = 0x200 };
    static uint8_t image[kImageSize];
    memset(image, 0, sizeof(image));
    image[0] = 0x7f; image[1] = 'E'; image[2] = 'L'; image[3] = 'F';
    image[4] = 2;    // 64-bit
    image[5] = 1;    // little endian
    image[6] = 1;
    image[16] = 3;   // ET_DYN
    image[18] = 183; // EM_AARCH64
    image[32] = kPhdrOff;
    image[54] = 56;  // e_phentsize
    image[56] = 2;   // e_phnum

    // phdr 0: PT_LOAD covering the whole image
    uint8_t *ph0 = image + kPhdrOff;
    ph0[0] = 1;                       // PT_LOAD
    ph0[4] = 7;                       // R|W|X
    *(uint64_t *)(ph0 + 8) = kLoadOff;
    *(uint64_t *)(ph0 + 16) = 0;      // vaddr
    *(uint64_t *)(ph0 + 32) = kImageSize;
    *(uint64_t *)(ph0 + 40) = kImageSize;

    // phdr 1: PT_DYNAMIC
    uint8_t *ph1 = ph0 + 56;
    ph1[0] = 2;                       // PT_DYNAMIC
    ph1[4] = 6;                       // R|W
    *(uint64_t *)(ph1 + 16) = kDynOff;
    *(uint64_t *)(ph1 + 32) = 0x40;
    *(uint64_t *)(ph1 + 40) = 0x40;

    // dynamic entries
    typedef struct { int64_t tag; uint64_t val; } dyn_entry;
    typedef struct { uint64_t off; uint64_t info; int64_t addend; } rela_entry;
    dyn_entry *dyn = (dyn_entry *)(image + kDynOff);
    dyn[0].tag = 7;  dyn[0].val = kRelaOff;   // DT_RELA
    dyn[1].tag = 8;  dyn[1].val = 24;         // DT_RELASZ
    dyn[2].tag = 9;  dyn[2].val = 24;         // DT_RELAENT
    dyn[3].tag = 0;  dyn[3].val = 0;          // DT_NULL

    rela_entry *rela = (rela_entry *)(image + kRelaOff);
    rela[0].off = kTargetOff;
    rela[0].info = 1027;              // R_AARCH64_RELATIVE
    rela[0].addend = 0x2A;

    e2_loaded_image info = {0};
    e2_load_result lr = e2_load_elf_full(&m, image, kImageSize, true, &info);
    CHECK(lr == E2_LOAD_OK, "load: %s", e2_load_result_name(lr));
    CHECK(info.load_bias == E2_PIE_BASE, "bias 0x%llx", (unsigned long long)info.load_bias);

    e2_load_result rr = e2_apply_relative_relocations(&m, image, kImageSize, &info);
    CHECK(rr == E2_LOAD_OK, "relocations: %s", e2_load_result_name(rr));

    uint64_t got = 0;
    e2_memory_read(&m.mem, E2_PIE_BASE + kTargetOff, &got, 8);
    CHECK(got == E2_PIE_BASE + 0x2A, "relocated value 0x%llx, want 0x%llx",
          (unsigned long long)got, (unsigned long long)(E2_PIE_BASE + 0x2A));

    e2_machine_destroy(&m);
}

static void test_reject_dynamic(void) {
    printf("dynamic executables are refused with a clear reason\n");
    // A crafted header: valid ELF64 aarch64 EXEC with one PT_INTERP segment.
    uint8_t image[64 + 2 * 56] = {0};
    image[0] = 0x7f; image[1] = 'E'; image[2] = 'L'; image[3] = 'F';
    image[4] = 2;    // 64-bit
    image[5] = 1;    // little endian
    image[6] = 1;
    // e_type = ET_EXEC
    image[16] = 2;
    // e_machine = 183 (AArch64)
    image[18] = 183; image[19] = 0;
    // e_phoff = 64
    image[32] = 64;
    // e_phentsize = 56
    image[54] = 56;
    // e_phnum = 2
    image[56] = 2;
    // first phdr: PT_INTERP
    image[64] = 3;

    e2_machine m;
    e2_machine_init(&m, 0, 4096);
    e2_load_result lr = e2_load_elf(&m, image, sizeof(image));
    CHECK(lr == E2_LOAD_NEEDS_INTERP, "expected needs-interpreter, got %s",
          e2_load_result_name(lr));
    e2_machine_destroy(&m);
}

static void test_reject_wrong_machine(void) {
    printf("wrong architecture is refused\n");
    uint8_t image[64] = {0};
    image[0] = 0x7f; image[1] = 'E'; image[2] = 'L'; image[3] = 'F';
    image[4] = 2;
    image[5] = 1;
    image[16] = 2;
    image[18] = 62;  // EM_X86_64
    e2_machine m;
    e2_machine_init(&m, 0, 4096);
    e2_load_result lr = e2_load_elf(&m, image, sizeof(image));
    CHECK(lr == E2_LOAD_UNSUPPORTED, "expected unsupported, got %s",
          e2_load_result_name(lr));
    e2_machine_destroy(&m);
}

static void test_shll(void);
static void test_scalar_immediates(void);

// getcwd and readlinkat are what os.getcwd() and os.readlink() land on.
// Python computes sys.path from both, and an ENOSYS there fails before any file
// is opened, so the syscall layer has to answer them even though nothing here
// touches a real filesystem.
static void path_syscall_checks(void) {
    e2_machine m;
    e2_machine_init(&m, 0, 1 << 20);
    const uint64_t at = 0x30000000ULL, buf = 0x10400000ULL;
    e2_memory_map_at(&m.mem, at, 0x1000, E2_PROT_READ | E2_PROT_WRITE | E2_PROT_EXEC);
    e2_memory_map_at(&m.mem, buf, 0x2000, E2_PROT_READ | E2_PROT_WRITE);

    e2_syscall_env env = {0};
    snprintf(env.cwd, sizeof(env.cwd), "/usr/lib");

    // getcwd(buf, 64) -> the buffer address, with the path written into it
    m.cpu.x[8] = E2_SYS_getcwd;
    m.cpu.x[0] = buf;
    m.cpu.x[1] = 64;
    CHECK(e2_service_syscall(&m, &env) == E2_OK, "getcwd returns OK");
    CHECK(m.cpu.x[0] == buf, "getcwd returns the buffer address");
    char got[64] = {0};
    e2_memory_read(&m.mem, buf, got, sizeof(got));
    CHECK(strcmp(got, "/usr/lib") == 0, "getcwd writes the current directory");

    // getcwd with a buffer that cannot hold the path: ERANGE, not a truncation
    m.cpu.x[0] = buf;
    m.cpu.x[1] = 2;
    CHECK(e2_service_syscall(&m, &env) == E2_OK, "short getcwd returns OK");
    CHECK((long long)m.cpu.x[0] == -34, "short getcwd reports ERANGE");

    e2_memory_destroy(&m.mem);
}


int main(void) {
    path_syscall_checks();
    printf("== e2 interpreter core tests ==\n");
    test_movz_and_ret();
    test_add_sub();
    test_movn_movk();
    test_loop_with_branch();
    test_flags_and_condition();
    test_load_store();
    test_svc();
    test_hint_space_is_noop();
    test_jump_table_matches_a_real_run();
    test_bitwise_select_matches_a_real_run();
    test_ldp_stp_match_a_real_run();
    test_simd_matches_a_real_run();
    test_permutes_match_a_real_run();
    test_division_and_shifts_match_a_real_run();
    test_bitfield_matches_a_real_run();
    test_ushll2_widens_from_the_upper_half();
    test_movi_64bit_immediate();
    test_ldrsb_and_tbz_together();
    test_ld1_lane_loads_one_lane();
    test_sysinfo_writes_the_struct();
    test_barriers_retire();
    test_ldrsb_backward_scan();
    test_frame_pointer_prologue();
    test_tbz_and_tbnz();
    test_tbz_on_bit_31();
    test_widen_multiply();
    test_saturating_narrow();
    test_simd_permute();
    test_xtn_narrows_lanes();
    test_clock_gettime_honours_the_id();
    test_statfs_layout();
    test_truncate_forms();
    test_newfstatat_honours_no_follow();
    test_chdir_checks_the_directory_exists();
    test_lseek_reports_the_new_position();
    test_nop();
    test_unimplemented();
    test_hello_world();
    test_exit_status();
    test_unknown_syscall();
    test_load_real_elf();
    test_recursive_fib_elf();
    test_simd_memset_elf();
    test_multiplication();
    test_function_calls();
    test_compiled_c_program();
    test_pie_relocation();
    test_reject_dynamic();
    test_reject_wrong_machine();

    test_shll();
    test_scalar_immediates();

    // A list that ran almost nothing reports "0 checks, 0 failure(s)", which reads like a
    // clean run. The suites below have the same guard for the same reason.
    // One constant, used by both the test and the message: a hardcoded literal in the
    // text would drift from the threshold and describe a check that is not the one made.
    enum { MIN_CHECKS = 50 };
    if (checks < MIN_CHECKS) {
        fprintf(stderr, "only %d check(s) ran, expected at least %d - is the call list intact?\n",
                checks, (int)MIN_CHECKS);
        return 1;
    }
    printf("\n%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

// SSHLL/USHLL select by bit31=0, bits28-23=011110 and opcode bits15-10=101001.
// Getting the mask wrong once meant the shift-by-4 form was never recognised,
// so this pins the behaviour: widen 32-bit lanes to 64-bit, signed and unsigned.
static void test_shll(void) {
    FILE *f = fopen("tests/guest/shll.elf", "rb");
    if (!f) {
        printf("  SKIP (tests/guest/shll.elf not built)\n");
        return;
    }
    fclose(f);
    FILE *in = fopen("tests/guest/shll.elf", "rb");
    fseek(in, 0, SEEK_END);
    long sz = ftell(in);
    fseek(in, 0, SEEK_SET);
    uint8_t *img = malloc((size_t)sz);
    if (fread(img, 1, (size_t)sz, in) != (size_t)sz) {
        fclose(in);
        free(img);
        CHECK(0, "could not read shll.elf");
        return;
    }
    fclose(in);

    e2_machine m;
    e2_machine_init(&m, 0, 1 << 20);
    e2_loaded_image info = {0};
    if (e2_load_elf_full(&m, img, (size_t)sz, true, &info) != E2_LOAD_OK) {
        free(img);
        CHECK(0, "could not load shll.elf");
        return;
    }
    free(img);

    // Run to exit, capturing what the guest writes to fd 1.
    static char captured[128];
    size_t captured_len = 0;
    struct capture_ctx { char *buf; size_t *len; } cx = {captured, &captured_len};
    (void)cx;

    int syscalls = 0;
    for (long i = 0; i < 200000; i++) {
        e2_status st = e2_step(&m);
        if (st == E2_SYSCALL) {
            if (m.cpu.x[8] == 64) {
                const uint64_t fd = m.cpu.x[0];
                const uint64_t buf = m.cpu.x[1];
                const uint64_t n = m.cpu.x[2];
                if (fd == 1 && captured_len + n < sizeof(captured)) {
                    e2_memory_read(&m.mem, buf, captured + captured_len, (size_t)n);
                    captured_len += (size_t)n;
                }
                m.cpu.x[0] = (uint64_t)n;
                syscalls++;
                continue;
            }
            if (m.cpu.x[8] == 93 || m.cpu.x[8] == 94) break;
            syscalls++;
            continue;
        }
        if (st != E2_OK) break;
    }
    captured[captured_len] = '\0';
    CHECK(strcmp(captured, "1,-1|16,-16|1,4294967295\n") == 0,
          "sshll/ushll widen 32-bit lanes to 64-bit");
    if (strcmp(captured, "1,-1|16,-16|1,4294967295\n") != 0) {
        printf("    got: %.80s\n", captured);
    }
    e2_memory_destroy(&m.mem);
}

// Executes a single instruction and returns the resulting state. Guest programs
// are the wrong tool for checking one encoding: an earlier attempt to verify
// FMOV immediate through a compiled program reported wrong values while the
// interpreter was in fact correct. Driving e2_step directly has no codegen in
// the way.
static e2_status run_single_insn(uint32_t insn, e2_machine *m, uint64_t *v0_out) {
    e2_machine_init(m, 0, 1 << 20);
    const uint64_t at = 0x30000000ULL;
    if (e2_memory_map_at(&m->mem, at, 0x1000,
                         E2_PROT_READ | E2_PROT_WRITE | E2_PROT_EXEC) != 0) {
        return E2_FAULT;
    }
    e2_memory_write(&m->mem, at, &insn, 4);
    m->cpu.pc = at;
    e2_status st = e2_step(m);
    if (v0_out != NULL) *v0_out = m->cpu.v[0][0];
    return st;
}

static void test_scalar_immediates(void) {
    static const struct { uint32_t insn; uint64_t want; const char *what; } cases[] = {
        {0x1E2C1000u, 0x000000003F000000ULL, "fmov s0, #0.5"},
        {0x1E2E1000u, 0x000000003F800000ULL, "fmov s0, #1.0"},
        {0x1E201000u, 0x0000000040000000ULL, "fmov s0, #2.0"},
        {0x1E3C1000u, 0x00000000BF000000ULL, "fmov s0, #-0.5"},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        e2_machine m;
        uint64_t got = 0;
        const e2_status st = run_single_insn(cases[i].insn, &m, &got);
        char label[96];
        snprintf(label, sizeof(label), "%s expands to the right bit pattern", cases[i].what);
        CHECK(st == E2_OK && got == cases[i].want, "%s", label);
        if (st != E2_OK || got != cases[i].want) {
            printf("    got 0x%016llx want 0x%016llx\n",
                   (unsigned long long)got, (unsigned long long)cases[i].want);
        }
        e2_memory_destroy(&m.mem);
    }

    // LSL by an immediate lowers to UBFM with imms >= immr, which is the form
    // that shows up right before a table lookup in Python's runtime.
    {
        static const struct { uint64_t in, want; } lsl[] = {
            {0x0ULL, 0x0ULL},
            {0x3ULL, 0x30ULL},
            {0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFF0ULL},
            {0x1000000000000000ULL, 0x0ULL},
        };
        for (unsigned i = 0; i < sizeof(lsl) / sizeof(lsl[0]); i++) {
            e2_machine m;
            e2_machine_init(&m, 0, 1 << 20);
            const uint64_t at = 0x30000000ULL;
            e2_memory_map_at(&m.mem, at, 0x1000, E2_PROT_READ | E2_PROT_WRITE | E2_PROT_EXEC);
            const uint32_t insn = 0xD37CEE80u;   // lsl x0, x20, #4
            e2_memory_write(&m.mem, at, &insn, 4);
            m.cpu.pc = at;
            m.cpu.x[20] = lsl[i].in;
            const e2_status st = e2_step(&m);
            CHECK(st == E2_OK && m.cpu.x[0] == lsl[i].want,
                  "lsl x0, x20, #4 with 0x%llx", (unsigned long long)lsl[i].in);
            e2_memory_destroy(&m.mem);
        }
    }

    // The destination width of a load follows the instruction, not the access
    // size: `ldrsb x2, [x2, #32]` must sign-extend a byte to all 64 bits even
    // though it touches one byte. Getting this wrong left bit 63 clear, which
    // kept Python's hash probe walking a table forever.
    {
        static const struct { uint32_t insn; uint8_t byte; uint64_t want; const char *what; } ls[] = {
            {0x39808042u, 0xFF, 0xFFFFFFFFFFFFFFFFULL, "ldrsb x2, [x2, #32] sign-extends to 64"},
            {0x39808042u, 0x80, 0xFFFFFFFFFFFFFF80ULL, "ldrsb x2, [x2, #32] 0x80"},
            {0x39808042u, 0x7F, 0x000000000000007FULL, "ldrsb x2, [x2, #32] 0x7F"},
            {0x39C08042u, 0xFF, 0x00000000FFFFFFFFULL, "ldrsb w2, [x2, #32] stays 32-bit"},
        };
        for (unsigned i = 0; i < sizeof(ls) / sizeof(ls[0]); i++) {
            e2_machine m;
            e2_machine_init(&m, 0, 1 << 20);
            const uint64_t at = 0x30000000ULL, mem = 0x10400000ULL;
            e2_memory_map_at(&m.mem, at, 0x1000, E2_PROT_READ | E2_PROT_WRITE | E2_PROT_EXEC);
            e2_memory_map_at(&m.mem, mem, 0x1000, E2_PROT_READ | E2_PROT_WRITE);
            e2_memory_write(&m.mem, at, &ls[i].insn, 4);
            e2_memory_write(&m.mem, mem + 32, &ls[i].byte, 1);
            m.cpu.pc = at;
            m.cpu.x[2] = mem;
            const e2_status st = e2_step(&m);
            CHECK(st == E2_OK && m.cpu.x[2] == ls[i].want, "%s", ls[i].what);
            e2_memory_destroy(&m.mem);
        }
    }


    // Five instruction groups found by running busybox applets rather than by
    // reading anything: `seq` needs every one of them.
    {
        // fmov d9, d8 - a plain register-to-register move in the FP one-source
        // group, which had no general handler before this.
        e2_machine m;
        e2_machine_init(&m, 0, 1 << 20);
        const uint64_t at = 0x30000000ULL;
        e2_memory_map_at(&m.mem, at, 0x1000, E2_PROT_READ | E2_PROT_WRITE | E2_PROT_EXEC);
        uint32_t insn = 0x1E604109u;
        e2_memory_write(&m.mem, at, &insn, 4);
        m.cpu.pc = at;
        m.cpu.v[8][0] = 0x400921FB54442D18ULL;
        e2_step(&m);
        CHECK(m.cpu.v[9][0] == 0x400921FB54442D18ULL, "fmov d9, d8 copies the register");
        e2_memory_destroy(&m.mem);
    }
    {
        // fmov v0.d[1], x3 then back: the upper half of a 128-bit register has
        // its own pair of encodings, distinct from the plain D form.
        e2_machine m;
        e2_machine_init(&m, 0, 1 << 20);
        const uint64_t at = 0x30000000ULL;
        e2_memory_map_at(&m.mem, at, 0x2000, E2_PROT_READ | E2_PROT_WRITE | E2_PROT_EXEC);
        uint32_t a = 0x9EAF0060u, b = 0x9EAE0003u;
        e2_memory_write(&m.mem, at, &a, 4);
        e2_memory_write(&m.mem, at + 4, &b, 4);
        m.cpu.pc = at;
        m.cpu.x[3] = 0xDEADBEEFCAFEF00DULL;
        m.cpu.v[0][1] = 0;
        e2_step(&m);
        CHECK(m.cpu.v[0][1] == 0xDEADBEEFCAFEF00DULL, "fmov v0.d[1], x3 writes the upper half");
        m.cpu.x[3] = 0;
        e2_step(&m);
        CHECK(m.cpu.x[3] == 0xDEADBEEFCAFEF00DULL, "fmov x3, v0.d[1] reads it back");
        e2_memory_destroy(&m.mem);
    }
    {
        // adc x6, x10, x8 - the carry group, which has its own prefix.
        e2_machine m;
        e2_machine_init(&m, 0, 1 << 20);
        const uint64_t at = 0x30000000ULL;
        e2_memory_map_at(&m.mem, at, 0x1000, E2_PROT_READ | E2_PROT_WRITE | E2_PROT_EXEC);
        uint32_t insn = 0x9A080146u;
        e2_memory_write(&m.mem, at, &insn, 4);
        m.cpu.pc = at;
        m.cpu.x[10] = 5;
        m.cpu.x[8] = 7;
        m.cpu.flags.c = true;
        e2_step(&m);
        CHECK(m.cpu.x[6] == 13, "adc includes the carry in");
        m.cpu.x[10] = 5;
        m.cpu.x[8] = 7;
        m.cpu.flags.c = false;
        m.cpu.pc = at;
        e2_step(&m);
        CHECK(m.cpu.x[6] == 12, "adc without carry");
        e2_memory_destroy(&m.mem);
    }
    {
        // fmadd d0, d1, d8, d9 - fused, so the product is not rounded first.
        e2_machine m;
        e2_machine_init(&m, 0, 1 << 20);
        const uint64_t at = 0x30000000ULL;
        e2_memory_map_at(&m.mem, at, 0x1000, E2_PROT_READ | E2_PROT_WRITE | E2_PROT_EXEC);
        uint32_t insn = 0x1F482420u;
        e2_memory_write(&m.mem, at, &insn, 4);
        m.cpu.pc = at;
        double a = 3.0, b = 4.0, add = 5.0;
        memcpy(&m.cpu.v[1][0], &a, 8);
        memcpy(&m.cpu.v[8][0], &b, 8);
        memcpy(&m.cpu.v[9][0], &add, 8);
        e2_step(&m);
        double got;
        memcpy(&got, &m.cpu.v[0][0], 8);
        CHECK(got == 17.0, "fmadd computes a*b+add");
        e2_memory_destroy(&m.mem);
    }

    {
        // fcvtzs d0, d0 - FP to integer, result kept as a float. `sleep`
        // converts a double to whole seconds through this.
        // fcvtzs leaves an INTEGER in the FP register, so its result is checked
        // as bits rather than as a double; the rounding forms below stay in the
        // FP domain and are checked as doubles.
        static const struct { uint32_t insn; double in; double want; const char *what; } cv[] = {
            {0x5E61B800u, -3.2, -4.0, "fcvtms rounds toward -inf"},
            {0x5EE1A800u,  3.2,  4.0, "fcvtps rounds toward +inf"},
            {0x5E61A800u,  2.5,  2.0, "fcvtns rounds to even"},
            {0x5E61C800u,  2.5,  3.0, "fcvtas rounds half away from zero"},
        };
        {
            // fcvtzs produces an integer, so assert on the raw bits.
            e2_machine m;
            e2_machine_init(&m, 0, 1 << 20);
            const uint64_t at = 0x30000000ULL;
            e2_memory_map_at(&m.mem, at, 0x1000, E2_PROT_READ | E2_PROT_WRITE | E2_PROT_EXEC);
            uint32_t tz = 0x5EE1B800u;   // fcvtzs d0, d0
            e2_memory_write(&m.mem, at, &tz, 4);
            const double a = 3.7, b = -3.7;
            memcpy(&m.cpu.v[0][0], &a, 8);
            m.cpu.pc = at;
            e2_step(&m);
            CHECK(m.cpu.v[0][0] == 3, "fcvtzs truncates toward zero");
            memcpy(&m.cpu.v[0][0], &b, 8);
            m.cpu.pc = at;
            e2_step(&m);
            CHECK(m.cpu.v[0][0] == (uint64_t)(int64_t)-3,
                  "fcvtzs truncates toward zero (negative)");
            e2_memory_destroy(&m.mem);
        }
        for (unsigned i = 0; i < sizeof(cv) / sizeof(cv[0]); i++) {
            e2_machine m;
            e2_machine_init(&m, 0, 1 << 20);
            const uint64_t at = 0x30000000ULL;
            e2_memory_map_at(&m.mem, at, 0x1000, E2_PROT_READ | E2_PROT_WRITE | E2_PROT_EXEC);
            e2_memory_write(&m.mem, at, &cv[i].insn, 4);
            m.cpu.pc = at;
            memcpy(&m.cpu.v[0][0], &cv[i].in, 8);
            const e2_status st = e2_step(&m);
            double got;
            memcpy(&got, &m.cpu.v[0][0], 8);
            CHECK(st == E2_OK && got == cv[i].want, "%s", cv[i].what);
            e2_memory_destroy(&m.mem);
        }
    }

    {
        // shl d1, d1, #52 - SIMD shift by immediate. printf scales a mantissa
        // with it, so getting the (imm - esize) rule wrong shows up as a wrong
        // decimal rather than as a crash.
        e2_machine m;
        e2_machine_init(&m, 0, 1 << 20);
        const uint64_t at = 0x30000000ULL;
        e2_memory_map_at(&m.mem, at, 0x1000, E2_PROT_READ | E2_PROT_WRITE | E2_PROT_EXEC);
        uint32_t insn = 0x5F745400u;   // shl d0, d0, #52
        e2_memory_write(&m.mem, at, &insn, 4);
        m.cpu.pc = at;
        m.cpu.v[0][0] = 1;
        e2_step(&m);
        CHECK(m.cpu.v[0][0] == (1ULL << 52), "shl d0, d0, #52 shifts by imm minus element size");

        // ushr d0, d0, #4 - same family, logical right shift
        uint32_t ushr = 0x7F7C0400u;
        e2_memory_write(&m.mem, at, &ushr, 4);
        m.cpu.pc = at;
        m.cpu.v[0][0] = 0x80;
        e2_step(&m);
        CHECK(m.cpu.v[0][0] == 0x8, "ushr d0, d0, #4 shifts logically");
        e2_memory_destroy(&m.mem);
    }
    {
        // scvtf d0, w0, #2 - integer scaled down by 2^fbits. `%g` needs it.
        e2_machine m;
        e2_machine_init(&m, 0, 1 << 20);
        const uint64_t at = 0x30000000ULL;
        e2_memory_map_at(&m.mem, at, 0x1000, E2_PROT_READ | E2_PROT_WRITE | E2_PROT_EXEC);
        uint32_t insn = 0x1E42F800u;
        e2_memory_write(&m.mem, at, &insn, 4);
        m.cpu.pc = at;
        m.cpu.x[0] = 5;
        e2_step(&m);
        double got;
        memcpy(&got, &m.cpu.v[0][0], 8);
        CHECK(got == 1.25, "scvtf d0, w0, #2 divides by four");
        e2_memory_destroy(&m.mem);
    }

    {
        // mvni v0.4s, #0 - MOVI with the op bit set, so the result is the
        // complement. Ignoring that bit made it produce zero, and the regex
        // engine uses this to write a -1 marker into a freshly built state.
        e2_machine m;
        e2_machine_init(&m, 0, 1 << 20);
        const uint64_t at = 0x30000000ULL;
        e2_memory_map_at(&m.mem, at, 0x1000, E2_PROT_READ | E2_PROT_WRITE | E2_PROT_EXEC);
        uint32_t insn = 0x6F000400u;   // mvni v0.4s, #0
        e2_memory_write(&m.mem, at, &insn, 4);
        m.cpu.pc = at;
        e2_step(&m);
        CHECK(m.cpu.v[0][0] == 0xFFFFFFFFFFFFFFFFULL,
              "mvni v0.4s, #0 sets every bit");
        CHECK(m.cpu.v[0][1] == 0xFFFFFFFFFFFFFFFFULL,
              "mvni v0.4s, #0 sets the upper half too");

        // The same immediate with op clear stays a plain move.
        uint32_t movi = 0x4F000400u;   // movi v0.4s, #0
        e2_memory_write(&m.mem, at, &movi, 4);
        m.cpu.pc = at;
        m.cpu.v[0][0] = ~0ULL;
        m.cpu.v[0][1] = ~0ULL;
        e2_step(&m);
        CHECK(m.cpu.v[0][0] == 0 && m.cpu.v[0][1] == 0,
              "movi v0.4s, #0 clears the register");
        e2_memory_destroy(&m.mem);
    }

    {
        // DCZID_EL0 must report DZP when DC ZVA is not implemented. Saying it
        // works makes memset take a fast path that then writes nothing, which
        // is silent and only shows up much later as uninitialised state.
        e2_machine m;
        e2_machine_init(&m, 0, 1 << 20);
        const uint64_t at = 0x30000000ULL;
        e2_memory_map_at(&m.mem, at, 0x1000, E2_PROT_READ | E2_PROT_WRITE | E2_PROT_EXEC);
        uint32_t insn = 0xD53B00E0u;   // mrs x0, dczid_el0
        e2_memory_write(&m.mem, at, &insn, 4);
        m.cpu.pc = at;
        e2_step(&m);
        CHECK((m.cpu.x[0] & 0x10) != 0, "DCZID_EL0 reports DZP: DC ZVA is prohibited");
        e2_memory_destroy(&m.mem);
    }

    {
        // The four Advanced SIMD logical operations share an encoding and are
        // selected by the size field. The regex engine builds its character
        // sets with these, so getting one wrong makes matching fail silently.
        static const struct { uint32_t insn; uint64_t a, b, want; const char *what; } ops[] = {
            {0x4E221C20u, 0xFF00FF00FF00FF00ULL, 0x0F0F0F0F0F0F0F0FULL,
             0x0F000F000F000F00ULL, "and v.16b"},
            {0x4E621C20u, 0xFF00FF00FF00FF00ULL, 0x0F0F0F0F0F0F0F0FULL,
             0xF000F000F000F000ULL, "bic v.16b"},
            {0x4EA21C20u, 0xFF00FF00FF00FF00ULL, 0x0F0F0F0F0F0F0F0FULL,
             0xFF0FFF0FFF0FFF0FULL, "orr v.16b"},
        };
        for (unsigned i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
            e2_machine m;
            e2_machine_init(&m, 0, 1 << 20);
            const uint64_t at = 0x30000000ULL;
            e2_memory_map_at(&m.mem, at, 0x1000, E2_PROT_READ | E2_PROT_WRITE | E2_PROT_EXEC);
            e2_memory_write(&m.mem, at, &ops[i].insn, 4);
            m.cpu.pc = at;
            m.cpu.v[1][0] = ops[i].a;
            m.cpu.v[2][0] = ops[i].b;
            e2_step(&m);
            CHECK(m.cpu.v[0][0] == ops[i].want, "%s", ops[i].what);
            e2_memory_destroy(&m.mem);
        }
    }

    {
        // cmeq v0.16b, v1.16b, v2.16b - per-byte compare. A regex DFA tests a
        // byte against the set this way, so a broken compare means a pattern
        // can never match a literal character.
        e2_machine m;
        e2_machine_init(&m, 0, 1 << 20);
        const uint64_t at = 0x30000000ULL;
        e2_memory_map_at(&m.mem, at, 0x1000, E2_PROT_READ | E2_PROT_WRITE | E2_PROT_EXEC);
        uint32_t insn = 0x6E228C20u;
        e2_memory_write(&m.mem, at, &insn, 4);
        m.cpu.pc = at;
        // Bytes 0, 2, 4 and 6 agree, the rest do not, so those lanes come back
        // all ones and the others zero.
        m.cpu.v[1][0] = 0x0102030405060708ULL;
        m.cpu.v[2][0] = 0xFF02FF04FF06FF08ULL;
        e2_step(&m);
        CHECK(m.cpu.v[0][0] == 0x00FF00FF00FF00FFULL,
              "cmeq v.16b marks equal bytes with 0xFF");
        e2_memory_destroy(&m.mem);
    }

    {
        // subs x2, x2, #64 - the borrow must clear C, which is what terminates
        // musl's 64-byte memset loop with `b.hi`. If C stayed set the loop would
        // run past the end of the buffer.
        static const struct { uint64_t a; uint64_t want; bool want_c; const char *what; } cases[] = {
            {0xE0, 0xA0, true,  "224 - 64 = 160, no borrow"},
            {0x20, 0xFFFFFFFFFFFFFFE0ULL, false, "32 - 64 = -32, borrow clears C"},
            {0x40, 0x00, true, "64 - 64 = 0, no borrow"},
        };
        for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            e2_machine m;
            e2_machine_init(&m, 0, 1 << 20);
            const uint64_t at = 0x30000000ULL;
            e2_memory_map_at(&m.mem, at, 0x1000, E2_PROT_READ | E2_PROT_WRITE | E2_PROT_EXEC);
            // subs x2, x2, #64 - the encoding the assembler produces, not a
            // guess: getting the immediate field wrong here would have made the
            // test agree with a wrong decoder.
            uint32_t insn = 0xF1010042u;
            e2_memory_write(&m.mem, at, &insn, 4);
            m.cpu.pc = at;
            m.cpu.x[2] = cases[i].a;
            e2_step(&m);
            CHECK(m.cpu.x[2] == cases[i].want && m.cpu.flags.c == cases[i].want_c,
                  "%s", cases[i].what);
            e2_memory_destroy(&m.mem);
        }
    }

    {
        // The 64-bit logical immediates musl's word-at-a-time strlen relies on.
        // `tst x2, #0x8080808080808080` is what detects a zero byte inside a
        // word; if the immediate decodes wrong, strlen never finds the
        // terminator and every string looks empty.
        e2_machine m;
        e2_machine_init(&m, 0, 1 << 20);
        const uint64_t at = 0x30000000ULL;
        e2_memory_map_at(&m.mem, at, 0x1000, E2_PROT_READ | E2_PROT_WRITE | E2_PROT_EXEC);
        uint32_t insn = 0xF201C05Fu;   // tst x2, #0x8080808080808080
        e2_memory_write(&m.mem, at, &insn, 4);
        m.cpu.pc = at;
        m.cpu.x[2] = 0x0000000000000000ULL;
        e2_step(&m);
        CHECK(m.cpu.flags.z, "tst with an all-zero word sets Z");

        m.cpu.pc = at;
        m.cpu.x[2] = 0x8080808080808080ULL;
        e2_step(&m);
        CHECK(!m.cpu.flags.z, "tst with the high bits set clears Z");
        e2_memory_destroy(&m.mem);
    }

    {
        // The conditional-select family. A clobbered destination or a mixed-up
        // "condition true" branch here would silently corrupt a register and
        // show up much later as a wrong value - the shape of every unexplained
        // value bug found so far.
        static const struct {
            uint32_t insn; uint64_t a, b; bool z; uint64_t want; const char *what;
        } cs[] = {
            {0x9A820020u, 0x11, 0x22, true,  0x11, "csel picks Rn when the condition holds"},
            {0x9A820020u, 0x11, 0x22, false, 0x22, "csel picks Rm otherwise"},
            {0x9A820420u, 0x11, 0x22, true,  0x11, "csinc picks Rn when the condition holds"},
            {0x9A820420u, 0x11, 0x22, false, 0x23, "csinc adds one to Rm otherwise"},
            {0xDA820020u, 0x11, 0x22, true,  0x11, "csinv picks Rn"},
            {0xDA820420u, 0x11, 0x22, true,  0x11, "csneg picks Rn when the condition holds"},
            {0xDA9F13E0u, 0, 0, true, ~0ULL, "csetm sets all ones when true"},
            {0xDA9F13E0u, 0, 0, false, 0, "csetm sets zero otherwise"},
        };
        for (unsigned i = 0; i < sizeof(cs) / sizeof(cs[0]); i++) {
            e2_machine m;
            e2_machine_init(&m, 0, 1 << 20);
            const uint64_t at = 0x30000000ULL;
            e2_memory_map_at(&m.mem, at, 0x1000, E2_PROT_READ | E2_PROT_WRITE | E2_PROT_EXEC);
            e2_memory_write(&m.mem, at, &cs[i].insn, 4);
            m.cpu.pc = at;
            m.cpu.x[1] = cs[i].a;
            m.cpu.x[2] = cs[i].b;
            m.cpu.flags.z = cs[i].z;
            m.cpu.flags.c = false;
            m.cpu.flags.n = false;
            m.cpu.flags.v = false;
            e2_step(&m);
            CHECK(m.cpu.x[0] == cs[i].want, "%s", cs[i].what);
            e2_memory_destroy(&m.mem);
        }
    }

    {
        // Bitfield extraction and the bit-manipulation one-sources. A regex DFA
        // pulls state indices out of packed tables with these, so a wrong width
        // or offset yields a plausible-looking but wrong index - which is
        // exactly the class of failure this engine shows.
        static const struct {
            uint32_t insn; uint64_t in; uint64_t want; const char *what;
        } bf[] = {
            {0x53042C20u, 0x0000AB00ULL, 0x000000B0ULL, "ubfx w0, w1, #4, #8"},
            {0x13042C20u, 0x0000AB00ULL, 0xFFFFFFB0ULL, "sbfx sign extends the extracted field"},
            {0x53042C20u, 0x0000F300ULL, 0x00000030ULL, "ubfx takes only the requested width"},
            {0x531C1C20u, 0x000000ABULL, 0x00000AB0ULL, "ubfiz w0, w1, #4, #8"},
            {0x5AC01020u, 0x00000001ULL, 0x0000001FULL, "clz w0, w1"},
            {0x5AC00020u, 0x00000002ULL, 0x40000000ULL, "rbit w0, w1"},
            {0x5AC00820u, 0x11223344ULL, 0x44332211ULL, "rev w0, w1"},
            {0x5AC00420u, 0x11223344ULL, 0x22114433ULL, "rev16 w0, w1"},
            {0x13001C20u, 0x000000FFULL, 0xFFFFFFFFULL, "sxtb w0, w1 sign extends"},
            {0x13001C20u, 0x0000007FULL, 0x0000007FULL, "sxtb keeps a positive byte"},
            {0x53003C62u, 0xFFFFFFFFFFFF1234ULL, 0x0000000000001234ULL, "uxth w2, w3"}, 
        };
        for (unsigned i = 0; i < sizeof(bf) / sizeof(bf[0]); i++) {
            e2_machine m;
            e2_machine_init(&m, 0, 1 << 20);
            const uint64_t at = 0x30000000ULL;
            e2_memory_map_at(&m.mem, at, 0x1000, E2_PROT_READ | E2_PROT_WRITE | E2_PROT_EXEC);
            e2_memory_write(&m.mem, at, &bf[i].insn, 4);
            m.cpu.pc = at;
            m.cpu.x[1] = bf[i].in;
            m.cpu.x[3] = bf[i].in;
            e2_step(&m);
            const uint64_t got = (bf[i].insn == 0x53003C62u) ? m.cpu.x[2] : m.cpu.x[0];
            CHECK(got == bf[i].want, "%s", bf[i].what);
            e2_memory_destroy(&m.mem);
        }
    }

    {
        // Addressing modes and extended-register arithmetic. A regex matcher
        // walks tables with indexed loads and sign-extends the index, so a wrong
        // extend mode or a pre/post-index slip reads the right table at the
        // wrong entry.
        e2_machine m;
        e2_machine_init(&m, 0, 1 << 20);
        const uint64_t code = 0x30000000ULL;
        const uint64_t data = 0x31000000ULL;
        e2_memory_map_at(&m.mem, code, 0x1000, E2_PROT_READ | E2_PROT_WRITE | E2_PROT_EXEC);
        e2_memory_map_at(&m.mem, data, 0x2000, E2_PROT_READ | E2_PROT_WRITE);
        uint32_t word = 0x11223344u;
        for (uint64_t off = 0; off < 0x40; off += 4) {
            e2_memory_write(&m.mem, data + off, &word, 4);
        }

        struct { uint32_t insn; uint64_t want; const char *what; } cases[] = {
            {0x8B22C020u, 0x30FFFE10ULL, "add x0, x1, w2, sxtw sign extends"},
            {0x8B220020u, 0x31000014ULL, "add x0, x1, w2, uxtb zero extends"},
            {0xB8627820u, 0x11223344u, "ldr w0, [x1, x2, lsl #2]"},
            {0xB8A27820u, 0x0000000011223344ULL, "ldrsw x0, [x1, x2, lsl #2]"},
            {0x78626820u, 0x00003344u, "ldrh w0, [x1, x2]"},
            {0xF8408420u, 0x1122334411223344ULL, "ldr x0, [x1], #8 is post-indexed"},
            {0xF8408C20u, 0x1122334411223344ULL, "ldr x0, [x1, #8]! is pre-indexed"},
            {0x72AACF00u, 0x56781234u, "movk replaces the high half only"},
            {0x10000080u, code + 16, "adr x0, +16"},
        };
        for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            e2_memory_write(&m.mem, code, &cases[i].insn, 4);
            m.cpu.pc = code;
            m.cpu.x[0] = 0;
            m.cpu.x[1] = data + 0x10;
            m.cpu.x[2] = 4;          // sxtw/uxtb source, and a table index
            // sxtw wants a negative value to prove the sign is kept.
            m.cpu.x[2] = (cases[i].insn == 0x8B22C020u) ? 0xFFFFFFFFFFFFFE00ULL : 4;
            m.cpu.x[0] = (cases[i].insn == 0x72AACF00u) ? 0x00001234u : 0;
            e2_step(&m);
            CHECK(m.cpu.x[0] == cases[i].want, "%s", cases[i].what);
        }
        // Post-index must have advanced the base by 8 and pre-index must have
        // applied the offset before the load; both are easy to get backwards.
        e2_memory_write(&m.mem, code, &cases[5].insn, 4);
        m.cpu.pc = code;
        m.cpu.x[1] = data + 0x10;
        e2_step(&m);
        CHECK(m.cpu.x[1] == data + 0x18, "post-index advances the base after the load");
        e2_memory_destroy(&m.mem);
    }

    {
        // 128-bit load and store. Splitting these into two 8-byte halves is fine
        // only if both halves are written; the regex engine stores a character
        // class this way and loses half of it when they are not.
        e2_machine m;
        e2_machine_init(&m, 0, 1 << 20);
        const uint64_t code = 0x30000000ULL;
        const uint64_t data = 0x31000000ULL;
        e2_memory_map_at(&m.mem, code, 0x1000, E2_PROT_READ | E2_PROT_WRITE | E2_PROT_EXEC);
        e2_memory_map_at(&m.mem, data, 0x2000, E2_PROT_READ | E2_PROT_WRITE);

        uint32_t insn = 0x3D800020u;   // str q0, [x1]
        e2_memory_write(&m.mem, code, &insn, 4);
        m.cpu.pc = code;
        m.cpu.x[1] = data;
        m.cpu.v[0][0] = 0x0123456789ABCDEFULL;
        m.cpu.v[0][1] = 0xFEDCBA9876543210ULL;
        e2_step(&m);
        uint64_t lo = 0, hi = 0;
        e2_memory_read(&m.mem, data, &lo, 8);
        e2_memory_read(&m.mem, data + 8, &hi, 8);
        CHECK(lo == 0x0123456789ABCDEFULL, "str q0 writes the low half");
        CHECK(hi == 0xFEDCBA9876543210ULL, "str q0 writes the high half");

        // The same through an offset form, which takes a different decoder path.
        uint32_t off_insn = 0x3D800C20u;   // str q0, [x1, #48]
        e2_memory_write(&m.mem, code, &off_insn, 4);
        m.cpu.pc = code;
        m.cpu.x[1] = data;
        e2_step(&m);
        e2_memory_read(&m.mem, data + 48, &lo, 8);
        e2_memory_read(&m.mem, data + 56, &hi, 8);
        CHECK(lo == 0x0123456789ABCDEFULL && hi == 0xFEDCBA9876543210ULL,
              "str q0, [x1, #48] writes both halves");

        // And the load must bring both halves back.
        uint32_t ld_insn = 0x3DC00C20u;    // ldr q0, [x1, #48]
        e2_memory_write(&m.mem, code, &ld_insn, 4);
        m.cpu.pc = code;
        m.cpu.x[1] = data;
        m.cpu.v[0][0] = 0;
        m.cpu.v[0][1] = 0;
        e2_step(&m);
        CHECK(m.cpu.v[0][0] == 0x0123456789ABCDEFULL &&
              m.cpu.v[0][1] == 0xFEDCBA9876543210ULL,
              "ldr q0, [x1, #48] reads both halves");
        e2_memory_destroy(&m.mem);
    }

    {
        // The two instructions the regex engine uses to build a 64-bit pair from
        // two 32-bit arguments. `mov v0.s[1], w2` then `sshll v0.2d, v0.2s, #0`
        // must leave lane 1 of the widened register holding w2's sign extension.
        e2_machine m;
        e2_machine_init(&m, 0, 1 << 20);
        const uint64_t at = 0x30000000ULL;
        e2_memory_map_at(&m.mem, at, 0x1000, E2_PROT_READ | E2_PROT_WRITE | E2_PROT_EXEC);

        uint32_t insn = 0x4E0C1C40u;   // mov v0.s[1], w2
        e2_memory_write(&m.mem, at, &insn, 4);
        m.cpu.pc = at;
        m.cpu.v[0][0] = 0x1111111122222222ULL;   // lane0 = 0x22222222, lane1 = 0x11111111
        m.cpu.x[2] = 0x61;
        e2_step(&m);
        CHECK(m.cpu.v[0][0] == 0x0000006122222222ULL,
              "mov v0.s[1], w2 writes lane 1 and leaves lane 0 alone");

        insn = 0x0F20A400u;            // sshll v0.2d, v0.2s, #0
        e2_memory_write(&m.mem, at, &insn, 4);
        m.cpu.pc = at;
        m.cpu.v[0][0] = 0x0000006100000022ULL;   // lane0 = 0x22, lane1 = 0x61
        m.cpu.v[0][1] = 0;
        e2_step(&m);
        CHECK(m.cpu.v[0][0] == 0x0000000000000022ULL, "sshll widens lane 0 into the low half");
        fprintf(stderr, "[sshll] got hi=0x%llx lo=0x%llx\n",
                (unsigned long long)m.cpu.v[0][1], (unsigned long long)m.cpu.v[0][0]);
        CHECK(m.cpu.v[0][1] == 0x0000000000000061ULL, "sshll widens lane 1 into the high half");
        e2_memory_destroy(&m.mem);
    }

    {
        // str x0, [sp] - an ordinary 64-bit store to the stack pointer. It looks
        // too simple to be wrong, which is exactly why it was the last place
        // anyone looked when a stack slot kept reading back as zero.
        e2_machine m;
        e2_machine_init(&m, 0, 1 << 20);
        const uint64_t at = 0x30000000ULL;
        e2_memory_map_at(&m.mem, at, 0x1000, E2_PROT_READ | E2_PROT_WRITE | E2_PROT_EXEC);
        uint32_t insn = 0xF90003E0u;   // str x0, [sp]
        e2_memory_write(&m.mem, at, &insn, 4);
        m.cpu.pc = at;
        m.cpu.x[0] = 0x520200;
        const uint64_t sp_before = m.cpu.sp;
        uint64_t slot = 0xBAD;
        e2_memory_write(&m.mem, sp_before, &slot, 8);
        e2_step(&m);
        e2_memory_read(&m.mem, sp_before, &slot, 8);
        CHECK(slot == 0x520200, "str x0, [sp] stores to the stack pointer");
        CHECK(m.cpu.sp == sp_before, "str x0, [sp] leaves sp alone");
        e2_memory_destroy(&m.mem);
    }

    {
        // Paired 64-bit floating-point store and load with a large offset, and
        // the stack adjustment that makes room for it. If either the offset or
        // the sp decrement is off by a slot, two frames overlap and a caller's
        // local gets overwritten by its own callee.
        e2_machine m;
        e2_machine_init(&m, 0, 1 << 20);
        const uint64_t code = 0x30000000ULL;
        const uint64_t data = 0x31000000ULL;
        e2_memory_map_at(&m.mem, code, 0x1000, E2_PROT_READ | E2_PROT_WRITE | E2_PROT_EXEC);
        e2_memory_map_at(&m.mem, data, 0x1000, E2_PROT_READ | E2_PROT_WRITE);

        // stp d8, d9, [x1, #208] then ldp back.
        uint32_t stp = 0x6D0D2428u;
        e2_memory_write(&m.mem, code, &stp, 4);
        m.cpu.pc = code;
        m.cpu.x[1] = data;
        m.cpu.v[8][0] = 0x1111111111111111ULL;
        m.cpu.v[9][0] = 0x2222222222222222ULL;
        e2_step(&m);
        uint64_t a = 0, b = 0;
        e2_memory_read(&m.mem, data + 208, &a, 8);
        e2_memory_read(&m.mem, data + 216, &b, 8);
        CHECK(a == 0x1111111111111111ULL, "stp d8, d9 stores d8 at the offset");
        CHECK(b == 0x2222222222222222ULL, "stp d8, d9 stores d9 sixteen bytes on");

        uint32_t ldp = 0x6D4D2428u;
        e2_memory_write(&m.mem, code, &ldp, 4);
        m.cpu.pc = code;
        m.cpu.x[1] = data;
        m.cpu.v[8][0] = 0;
        m.cpu.v[9][0] = 0;
        e2_step(&m);
        CHECK(m.cpu.v[8][0] == 0x1111111111111111ULL &&
              m.cpu.v[9][0] == 0x2222222222222222ULL, "ldp d8, d9 reads both back");

        // sub sp, sp, #224 must move exactly 224 bytes and leave the flags be.
        uint32_t sub_sp = 0xD10383FFu;
        e2_memory_write(&m.mem, code, &sub_sp, 4);
        m.cpu.pc = code;
        const uint64_t before = m.cpu.sp;
        e2_step(&m);
        CHECK(m.cpu.sp == before - 224, "sub sp, sp, #224 subtracts exactly 224");
        e2_memory_destroy(&m.mem);
    }

    {
        // stp d8, d9, [sp, #208] and the same store through a general register.
        // The two encodings differ only in the base register field, so if one
        // lands 16 bytes off the other, the sp form is being decoded on a
        // separate path that gets the offset wrong.
        e2_machine m;
        e2_machine_init(&m, 0, 1 << 20);
        const uint64_t code = 0x30000000ULL;
        e2_memory_map_at(&m.mem, code, 0x1000, E2_PROT_READ | E2_PROT_WRITE | E2_PROT_EXEC);

        uint32_t insn = 0x6D0D27E8u;   // stp d8, d9, [sp, #208]
        e2_memory_write(&m.mem, code, &insn, 4);
        m.cpu.pc = code;
        m.cpu.v[8][0] = 0x1111111111111111ULL;
        m.cpu.v[9][0] = 0x2222222222222222ULL;
        const uint64_t sp = m.cpu.sp;
        uint64_t a = 0xBAD, b = 0xBAD, guard = 0xCAFE;
        e2_memory_write(&m.mem, sp + 208, &a, 8);
        e2_memory_write(&m.mem, sp + 216, &b, 8);
        e2_memory_write(&m.mem, sp + 224, &guard, 8);
        e2_step(&m);
        e2_memory_read(&m.mem, sp + 208, &a, 8);
        e2_memory_read(&m.mem, sp + 216, &b, 8);
        CHECK(a == 0x1111111111111111ULL && b == 0x2222222222222222ULL,
              "stp d8, d9, [sp, #208] stores at +208 and +216");
        // A 64-bit pair spans 16 bytes, not 32. Writing the extra halves lands on
        // whatever follows the pair - for this instruction in awk that was the
        // caller's stack slot, which is why it went undetected for so long: the
        // two values that matter were correct, only the overflow was wrong.
        uint64_t beyond = 0;
        e2_memory_read(&m.mem, sp + 224, &beyond, 8);
        CHECK(beyond == 0xCAFE, "stp d8, d9 does not write past the pair");
        e2_memory_destroy(&m.mem);
    }

    {
        // fcmpe must set NZCV so that b.ge (N == V) means "not less than".
        // Getting the operands the wrong way round here makes every float
        // comparison branch the opposite way, which is silent: the program takes
        // a valid-looking path and simply never does the work.
        e2_machine m;
        e2_machine_init(&m, 0, 1 << 20);
        const uint64_t at = 0x30000000ULL;
        e2_memory_map_at(&m.mem, at, 0x1000, E2_PROT_READ | E2_PROT_WRITE | E2_PROT_EXEC);
        uint32_t insn = 0x1E602110u;   // fcmpe d8, d0
        e2_memory_write(&m.mem, at, &insn, 4);

        const double one = 1.0, two = 2.0;
        memcpy(&m.cpu.v[8][0], &one, 8);
        memcpy(&m.cpu.v[0][0], &two, 8);
        m.cpu.pc = at;
        e2_step(&m);
        CHECK(m.cpu.flags.n == true && m.cpu.flags.z == false &&
              m.cpu.flags.c == false && m.cpu.flags.v == false,
              "fcmpe 1, 2 reports less-than");

        memcpy(&m.cpu.v[8][0], &two, 8);
        memcpy(&m.cpu.v[0][0], &one, 8);
        m.cpu.pc = at;
        e2_step(&m);
        CHECK(m.cpu.flags.n == false && m.cpu.flags.z == false &&
              m.cpu.flags.c == true && m.cpu.flags.v == false,
              "fcmpe 2, 1 reports greater-than");

        memcpy(&m.cpu.v[8][0], &one, 8);
        memcpy(&m.cpu.v[0][0], &one, 8);
        m.cpu.pc = at;
        e2_step(&m);
        CHECK(m.cpu.flags.z == true && m.cpu.flags.c == true && m.cpu.flags.v == false,
              "fcmpe of equal values reports equal");
        e2_memory_destroy(&m.mem);
    }

    {
        // fcvtzs with a 64-bit destination. The 32-bit form is exercised by
        // printf("%d") and worked, while awk's int() - which casts to long long
        // - returned the raw IEEE bits of the double instead of the value. That
        // is what a conversion that never happened looks like from outside.
        e2_machine m;
        e2_machine_init(&m, 0, 1 << 20);
        const uint64_t at = 0x30000000ULL;
        e2_memory_map_at(&m.mem, at, 0x1000, E2_PROT_READ | E2_PROT_WRITE | E2_PROT_EXEC);

        const double three_nine = 3.9, neg = -3.9;
        uint64_t insn64 = 0x9E780000u;   // fcvtzs x0, d0
        e2_memory_write(&m.mem, at, &insn64, 4);
        memcpy(&m.cpu.v[0][0], &three_nine, 8);
        m.cpu.pc = at;
        e2_step(&m);
        CHECK(m.cpu.x[0] == 3, "fcvtzs x0, d0 truncates toward zero");

        memcpy(&m.cpu.v[0][0], &neg, 8);
        m.cpu.pc = at;
        e2_step(&m);
        CHECK(m.cpu.x[0] == (uint64_t)(int64_t)-3, "fcvtzs x0, d0 truncates negatives");

        uint32_t insn32 = 0x1E780000u;   // fcvtzs w0, d0
        e2_memory_write(&m.mem, at, &insn32, 4);
        memcpy(&m.cpu.v[0][0], &three_nine, 8);
        m.cpu.pc = at;
        e2_step(&m);
        CHECK(m.cpu.x[0] == 3, "fcvtzs w0, d0 truncates into 32 bits");
        e2_memory_destroy(&m.mem);
    }

    {
        // int to double: scvtf d0, x0 and ucvtf d0, x0. pow and fmod return
        // nonsense while sqrt and exp are fine, and INT64_MIN showing up as a
        // result is the signature of a saturated float-to-int conversion - so
        // the conversions in this direction are worth pinning down directly.
        e2_machine m;
        e2_machine_init(&m, 0, 1 << 20);
        const uint64_t at = 0x30000000ULL;
        e2_memory_map_at(&m.mem, at, 0x1000, E2_PROT_READ | E2_PROT_WRITE | E2_PROT_EXEC);

        uint32_t scvtf = 0x9E620000u;   // scvtf d0, x0
        e2_memory_write(&m.mem, at, &scvtf, 4);
        m.cpu.x[0] = 5;
        m.cpu.pc = at;
        e2_step(&m);
        double d = 0;
        memcpy(&d, &m.cpu.v[0][0], 8);
        CHECK(d == 5.0, "scvtf d0, x0 converts a positive integer");

        m.cpu.x[0] = (uint64_t)(int64_t)-7;
        m.cpu.pc = at;
        e2_step(&m);
        memcpy(&d, &m.cpu.v[0][0], 8);
        CHECK(d == -7.0, "scvtf d0, x0 converts a negative integer");

        uint32_t ucvtf = 0x9E630000u;   // ucvtf d0, x0
        e2_memory_write(&m.mem, at, &ucvtf, 4);
        m.cpu.x[0] = 18446744073709551615ULL;
        m.cpu.pc = at;
        e2_step(&m);
        memcpy(&d, &m.cpu.v[0][0], 8);
        CHECK(d > 1.8e19, "ucvtf d0, x0 handles the top of the unsigned range");
        e2_memory_destroy(&m.mem);
    }

    {
        // frintz on a double. The FRINT table only carried the single-precision
        // encodings (0x1E24_xxxx); the double forms are 0x1E64_xxxx, one bit
        // higher in the type field.
        e2_machine m;
        e2_machine_init(&m, 0, 1 << 20);
        const uint64_t at = 0x30000000ULL;
        e2_memory_map_at(&m.mem, at, 0x1000, E2_PROT_READ | E2_PROT_WRITE | E2_PROT_EXEC);
        uint32_t insn = 0x1E65C000u;   // frintz d0, d0
        e2_memory_write(&m.mem, at, &insn, 4);
        const double v = 3.9;
        memcpy(&m.cpu.v[0][0], &v, 8);
        m.cpu.pc = at;
        e2_step(&m);
        double out = 0;
        memcpy(&out, &m.cpu.v[0][0], 8);
        CHECK(out == 3.0, "frintz d0 truncates toward zero");
        e2_memory_destroy(&m.mem);
    }

    {
        // The same conversions with a register other than 0. awk's integer check
        // uses x3, so an off-by-one in the register field would only show up
        // there and not in a test written against x0.
        e2_machine m;
        e2_machine_init(&m, 0, 1 << 20);
        const uint64_t at = 0x30000000ULL;
        e2_memory_map_at(&m.mem, at, 0x1000, E2_PROT_READ | E2_PROT_WRITE | E2_PROT_EXEC);

        uint32_t insn = 0x9E620060u;   // scvtf d0, x3
        e2_memory_write(&m.mem, at, &insn, 4);
        m.cpu.x[3] = 7;
        m.cpu.pc = at;
        e2_step(&m);
        double d = 0;
        memcpy(&d, &m.cpu.v[0][0], 8);
        CHECK(d == 7.0, "scvtf d0, x3 converts from x3");

        uint32_t back = 0x9E780003u;   // fcvtzs x3, d0
        e2_memory_write(&m.mem, at, &back, 4);
        m.cpu.x[3] = 0;
        m.cpu.pc = at;
        e2_step(&m);
        CHECK(m.cpu.x[3] == 7, "fcvtzs x3, d0 writes to x3");
        e2_memory_destroy(&m.mem);
    }

    {
        // fcvtzs in its SIMD scalar form: the destination is an FP register, not
        // a general one. awk's int() uses exactly this pair - fcvtzs d8, d9 then
        // scvtf d8, d8 - and getting it wrong leaves the raw IEEE bits in place,
        // which is why int(7) printed 7.0's bit pattern.
        e2_machine m;
        e2_machine_init(&m, 0, 1 << 20);
        const uint64_t at = 0x30000000ULL;
        e2_memory_map_at(&m.mem, at, 0x1000, E2_PROT_READ | E2_PROT_WRITE | E2_PROT_EXEC);

        uint32_t insn = 0x5EE1B928u;   // fcvtzs d8, d9
        e2_memory_write(&m.mem, at, &insn, 4);
        const double seven = 7.0, three_nine = 3.9;
        memcpy(&m.cpu.v[9][0], &seven, 8);
        m.cpu.pc = at;
        e2_step(&m);
        CHECK(m.cpu.v[8][0] == 7, "fcvtzs d8, d9 converts to an integer in an FP register");

        memcpy(&m.cpu.v[9][0], &three_nine, 8);
        m.cpu.pc = at;
        e2_step(&m);
        CHECK(m.cpu.v[8][0] == 3, "fcvtzs d8, d9 truncates toward zero");

        uint32_t back = 0x5E61D908u;   // scvtf d8, d8
        e2_memory_write(&m.mem, at, &back, 4);
        m.cpu.v[8][0] = 7;
        m.cpu.pc = at;
        e2_step(&m);
        double d = 0;
        memcpy(&d, &m.cpu.v[8][0], 8);
        CHECK(d == 7.0, "scvtf d8, d8 converts back to a double");
        e2_memory_destroy(&m.mem);
    }

    {
        // fmov between a general register and an FP register moves the BITS, not
        // the value: fmov d1, x0 with x0 holding a double's pattern gives that
        // double back. musl's pow ends with exactly this pair, so getting it
        // wrong corrupts the result at the last step.
        e2_machine m;
        e2_machine_init(&m, 0, 1 << 20);
        const uint64_t at = 0x30000000ULL;
        e2_memory_map_at(&m.mem, at, 0x1000, E2_PROT_READ | E2_PROT_WRITE | E2_PROT_EXEC);

        const double two = 2.0;
        uint64_t bits = 0;
        memcpy(&bits, &two, 8);
        uint32_t insn = 0x9E670001u;   // fmov d1, x0
        e2_memory_write(&m.mem, at, &insn, 4);
        m.cpu.x[0] = bits;
        m.cpu.pc = at;
        e2_step(&m);
        CHECK(m.cpu.v[1][0] == bits, "fmov d1, x0 copies the bits verbatim");

        uint32_t back = 0x9E660001u;   // fmov x1, d0
        e2_memory_write(&m.mem, at, &back, 4);
        memcpy(&m.cpu.v[0][0], &two, 8);
        m.cpu.x[1] = 0;
        m.cpu.pc = at;
        e2_step(&m);
        CHECK(m.cpu.x[1] == bits, "fmov x1, d0 copies the bits back");
        e2_memory_destroy(&m.mem);
    }

    // UMOV moves a vector element into a general register. musl's memset does
    // `mov x1, v0.d[0]` to materialise the replicated byte pattern, so a broken
    // UMOV silently turned every memset into a zero fill. It used to decode as
    // DUP because that rule only tested bits 11-10 of the opcode.
    {
        static const struct { uint32_t insn; uint64_t v_lo; uint64_t want; const char *what; } um[] = {
            {0x4E083C01u, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, "umov x1, v0.d[0] all ones"},
            {0x4E083C01u, 0x1122334455667788ULL, 0x1122334455667788ULL, "umov x1, v0.d[0] pattern"},
            {0x0E043C01u, 0x1122334455667788ULL, 0x0000000055667788ULL, "umov w1, v0.s[0]"},
            {0x0E013C01u, 0x1122334455667788ULL, 0x0000000000000088ULL, "umov w1, v0.b[0]"},
        };
        for (unsigned i = 0; i < sizeof(um) / sizeof(um[0]); i++) {
            e2_machine m;
            e2_machine_init(&m, 0, 1 << 20);
            const uint64_t at = 0x30000000ULL;
            e2_memory_map_at(&m.mem, at, 0x1000, E2_PROT_READ | E2_PROT_WRITE | E2_PROT_EXEC);
            e2_memory_write(&m.mem, at, &um[i].insn, 4);
            m.cpu.pc = at;
            m.cpu.v[0][0] = um[i].v_lo;
            const e2_status st = e2_step(&m);
            CHECK(st == E2_OK && m.cpu.x[1] == um[i].want, "%s", um[i].what);
            e2_memory_destroy(&m.mem);
        }
    }

    // Sign-extending loads must extend, not just move bytes. The register-offset
    // path skipped that once, so a 0xFF byte became 0xFF instead of -1 and every
    // index derived from it was wrong.
    {
        static const struct { uint32_t insn; unsigned char byte; uint64_t want; const char *what; } se[] = {
            {0x38B56B34u, 0xFF, 0xFFFFFFFFFFFFFFFFULL, "ldrsb x20, [x25, x21] with -1"},
            {0x38B56B34u, 0x7F, 0x000000000000007FULL, "ldrsb x20, [x25, x21] with 127"},
            {0x38B56B34u, 0x80, 0xFFFFFFFFFFFFFF80ULL, "ldrsb x20, [x25, x21] with -128"},
        };
        for (unsigned i = 0; i < sizeof(se) / sizeof(se[0]); i++) {
            e2_machine m;
            e2_machine_init(&m, 0, 1 << 20);
            const uint64_t at = 0x30000000ULL;
            e2_memory_map_at(&m.mem, at, 0x1000, E2_PROT_READ | E2_PROT_WRITE | E2_PROT_EXEC);
            const uint64_t tbl = 0x104D0000ULL;
            e2_memory_map_at(&m.mem, tbl, 0x1000, E2_PROT_READ | E2_PROT_WRITE);
            e2_memory_write(&m.mem, at, &se[i].insn, 4);
            e2_memory_write(&m.mem, tbl, &se[i].byte, 1);
            m.cpu.pc = at;
            m.cpu.x[25] = tbl;
            m.cpu.x[21] = 0;
            const e2_status st = e2_step(&m);
            CHECK(st == E2_OK && m.cpu.x[20] == se[i].want, "%s", se[i].what);
            e2_memory_destroy(&m.mem);
        }
    }

    // scvtf/ucvtf convert an integer in x0 to a float in s0.
    {
        e2_machine m;
        e2_machine_init(&m, 0, 1 << 20);
        const uint64_t at = 0x30000000ULL;
        e2_memory_map_at(&m.mem, at, 0x1000, E2_PROT_READ | E2_PROT_WRITE | E2_PROT_EXEC);
        const uint32_t insn = 0x1E220001u;   // scvtf s1, w0
        e2_memory_write(&m.mem, at, &insn, 4);
        m.cpu.pc = at;
        m.cpu.x[0] = 7;
        const e2_status st = e2_step(&m);
        CHECK(st == E2_OK && (m.cpu.v[1][0] & 0xFFFFFFFFULL) == 0x40E00000ULL,
              "scvtf converts 7 to 7.0f");
        e2_memory_destroy(&m.mem);
    }
}
