/* SPDX-License-Identifier: MIT */
#include "LkReturn.h"
.macro save_sys reg, index
    mrs x9, \reg
    str x9, [x16, #(8*\index)]
.endm
.macro restore_sys reg, index
    ldr x9, [x16, #(8*\index)]
    msr \reg, x9
.endm
.macro trace_stage stage, clean=1
    ldr x9, =0x82fff000
    mov x10, #\stage
    str x10, [x9, #8]
    .if \clean
    dc cvac, x9
    .endif
    dsb sy
.endm
.section .text.lk_return,"ax"
.global handoff_enter_sec_returnable
handoff_enter_sec_returnable:
    ldr x16, =W202_LK_CONTEXT_ADDRESS
    save_sys daif, 22
    msr daifset, #0xf
    mov x9, sp
    stp x9, x18, [x16]
    stp x19, x20, [x16, #16]
    stp x21, x22, [x16, #32]
    stp x23, x24, [x16, #48]
    stp x25, x26, [x16, #64]
    stp x27, x28, [x16, #80]
    stp x29, x30, [x16, #96]
    stp d8, d9, [x16, #112]
    stp d10, d11, [x16, #128]
    stp d12, d13, [x16, #144]
    stp d14, d15, [x16, #160]
    save_sys sctlr_el2, 23
    save_sys ttbr0_el2, 24
    save_sys tcr_el2, 25
    save_sys mair_el2, 26
    save_sys vbar_el2, 27
    save_sys hcr_el2, 28
    save_sys cptr_el2, 29
    save_sys tpidr_el2, 30
    save_sys tpidr_el1, 31
    save_sys tpidr_el0, 32
    save_sys sp_el0, 33
    save_sys sp_el1, 34
    save_sys vbar_el1, 35
    save_sys sctlr_el1, 36
    save_sys cpacr_el1, 37
    save_sys fpcr, 38
    save_sys fpsr, 39
    save_sys cntp_ctl_el0, 40
    save_sys cntp_cval_el0, 41
    save_sys cntv_ctl_el0, 42
    save_sys cntv_cval_el0, 43
    save_sys cnthp_ctl_el2, 44
    save_sys cnthp_cval_el2, 45
    save_sys cntvoff_el2, 46
    save_sys cnthctl_el2, 47
    save_sys cntkctl_el1, 48
    save_sys icc_sre_el2, 49
    save_sys icc_sre_el1, 50
    save_sys icc_ctlr_el1, 51
    save_sys icc_pmr_el1, 52
    save_sys icc_bpr1_el1, 53
    save_sys icc_igrpen1_el1, 54
    save_sys tpidrro_el0, 55
    save_sys mdcr_el2, 56
    mov x19, x0
    trace_stage 0x11
    bl lk_return_save
    cbnz w0, save_failed
    trace_stage 0x12
    ldr x16, =0xb500b904
    blr x16
    trace_stage 0x22, 0
    dsb sy
    br x19
save_failed:
    trace_stage 0x13
    mov w0, #-1
    b restore_abi

/* EFI reaches this physical address at EL2 with M/C/I off and DAIF masked. */
.global lk_return_resume
lk_return_resume:
    ldr x9, =0x82fff000
    mov x10, #0x61
    str x10, [x9, #8]
    dsb sy
    ldr x16, =W202_LK_CONTEXT_ADDRESS
    mov x19, x0
    msr daifset, #0xf
    msr cntp_ctl_el0, xzr
    msr cntv_ctl_el0, xzr
    msr cnthp_ctl_el2, xzr
    restore_sys hcr_el2, 28
    restore_sys cptr_el2, 29
    restore_sys cpacr_el1, 37
    restore_sys mdcr_el2, 56
    restore_sys ttbr0_el2, 24
    restore_sys tcr_el2, 25
    restore_sys mair_el2, 26
    restore_sys vbar_el2, 27
    restore_sys vbar_el1, 35
    restore_sys sctlr_el1, 36
    restore_sys tpidr_el2, 30
    restore_sys tpidr_el1, 31
    restore_sys tpidr_el0, 32
    restore_sys tpidrro_el0, 55
    restore_sys sp_el0, 33
    restore_sys sp_el1, 34
    dsb sy
    tlbi alle2
    ic iallu
    dsb sy
    isb
    restore_sys sctlr_el2, 23
    isb
    ldr x9, [x16]
    mov sp, x9
    bl lk_return_restore
    cbnz w0, restore_failed
    ldr x9, =0x82fff000
    mov x10, #0x62
    str x10, [x9, #8]
    dc cvac, x9
    dsb sy
    ldr x16, =W202_LK_CONTEXT_ADDRESS
    restore_sys icc_sre_el2, 49
    isb
    restore_sys icc_sre_el1, 50
    isb
    restore_sys icc_ctlr_el1, 51
    restore_sys icc_pmr_el1, 52
    restore_sys icc_bpr1_el1, 53
    restore_sys icc_igrpen1_el1, 54
    restore_sys cntvoff_el2, 46
    restore_sys cnthctl_el2, 47
    restore_sys cntkctl_el1, 48
    restore_sys cntp_cval_el0, 41
    restore_sys cntv_cval_el0, 43
    restore_sys cnthp_cval_el2, 45
    restore_sys cntp_ctl_el0, 40
    restore_sys cntv_ctl_el0, 42
    restore_sys cnthp_ctl_el2, 44
    mov w0, w19
restore_abi:
    ldr x16, =W202_LK_CONTEXT_ADDRESS
    restore_sys fpcr, 38
    restore_sys fpsr, 39
    ldp d8, d9, [x16, #112]
    ldp d10, d11, [x16, #128]
    ldp d12, d13, [x16, #144]
    ldp d14, d15, [x16, #160]
    ldp x19, x20, [x16, #16]
    ldp x21, x22, [x16, #32]
    ldp x23, x24, [x16, #48]
    ldp x25, x26, [x16, #64]
    ldp x27, x28, [x16, #80]
    ldp x29, x30, [x16, #96]
    ldp x9, x18, [x16]
    mov sp, x9
    dsb sy
    restore_sys daif, 22
    isb
    ret
restore_failed:
    /* Keep the PMIC watchdog armed; do not resume a partial interrupt state. */
    wfe
    b restore_failed
.global lk_return_android
lk_return_android:
    /* UEFI may have consumed almost all of the RAM payload's 30-second
     * deadline. Give native loading and Android userspace a fresh bounded
     * window; the OS takes over this watchdog after its PM service starts.
     * Preserve both the native target and the caller's callee-saved state. */
    stp x19, x30, [sp, #-16]!
    mov x19, x0
    ldr x0, =180000
    ldr x16, =0xb5093220
    blr x16
    mov x9, x19
    ldp x19, x30, [sp], #16
    mov x0, x9
    /* The PLK1 hook made 0x86000000 RO+X. Native boot writes its image there.
     * Restore RW+XN only after leaving this module, using the pinned LK helper.
     * Prepare the native handler's stack frame and enter after its prologue;
     * its normal epilogue then retains a valid Fastboot fallback. The caller
     * selects native continue for a RAM command, or normal_mode for a cold
     * resident that never initialized Fastboot USB. */
    mov x30, x0
    ldr x9, =0xb5012a80
    stp x29, x9, [sp, #-16]!
    ldr x21, =0xb5f01180
    ldr x7, =0x0060000086000711
    ldr x0, =0xb5122000
    ldr x16, =0xb500e2a0
    br x16
.ltorg
.section .note.GNU-stack,"",%progbits
