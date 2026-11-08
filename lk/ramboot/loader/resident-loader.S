/* Read-only loader: return before the module replaces this entire handler
 * with its idle bridge. Do not enter the module from this address. */
.include "resident-layout.inc"
.section .text.loader,"ax"
.global resident_loader
resident_loader:
    stp x29, x30, [sp, #-64]!
    mov x29, sp
    stp x19, x20, [sp, #16]
    stp x21, x22, [sp, #32]
    stp x23, x24, [sp, #48]
    mov x20, x0
    mov x19, #0x86000000
    mov w23, #-1
    adrp x21, ptable_page
    add x21, x21, #0x180
    ldr x22, [x21]
    ldr x5, original_pte
    cmp x22, x5
    b.ne finish
    adrp x5, fastboot_page
    stp xzr, xzr, [x5, #0x58]
    adr x0, partition
    ldr w1, =RO_BYTES
    ldr w2, =RO_FLASH_OFFSET
    mov x3, x19
    bl lk_partition_read
    cbnz w0, finish
    .if DATA_BYTES
    adr x0, partition
    ldr w1, =DATA_BYTES
    ldr w2, =DATA_FLASH_OFFSET
    mov x3, #0x86400000
    bl lk_partition_read
    cbnz w0, finish
    .endif
    ldr w3, [x19, #8]
    ldr w4, =0x314b4c50
    cmp w3, w4
    b.ne finish
    ldr w3, [x19, #4]
    ldr w4, =IMAGE_BYTES
    cmp w3, w4
    b.ne finish
    ldr w3, [x19, #12]
    cmp w3, #1
    b.ne finish
    adrp x5, fastboot_page
    str x19, [x5, #0x48]
    str x4, [x5, #0x58]
    str x4, [x5, #0x60]
    mov x0, x19
    mov x1, x4
    bl lk_icache_sync
    mov w23, #0
finish:
    cbz x20, returned
    adrp x0, okay_page
    add x0, x0, #0x748
    cbnz w23, reply
    add x0, x0, #0x98
reply:
    mov x1, xzr
    bl lk_status
returned:
    mov w0, w23
    ldp x23, x24, [sp, #48]
    ldp x21, x22, [sp, #32]
    ldp x19, x20, [sp, #16]
    ldp x29, x30, [sp], #64
    ret
partition: .asciz "uboot_b"
.balign 4
original_pte: .quad 0x0060000086000711
.ltorg
.org 0x120
/* Recovery/fastboot/charging never load or execute the optional resident. */
.global resident_dispatch_filter
resident_dispatch_filter:
    cmp w19, #2
    b.eq resident_start
    cmp w19, #16
    b.eq resident_start
    br x0
.space 432-(.-resident_loader), 0
