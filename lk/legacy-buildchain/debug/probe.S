/* SPDX-License-Identifier: MIT
 * PLK1 smoke test; the hang variant deliberately leaves reset protection live.
 */
.section .text,"ax"
.global probe_start
probe_start:
    b payload
    .word image_end - probe_start
    .ascii "PLK1"
    .word 1
    .quad 0, 0
payload:
#if W202_DEBUG_HANG
    wfe
    b payload
#else
    mov w0, #0
    ret
#endif
image_end:
.section .note.GNU-stack,"",%progbits
