/* Only NORMAL/EXT_RSTN reach this bootstrap. A failed resident returns to
 * native fastboot so it cannot accidentally start an unverified B kernel. */
.section .text.start,"ax"
.global resident_start, resident_keep_splash
resident_start:
    stp x29, x30, [sp, #-48]!
    mov x29, sp
    stp x19, x20, [sp, #16]
    stp x21, x22, [sp, #32]
    mov x20, x0
    mov x0, xzr
    bl resident_loader
    cbnz w0, load_failed
    adrp x21, ptable_page
    add x21, x21, #0x180
    ldr x22, [x21]
    bic x7, x22, #0x0060000000000000
    orr x7, x7, #0x80
    bl permissions
    adr x0, operation
    mov w1, w19
    mov x2, x20
    mov x3, #0x86000000
    blr x3
    mov x7, x22
    bl permissions
    b resume_fastboot
load_failed:
    mov w1, w0
    adr x0, load_error
    bl lk_printf
resume_fastboot:
    ldp x21, x22, [sp, #32]
    ldp x19, x20, [sp, #16]
    ldp x29, x30, [sp], #48
    b lk_fastboot_init
operation: .asciz ":dispatch"
load_error: .asciz "UEFI load:%d\n"
.balign 4
.org 0x90
/* The native LCD preload thread should retain the menu during handoff.
 * Before a resident has run, preserve the factory charge/recovery display. */
resident_keep_splash:
    adrp x16, bridge_page
    ldr w17, [x16, #0x394]
    cbz w17, lk_lcd_splash
    ret
.space 164-(.-resident_start), 0
