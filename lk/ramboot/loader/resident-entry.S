/* SPDX-License-Identifier: MIT */
.section .header,"ax"
.global probe_start
probe_start:
    b resident_ramboot
    .word __image_end - probe_start
    .ascii "PLK1"
    .word 1
    .quad 0, 0

/* Keep the tested SEC transition in a single shared source file. */
#define W202_RESIDENT_HANDOFF_ONLY
#include "enter-entry.S"
