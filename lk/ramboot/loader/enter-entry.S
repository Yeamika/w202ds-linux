/* SPDX-License-Identifier: MIT */
#ifndef W202_RESIDENT_HANDOFF_ONLY
.section .header,"ax"
.global probe_start
probe_start:
    b ramboot_enter
    .word __image_end - probe_start
    .ascii "PLK1"
    .word 1
    .quad 0, 0
#endif

.section .text.handoff_sec,"ax"
.global handoff_enter_sec
handoff_enter_sec:
    mov x19, x0
    movz x16, #0xb904
    movk x16, #0xb500, lsl #16
    blr x16
    movz x9, #0xf000
    movk x9, #0x82ff, lsl #16
    mov x10, #0x22
    str x10, [x9, #8]
    mrs x10, CurrentEL
    str x10, [x9, #16]
    mrs x10, SCTLR_EL2
    str x10, [x9, #24]
    mrs x10, DAIF
    str x10, [x9, #32]
    mov x10, sp
    str x10, [x9, #40]
    dsb sy
    br x19

.section .note.GNU-stack,"",%progbits
