// The permute and shift instructions, checked against qemu.
//
// UZP was implemented as an interleave and ZIP as a concatenation - exactly swapped - and that
// survived because nothing compared them against anything. A program that gathers even bytes and
// then reads them back gets plausible values either way; the difference only shows when the order
// matters, which it always does.
    .global _start
    .text
_start:
    adr x19, data
    ldr q1, [x19]
    ldr q2, [x19, #16]

    .inst 0x4e021820      // uzp1 v0.16b, v1.16b, v2.16b
    mov x2, v0.d[0]
    bl print

    ldr q1, [x19]
    ldr q2, [x19, #16]
    .inst 0x4e025820      // uzp2 v0.16b, v1.16b, v2.16b
    mov x2, v0.d[0]
    bl print

    ldr q1, [x19]
    ldr q2, [x19, #16]
    .inst 0x4e022820      // trn1 v0.16b, v1.16b, v2.16b
    mov x2, v0.d[0]
    bl print

    ldr q1, [x19]
    ldr q2, [x19, #16]
    .inst 0x4e026820      // trn2 v0.16b, v1.16b, v2.16b
    mov x2, v0.d[0]
    bl print

    ldr q1, [x19]
    ldr q2, [x19, #16]
    .inst 0x4e023820      // zip1 v0.16b, v1.16b, v2.16b
    mov x2, v0.d[0]
    bl print

    ldr q1, [x19]
    ldr q2, [x19, #16]
    .inst 0x4e027820      // zip2 v0.16b, v1.16b, v2.16b
    mov x2, v0.d[0]
    bl print

    // xtn v0.8b, v1.8h - narrow by half, keeping the low half of each lane
    ldr q1, [x19, #32]
    .inst 0x0e212820      // xtn v0.8b, v1.8h
    mov x2, v0.d[0]
    bl print

    // ld1r {v0.16b}, [x0] - load one element and replicate it across the vector
    adr x0, rep
    .inst 0x4d40c000      // ld1r {v0.16b}, [x0]
    mov x2, v0.d[0]
    bl print
    mov x2, v0.d[1]
    bl print

    // shl v0.16b, v1.16b, #3 - shift each byte left
    ldr q1, [x19, #48]
    .inst 0x4f0b5420      // shl v0.16b, v1.16b, #3
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
    .byte 0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F
    .byte 0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1A,0x1B,0x1C,0x1D,0x1E,0x1F
    .short 0x1234,0x5678,0x9ABC,0xDEF0,0x1111,0x2222,0x3333,0x4444
    .byte 0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x81,0x41,0x21,0x11,0x09,0x05,0x03,0x02
rep:
    .byte 0x5A
    .bss
buf:
    .space 32
