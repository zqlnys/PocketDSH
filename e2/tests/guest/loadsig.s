// Signed loads, which differ in three ways that are easy to conflate: how far the sign travels,
// and whether the destination is W or X. A byte of 0x80 is -128 in all of them, but what lands in
// the register is 0xFFFFFFFFFFFFFF80 for an X destination and 0x00000000FFFFFF80 for a W one.
    .global _start
    .text
_start:
    adr x19, data

    ldrsb x1, [x19]          // 0x80 -> -128, sign extended to 64
    mov x2, x1
    bl print

    ldrsb w1, [x19]          // same byte, W destination: sign travels to 32 only
    mov x2, x1
    bl print

    ldrsb x1, [x19, #1]      // 0x7F -> 127
    mov x2, x1
    bl print

    ldrsh x1, [x19, #2]      // 0x8000 -> -32768
    mov x2, x1
    bl print

    ldrsh w1, [x19, #2]
    mov x2, x1
    bl print

    ldrsw x1, [x19, #4]      // 0x80000000 -> -2147483648
    mov x2, x1
    bl print

    ldrb w1, [x19]           // unsigned, so no sign at all
    mov x2, x1
    bl print

    ldrh w1, [x19, #2]
    mov x2, x1
    bl print

    ldr w1, [x19, #4]        // a plain 32-bit load zero-extends
    mov x2, x1
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
    .byte 0x80, 0x7F, 0x00, 0x80, 0x00, 0x00, 0x00, 0x80
    .bss
buf:
    .space 32
