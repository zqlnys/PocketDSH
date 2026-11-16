// Isolates the function-call path: argument passing in x0-x7, callee-saved
// registers preserved across calls, nested calls, and return values. If the
// interpreter mishandles any of these, a program that mostly works will render
// digits from the wrong register.
//
//   out = f(1) + g(2) * 3 + h(4, 5)   expected: 10 + 6 + 9 = 25

    .global _start
    .text

// x0 -> x0 * 10 + 0
f:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    mov x1, #10
    mul x0, x0, x1
    ldp x29, x30, [sp], #16
    ret

// x0 -> x0 * 3
g:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    mov x1, #3
    mul x0, x0, x1
    ldp x29, x30, [sp], #16
    ret

// x0 + x1
h:
    add x0, x0, x1
    ret

// Uses callee-saved registers across a call: the caller's x19-x21 must survive.
use_callee_saved:
    stp x29, x30, [sp, #-32]!
    mov x29, sp
    str x19, [sp, #16]
    str x20, [sp, #24]
    mov x19, #7
    mov x20, #11
    mov x0, #0
    bl f              // clobbers x1, must not touch x19/x20
    add x0, x0, x19   // +7
    add x0, x0, x20   // +11
    ldr x20, [sp, #24]
    ldr x19, [sp, #16]
    ldp x29, x30, [sp], #32
    ret

_start:
    stp x29, x30, [sp, #-32]!
    mov x29, sp

    mov x0, #1
    bl f
    str x0, [sp, #16]     // 10

    mov x0, #2
    bl g
    str x0, [sp, #24]     // 6

    mov x0, #4
    mov x1, #5
    bl h                  // 9

    ldr x1, [sp, #16]
    add x0, x0, x1        // 19
    ldr x1, [sp, #24]
    add x0, x0, x1        // 25, expected

    cmp x0, #25
    b.ne report

    bl use_callee_saved   // 0 + 7 + 11 = 18
    cmp x0, #18
    b.eq pass
report:
    and x0, x0, #0xFF
    mov x8, #93
    svc #0
pass:

    mov x0, #1
    adr x1, msg
    mov x2, #3
    mov x8, #64
    svc #0
    mov x0, #0
    b exit
fail:
    mov x0, #1
exit:
    mov x8, #93
    svc #0

msg:
    .ascii "ok\n"
