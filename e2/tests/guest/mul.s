// Multiplication coverage: 32-bit wraparound, 64-bit, multiply-accumulate,
// widen-multiply, and the high-half variants. Each result is compared and the
// first mismatch is reported as the exit code.

    .global _start
    .text

_start:
    // 32-bit multiply that overflows 32 bits: 100000 * 100000 = 10^10
    // low 32 bits = 0x540BE400, so the wrapped result is 1410065408.
    movz w0, #0x86A0
    movk w0, #0x1, lsl #16
    mov w1, w0
    mul w2, w0, w1
    mov w3, #0xE400
    movk w3, #0x540B, lsl #16
    cmp w2, w3
    b.ne f1

    // 64-bit multiply keeps the full product.
    movz x0, #0x86A0
    movk x0, #0x1, lsl #16
    mov x1, x0
    mul x2, x0, x1
    mov x3, #0xE400
    movk x3, #0x540B, lsl #16
    movk x3, #0x2, lsl #32
    cmp x2, x3
    b.ne f2

    // multiply-accumulate: 7 * 6 + 5 = 47
    mov w0, #7
    mov w1, #6
    mov w2, #5
    madd w3, w0, w1, w2
    cmp w3, #47
    b.ne f3

    // multiply-subtract: 5 - 7 * 6 = -37
    mov w0, #7
    mov w1, #6
    mov w2, #5
    msub w3, w0, w1, w2
    mov w4, #37
    neg w4, w4
    cmp w3, w4
    b.ne f4

    // widen multiply: (2^31) * 2 = 2^32 (would be 0 in 32 bits)
    mov w0, #0
    movk w0, #0x8000, lsl #16
    mov w1, #2
    umull x2, w0, w1
    mov x3, #1
    lsl x3, x3, #32
    cmp x2, x3
    b.ne f5

    mov x0, #0
    b exit

f1: mov x0, #1
    b exit
f2: mov x0, #2
    b exit
f3: mov x0, #3
    b exit
f4: mov x0, #4
    b exit
f5: lsr x0, x2, #32       // report the high half of the widen result
    add x0, x0, #100      // offset so 0 is distinguishable from success
    b exit
exit:
    mov x8, #93
    svc #0
