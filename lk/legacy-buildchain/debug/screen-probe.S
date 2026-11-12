/* SPDX-License-Identifier: MIT
 * RAM-only verification of the same LCD interface used by the LK extensions.
 */
.section .header,"ax"
.global probe_start
probe_start:
    b screen_probe
    .word __image_end - probe_start
    .ascii "PLK1"
    .word 1
    .quad 0, 0
.section .text,"ax"
screen_probe:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    adr x0, message
    bl lk_debug_screen
    ldp x29, x30, [sp], #16
    mov w0, #0
    ret
message: .asciz "Native LCD console OK; RAM screen probe returned"
.balign 4
.section .note.GNU-stack,"",%progbits
