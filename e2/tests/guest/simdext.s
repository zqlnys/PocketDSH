// SIMD instructions added late in this backend's life, checked against qemu rather than against
// this implementation's own idea of what they do.
//
// Each case loads its inputs, runs one instruction, and prints the 64-bit result. The test is
// the comparison: identical output from two independent implementations.
    .global _start
    .text
_start:
    adr x19, data

    // cnt v0.8b, v1.8b - set bits per byte
    ldr q1, [x19, #16]
    .inst 0x0e205820      // cnt v0.8b, v1.8b
    mov x2, v0.d[0]
    bl print

    // addv b0, v1.8b - sum every byte
    ldr q1, [x19, #16]
    .inst 0x0e31b820      // addv b0, v1.8b
    mov x2, v0.d[0]
    bl print

    // rev64 v0.8b, v1.8b - reverse bytes within each 8-byte half
    ldr q1, [x19]
    .inst 0x0e200820      // rev64 v0.8b, v1.8b
    mov x2, v0.d[0]
    bl print

    // ext v0.16b, v1.16b, v2.16b, #4 - take 4..19 of the concatenation
    ldr q1, [x19]
    ldr q2, [x19, #16]
    .inst 0x6e024020      // ext v0.16b, v1.16b, v2.16b, #4
    mov x2, v0.d[0]
    bl print

    // tbl v0.16b, {v1.16b}, v2.16b - gather bytes by index
    ldr q1, [x19]
    ldr q2, [x19, #32]
    .inst 0x4e020020      // tbl v0.16b, {v1.16b}, v2.16b
    mov x2, v0.d[0]
    bl print

    // dup v0.4s, v1.s[1] - broadcast one element
    ldr q1, [x19, #16]
    .inst 0x4e0c0420      // dup v0.4s, v1.s[1]
    mov x2, v0.d[0]
    bl print

    // umov w0, v1.b[3] - read one lane into a general register
    ldr q1, [x19, #16]
    .inst 0x0e1d3820      // umov w0, v1.b[3]
    mov x2, x0
    bl print

    // smov w0, v1.h[2] - sign-extended lane read
    ldr q1, [x19, #48]
    .inst 0x4e1c2820      // smov w0, v1.h[2]
    mov x2, x0
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
    .byte 0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xAA,0xBB,0xCC,0xDD,0xEE,0xFF
    .byte 0x0F,0xF0,0x33,0xCC,0x55,0xAA,0x01,0x80,0x7F,0xFE,0x11,0x22,0x33,0x44,0x55,0x66
    .byte 0x03,0x02,0x01,0x00,0x07,0x06,0x05,0x04,0x0B,0x0A,0x09,0x08,0x0F,0x0E,0x0D,0x0C
    .byte 0xF0,0xFF,0x80,0x00,0x7F,0x01,0x34,0x12,0xFF,0xFF,0x00,0x00,0x11,0x11,0x22,0x22
    .bss
buf:
    .space 32
