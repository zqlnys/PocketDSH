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

static long capture_write(void *ctx, int fd, const void *buf, size_t len) {
    capture *c = (capture *)ctx;
    if (fd != 1 && fd != 2) {
        return -9;  // EBADF
    }
    if (c->out_len + len < sizeof(c->out)) {
        memcpy(c->out + c->out_len, buf, len);
        c->out_len += len;
        c->out[c->out_len] = '\0';
    }
    return (long)len;
}

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
    e2_syscall_env env = {.write = capture_write, .exit = capture_exit, .ctx = &cap};
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
    e2_syscall_env env = {.write = capture_write, .exit = capture_exit, .ctx = &cap};

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
    e2_syscall_env env = {.write = capture_write, .exit = capture_exit, .ctx = &cap};
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
        e2_syscall_env env = {.write = capture_write, .exit = capture_exit, .ctx = &cap};
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
        e2_syscall_env env = {.write = capture_write, .exit = capture_exit, .ctx = &cap};
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
        e2_syscall_env env = {.write = capture_write, .exit = capture_exit, .ctx = &cap};
        e2_status st = run_program(&m, &env, 10000);
        CHECK(st == E2_HALTED, "expected halt, got %s (%s)", e2_status_name(st),
              m.ctx.reason ? m.ctx.reason : "-");
        CHECK(cap.status == 0, "vector fill verification failed, exit %d", cap.status);
        CHECK(strcmp(cap.out, "ok\n") == 0, "output \"%s\"", cap.out);
    }

    e2_machine_destroy(&m);
    free(image);
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
    test_reject_dynamic();
    test_reject_wrong_machine();

    printf("\n%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
