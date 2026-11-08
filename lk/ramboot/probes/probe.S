/* SPDX-License-Identifier: MIT */
.section .header,"ax"
.global probe_start
probe_start:
    b ramboot_probe
    .word __image_end - probe_start
    .ascii "PLK1"
    .word 1
    .quad 0, 0

.section .note.GNU-stack,"",%progbits
