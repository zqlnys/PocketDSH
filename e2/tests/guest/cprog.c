// Freestanding C program: compiled by clang for aarch64 with no libc, so the
// instruction mix is whatever the compiler really emits for this code rather
// than what I guessed it would emit. Any missing instruction shows up as a
// concrete failure with a backtrace point.

typedef unsigned long usize;
typedef long ssize;

static long syscall3(long n, long a, long b, long c) {
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
    return x0;
}

static void write_all(const char *buf, usize len) {
    syscall3(64, 1, (long)buf, (long)len);
}

static void exit_now(int code) {
    syscall3(93, code, 0, 0);
    for (;;) { }
}

// Small string helpers so the compiler emits real loops rather than calling into
// a libc that does not exist here.
static usize str_len(const char *s) {
    usize n = 0;
    while (s[n]) n++;
    return n;
}

static void str_copy(char *dst, const char *src) {
    while (*src) *dst++ = *src++;
    *dst = 0;
}

static int checksum(const unsigned char *p, usize n) {
    int sum = 0;
    for (usize i = 0; i < n; i++) {
        sum = (sum * 31 + p[i]) & 0xFFFF;
    }
    return sum;
}

struct point { int x; int y; };

static struct point make_point(int x, int y) {
    struct point p;
    p.x = x;
    p.y = y;
    return p;
}

static int point_sum(struct point p) {
    return p.x + p.y;
}

static char out[128];
static usize out_len;

static void emit(const char *s) {
    usize n = str_len(s);
    for (usize i = 0; i < n && out_len < sizeof(out); i++) out[out_len++] = s[i];
}

static void emit_num(int value) {
    char tmp[16];
    int n = 0;
    unsigned v = value < 0 ? (unsigned)(-value) : (unsigned)value;
    if (value < 0) emit("-");
    do {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v);
    while (n > 0) {
        if (out_len < sizeof(out)) out[out_len++] = tmp[--n];
    }
}

void _start(void) {
    unsigned char data[32];
    for (int i = 0; i < 32; i++) {
        data[i] = (unsigned char)(i * 7 + 3);
    }

    char msg[16];
    str_copy(msg, "checksum=");
    emit(msg);
    emit_num(checksum(data, sizeof(data)));
    emit("\n");

    struct point p = make_point(20, 22);
    emit("sum=");
    emit_num(point_sum(p));
    emit("\n");

    // Report through an explicitly-typed int so a byte-vs-int difference in the
    // call path is visible rather than conflated with the renderer.
    emit("d0=");
    { int v0 = (int)data[0]; emit_num(v0); }
    emit("\n");
    emit("d1=");
    { int v1 = (int)data[1]; emit_num(v1); }
    emit("\n");
    emit("idx=");
    { int t = 0; for (int i = 0; i < 32; i++) t += data[i]; emit_num(t); }
    emit("\n");

    write_all(out, out_len);
    exit_now(0);
}
