// Loading a pointer: 8 bytes or 4, and what the high half must be.
//
// A thread was found executing at an address with its top thirty-two bits missing. The way to get
// that from this instruction set is a four-byte load where an eight-byte one was meant, because
// LDR W zero-extends. These cases pin both forms against the same data.
    .global _start
    .text
_start:
    adr x19, data

    ldr x1, [x19]            // 8 bytes: the whole pointer
    mov x2, x1
    bl print

    ldr w1, [x19]            // 4 bytes: low half only, zero extended
    mov x2, x1
    bl print

    ldr x1, [x19, #8]        // the second word on its own
    mov x2, x1
    bl print

    ldr x1, [x19]            // from a register offset, as a struct walk does
    mov x2, x1
    bl print

    add x3, x19, #0
    ldr x1, [x3, #0]
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
    .quad 0x400000872ca4     // a pointer whose high half is not zero
    .quad 0x000000872ca4
    .bss
buf:
    .space 32
