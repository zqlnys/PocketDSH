    .global _start
    .text
_start:
    mov x19, sp
    ldr x20, [x19], #8
1:
    ldr x0, [x19], #8
    cbnz x0, 1b
2:
    ldr x0, [x19], #8
    cbnz x0, 2b
3:
    ldr x0, [x19], #8
    cbz x0, 9f
    ldr x1, [x19], #8
    bl printpair
    b 3b
9:
    mov x0, #0
    mov x8, #93
    svc #0
printpair:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    mov x2, x0
    bl hex
    mov w12, #' '
    bl putc
    ldr x2, [x19, #-8]
    bl hex
    mov w12, #10
    bl putc
    ldp x29, x30, [sp], #16
    ret
hex:
    adr x9, buf
    mov x16, #16
4:
    lsr x12, x2, #60
    and x12, x12, #0xF
    cmp x12, #10
    b.lo 5f
    add w12, w12, #87
    b 6f
5:
    add w12, w12, #48
6:
    strb w12, [x9], #1
    lsl x2, x2, #4
    subs x16, x16, #1
    b.ne 4b
    mov x0, #1
    adr x1, buf
    mov x2, #16
    mov x8, #64
    svc #0
    ret
putc:
    strb w12, [sp, #-16]!
    mov x0, #1
    mov x1, sp
    mov x2, #1
    mov x8, #64
    svc #0
    add sp, sp, #16
    ret
    .bss
buf:
    .space 32
