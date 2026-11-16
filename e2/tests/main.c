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
static int test_file_stat(void *ctx, int fd, void *buf, size_t buflen) {
    (void)ctx; (void)fd; (void)buf; (void)buflen;
    return -9;  // EBADF
}
static long long test_clock_ns(void *ctx) { (void)ctx; return 0; }

static const e2_file_ops test_files = {
    .open = test_file_open,
    .read = test_file_read,
    .write = test_file_write,
    .close = test_file_close,
    .stat = test_file_stat,
    .clock_ns = test_clock_ns,
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

int main(void) {
    printf("== e2 interpreter core tests ==\n");
    test_movz_and_ret();
    test_add_sub();
    test_movn_movk();
    test_loop_with_branch();
    test_flags_and_condition();
    test_load_store();
    test_svc();
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

    printf("\n%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
