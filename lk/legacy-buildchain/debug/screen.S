/* SPDX-License-Identifier: MIT
 * Reuse the pinned native LCD console, font, framebuffer and cache sync.
 * Called only from LK context; no heap, display reinitialization or storage.
 */
.section .text,"ax"
.global lk_debug_screen
lk_debug_screen:
    stp x29, x30, [sp, #-32]!
    mov x29, sp
    str x19, [sp, #16]
    mov x19, x0
    ldr x9, =lk_lcd_enabled
    ldrb w9, [x9]
    cbz w9, screen_done
    mov w0, #0
    mov w1, #4
    bl lk_lcd_position
    adr x0, screen_title
    bl lk_lcd_puts
    mov w0, #0
    mov w1, #6
    bl lk_lcd_position
    adr x0, screen_blank
    bl lk_lcd_puts
    mov w0, #0
    mov w1, #6
    bl lk_lcd_position
    mov x0, x19
    bl lk_lcd_puts
screen_done:
    ldr x19, [sp, #16]
    ldp x29, x30, [sp], #32
    ret
screen_title: .asciz "PJ4EDK2 LK DEBUG"
screen_blank: .fill 72, 1, 32
    .byte 0
.balign 4
.ltorg
.section .note.GNU-stack,"",%progbits
