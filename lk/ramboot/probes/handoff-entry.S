/* SPDX-License-Identifier: MIT */
.section .header,"ax"
.global probe_start
probe_start:
    b handoff_probe
    .word __image_end - probe_start
    .ascii "PLK1"
    .word 1
    .quad 0, 0

.section .text.handoff_jump,"ax"
.global handoff_cleanup_and_jump
handoff_cleanup_and_jump:
    movz x16, #0xb904
    movk x16, #0xb500, lsl #16
    blr x16
    movz x16, #0x8280, lsl #16
    br x16

/* Copied as position-independent instructions to 0x82800000. */
.section .text.handoff_landing,"ax"
.global handoff_landing_start
.global handoff_landing_end
handoff_landing_start:
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
1:  wfe
    b 1b
handoff_landing_end:

.section .note.GNU-stack,"",%progbits
