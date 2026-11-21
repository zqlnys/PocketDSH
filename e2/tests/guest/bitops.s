    .global _start
    .text
_start:
    adr x19, data
    ldr q0, [x19]        // vd, distinct
    ldr q1, [x19, #16]   // vn
    ldr q2, [x19, #32]   // vm
    .inst 0x6e621c20     // bsl  v0.16b, v1.16b, v2.16b
    mov x2, v0.d[0]
    bl print
    ldr q0, [x19]
    ldr q1, [x19, #16]
    ldr q2, [x19, #32]
    .inst 0x6ea21c20     // bit  v0.16b, v1.16b, v2.16b
    mov x2, v0.d[0]
    bl print
    ldr q0, [x19]
    ldr q1, [x19, #16]
    ldr q2, [x19, #32]
    .inst 0x6ee21c20     // bif  v0.16b, v1.16b, v2.16b
    mov x2, v0.d[0]
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
    .byte 0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0
    .byte 0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC
    .byte 0xAA,0xAA,0xAA,0xAA,0xAA,0xAA,0xAA,0xAA,0xAA,0xAA,0xAA,0xAA,0xAA,0xAA,0xAA,0xAA
    .bss
buf:
    .space 32
