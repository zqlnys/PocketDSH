// Exercises the machine code shapes a compiler emits for a non-trivial integer
// function: a stack frame with STP/LDP, BL/RET, a loop condition, recursion, and
// spill slots.
//
//    int fib(int n) { return n < 2 ? n : fib(n-1) + fib(n-2); }
//    write(1, "ok\n", 3); exit(fib(10) == 55 ? 0 : 1);
//
// Scratch values live in stack slots rather than callee-saved registers so the
// test depends only on instructions the interpreter implements.

    .global _start
    .text

// x0 = n, returns fib(n) in x0
fib:
    stp x29, x30, [sp, #-32]!   // frame: fp, lr, two spill slots
    mov x29, sp
    str x0, [sp, #16]           // save n
    cmp x0, #2
    b.lt fib_base

    sub x0, x0, #1
    bl fib
    str x0, [sp, #24]           // save fib(n-1)

    ldr x0, [sp, #16]           // reload n
    sub x0, x0, #2
    bl fib

    ldr x1, [sp, #24]
    add x0, x0, x1

fib_base:
    ldp x29, x30, [sp], #32
    ret

_start:
    mov x0, #10
    bl fib
    cmp x0, #55
    b.ne fail

    mov x0, #1
    adr x1, msg
    mov x2, #3
    mov x8, #64                 // SYS_write
    svc #0

    mov x0, #0
    b exit
fail:
    mov x0, #1
exit:
    mov x8, #93                 // SYS_exit
    svc #0

msg:
    .ascii "ok\n"
