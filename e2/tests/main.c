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
