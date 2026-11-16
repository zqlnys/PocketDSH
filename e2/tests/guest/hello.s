// Minimal static aarch64 Linux program: write(1, "hello\n", 6); exit(0).
// Assembled by the test harness with clang --target=aarch64-linux-gnu and
// linked by ld.lld, so the loader is exercised against real compiler output
// rather than hand-packed arrays.

    .global _start
    .text
_start:
    mov x0, #1              // fd = stdout
    adr x1, msg             // buffer
    mov x2, #6              // length
    mov x8, #64             // SYS_write
    svc #0

    mov x0, #0              // status
    mov x8, #93             // SYS_exit
    svc #0

msg:
    .ascii "hello\n"
