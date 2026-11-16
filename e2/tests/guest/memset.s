// The shapes musl's memset uses on aarch64: broadcast a byte into a vector, then
// write it out in 32-byte chunks. If the interpreter cannot run this, no real
// libc can start.
//
//   buf[64]; memset-like fill with 'A'; verify; write "ok\n"; exit

    .global _start
    .text

_start:
    // Fill 64 bytes at buf with 'A' (0x41) using vector stores.
    mov w1, #0x41
    dup v0.16b, w1
    adr x0, buf
    stp q0, q0, [x0]
    stp q0, q0, [x0, #32]

    // Verify all 64 bytes.
    mov w2, #64
    mov w3, #0
verify:
    ldrb w4, [x0, x3]
    cmp w4, #0x41
    b.ne fail
    add x3, x3, #1
    cmp x3, #64
    b.ne verify

    mov x0, #1
    adr x1, msg
    mov x2, #3
    mov x8, #64
    svc #0
    mov x0, #0
    b exit
fail:
    mov x0, #1
exit:
    mov x8, #93
    svc #0

msg:
    .ascii "ok\n"
    .bss
    .align 4
buf:
    .space 64
