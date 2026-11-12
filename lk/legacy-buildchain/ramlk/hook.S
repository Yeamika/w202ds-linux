/* Replaces only the 236-byte getdump handler in this pinned LK build. */
.section .text.hook,"ax"
.global hook
hook:
    stp x29, x30, [sp, #-48]!
    mov x29, sp
    stp x19, x20, [sp, #16]
    stp x21, x22, [sp, #32]
    mov x19, x0
    mov x20, #0x86000000
    cmp x1, x20
    b.ne reject
    cmp x2, #32
    b.lo reject
    tst x2, #0xffffffffff000000
    b.ne reject
    ldr w3, [x20, #4]
    cmp x2, x3
    b.ne reject
    ldp w3, w4, [x20, #8]
    mov w5, #0x4c50
    movk w5, #0x314b, lsl #16
    cmp w3, w5
    b.ne reject
    cmp w4, #1
    b.ne reject
    mov x0, x20
    mov x1, x2
    bl lk_icache_sync
    /* Pinned build's EL2 L2 table is initialized at 0xb5f01000. The first
     * module block is temporarily RO+X; restore RW+XN before the next download.
     * Mutable module data is outside this block at 0x86400000. */
    adrp x21, ptable_page
    add x21, x21, #0x180
    ldr x22, [x21]
    tbnz x22, #1, reject
    and x5, x22, #0x0000ffffffe00000
    cmp x5, x20
    b.ne reject
    bic x7, x22, #0x0060000000000000
    orr x7, x7, #0x80
    bl permissions
    mov x0, x19
    blr x20
    mov w19, w0
    mov x7, x22
    bl permissions
    cbnz w19, reject
    adrp x0, okay_page
    add x0, x0, #0x7e0
    b reply
reject:
    adrp x0, okay_page
    add x0, x0, #0x748
reply:
    mov x1, xzr
    bl lk_status
    ldp x21, x22, [sp, #32]
    ldp x19, x20, [sp, #16]
    ldp x29, x30, [sp], #48
    ret
permissions:
    str x7, [x21]
    dc cvac, x21
    dsb ishst
    tlbi alle2is
    dsb ish
    isb
    ret
    .space 236-(.-hook), 0
