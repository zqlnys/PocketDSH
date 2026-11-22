// Division and the variable shifts, including the cases that only bite at the edges.
//
// INT64_MIN / -1 traps on real hardware; an implementation that lets it through divides by a
// negative one and returns something, and nothing notices until a value happens to be that one.
// The same is true of dividing by zero, which must give zero rather than trapping.
    .global _start
    .text
_start:
    adr x19, data

    // sdiv: ordinary signed division, rounding toward zero
    ldr x1, [x19]
    ldr x2, [x19, #8]
    sdiv x3, x1, x2
    mov x2, x3
    bl print

    // sdiv: -7 / 2 must be -3, not -4
    ldr x1, [x19, #16]
    ldr x2, [x19, #8]
    sdiv x3, x1, x2
    mov x2, x3
    bl print

    // sdiv: INT64_MIN / -1 - the overflow case
    mov x1, #1
    lsl x1, x1, #63
    mov x2, #-1
    sdiv x3, x1, x2
    mov x2, x3
    bl print

    // sdiv by zero must be zero, not a trap
    ldr x1, [x19]
    mov x2, #0
    sdiv x3, x1, x2
    mov x2, x3
    bl print

    // udiv: unsigned, so the same bits divide differently
    ldr x1, [x19, #16]
    ldr x2, [x19, #8]
    udiv x3, x1, x2
    mov x2, x3
    bl print

    // lslv / lsrv / asrv / rorv by a register amount
    ldr x1, [x19, #24]
    mov x2, #3
    lslv x3, x1, x2
    mov x2, x3
    bl print

    ldr x1, [x19, #24]
    mov x2, #3
    lsrv x3, x1, x2
    mov x2, x3
    bl print

    ldr x1, [x19, #16]
    mov x2, #3
    asrv x3, x1, x2
    mov x2, x3
    bl print

    ldr x1, [x19, #24]
    mov x2, #8
    rorv x3, x1, x2
    mov x2, x3
    bl print

    mov x0, #0
    mov x8, #93
    svc #0

print:
    adr x9, buf
    mov x16, #16
2:  lsr x12, x2, #60
    and x12, x12, #0xF
    cmp x12, #10
    b.lo 3f
    add w12, w12, #87
    b 4f
3:  add w12, w12, #48
4:  strb w12, [x9], #1
    lsl x2, x2, #4
    subs x16, x16, #1
    b.ne 2b
    mov w12, #10
    strb w12, [x9], #1
    mov x0, #1
    adr x1, buf
    sub x2, x9, x1
    mov x8, #64
    svc #0
    ret

    .data
    .align 4
data:
    .quad 100          // 0
    .quad 2            // 8
    .quad -7           // 16
    .quad 0xF0E1D2C3B4A59687  // 24
    .bss
buf:
    .space 32
