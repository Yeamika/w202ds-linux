/* SPDX-License-Identifier: MIT */
.section .header,"ax"
.global probe_start
probe_start:
    b memory_read
    .word __image_end - probe_start
    .ascii "PLK1"
    .word 1
    /* The host patches these read parameters before downloading to RAM. */
    .quad 0x82ffe000, 0x2000
.section .note.GNU-stack,"",%progbits
