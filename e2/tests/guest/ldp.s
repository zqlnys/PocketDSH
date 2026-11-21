    .global _start
    .text
_start:
    adr x10, area
    mov x0, #0x11
    mov x1, #0x22
    stp x0, x1, [x10]
    mov x0, #0
    mov x1, #0
    ldp x0, x1, [x10]
    mov x2, x0
    bl print
    mov x2, x1
    bl print
    mov x0, #0x33
    mov x1, #0x44
    stp x0, x1, [x10, #-16]!
    mov x2, x10
    bl print
    ldp x0, x1, [x10], #16
    mov x2, x0
    bl print
    mov x2, x1
    bl print
    mov x2, x10
    bl print
    mov x0, #0
    mov x8, #93
    svc #0
print:
    adr x9, buf
    mov x16, #16
1:
    lsr x12, x2, #60
    and x12, x12, #0xF
    cmp x12, #10
    b.lo 2f
    add w12, w12, #87
    b 3f
2:
    add w12, w12, #48
3:
    strb w12, [x9], #1
    lsl x2, x2, #4
    subs x16, x16, #1
    b.ne 1b
    mov w12, #10
    strb w12, [x9], #1
    mov x0, #1
    adr x1, buf
    sub x2, x9, x1
    mov x8, #64
    svc #0
    ret
    .bss
area:
    .space 64
buf:
    .space 32
