typedef unsigned long size_t;
static long sys3(long n, long a, long b, long c) {
    register long x8 __asm__("x8") = n; register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b; register long x2 __asm__("x2") = c;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
    return x0;
}
static char out[256]; static size_t len;
static void putc_(char c) { if (len < sizeof(out)) out[len++] = c; }
static void putnum(long v) {
    char t[24]; int n = 0; unsigned long u;
    if (v < 0) { putc_('-'); u = (unsigned long)(-v); } else u = (unsigned long)v;
    do { t[n++] = '0' + (u % 10); u /= 10; } while (u);
    while (n) putc_(t[--n]);
}
// Source data is placed in memory and loaded with ldr q0, so the test does not
// depend on element-insert instructions that the backend may not implement.
static int src[4] = {1, -1, 0, 0};
static long dst[4];
void _start(void) {
    __asm__ volatile(
        "ldr  q0, [%0]\n\t"
        "sshll v1.2d, v0.2s, #0\n\t"
        "str  q1, [%1]\n\t"
        :: "r"(src), "r"(dst) : "v0","v1","memory");
    putnum(dst[0]); putc_(','); putnum(dst[1]); putc_('|');
    __asm__ volatile(
        "ldr  q0, [%0]\n\t"
        "sshll v1.2d, v0.2s, #4\n\t"
        "str  q1, [%1]\n\t"
        :: "r"(src), "r"(dst) : "v0","v1","memory");
    putnum(dst[0]); putc_(','); putnum(dst[1]); putc_('|');
    __asm__ volatile(
        "ldr  q0, [%0]\n\t"
        "ushll v1.2d, v0.2s, #0\n\t"
        "str  q1, [%1]\n\t"
        :: "r"(src), "r"(dst) : "v0","v1","memory");
    putnum(dst[0]); putc_(','); putnum(dst[1]);
    putc_('\n');
    sys3(64, 1, (long)out, (long)len);
    sys3(93, 0, 0, 0);
    for (;;) {}
}
