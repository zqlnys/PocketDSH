    .global _start
    .text
_start:
    adr x10, table
    mov x0, #2
    ldr x1, [x10, x0, lsl #3]
    br x1
    mov x2, #0xdead
    b done
wrong:
    mov x2, #0x11
    b done
targ:
    mov x2, #0x2a
    b done
done:
    adr x9, buf
    mov x16, #16
2:
    lsr x12, x2, #60
    and x12, x12, #0xF
    cmp x12, #10
    b.lo 3f
    add w12, w12, #39
    b 4f
3:
    add w12, w12, #48
4:
    strb w12, [x9], #1
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
    mov x0, #0
    mov x8, #93
    svc #0
    .data
table:
    .quad wrong, wrong, targ, wrong
    .bss
buf:
    .space 32
