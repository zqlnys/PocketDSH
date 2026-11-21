    .global _start
    .text
_start:
    movz x0, #3, lsl #16
    movk x0, #0xd090
1:
    add x1, x1, #1
    eor x2, x1, x0
    sub x0, x0, #1
    cbnz x0, 1b
    mov x0, #0
    mov x8, #93
    svc #0
