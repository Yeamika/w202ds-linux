/* SPDX-License-Identifier: MIT
 * Bounded EL2 call into suspended LK; both address spaces identity-map this
 * trampoline and its reserved context. IRQ/FIQ remain masked throughout LK.
 * No GIC configuration, UEFI stack, framebuffer, or display controller writes.
 */
#include "LkReturn.h"
#include "LkFastboot.h"
.macro save_sys reg, index
    mrs x9, \reg
    str x9, [x16, #(8*\index)]
.endm
.macro load_sys reg, index
    ldr x9, [x16, #(8*\index)]
    msr \reg, x9
.endm
.section .text.lk_fb,"ax"
.global lk_fb_gateway
lk_fb_gateway:
    mrs x9, CurrentEL
    cmp x9, #8
    b.ne unsupported
    ldr x16, =W202_LK_FB_CONTEXT
    save_sys daif,22
    msr daifset,#0xf
    mov x9,sp
    stp x9,x18,[x16]
    stp x19,x20,[x16,#16]
    stp x21,x22,[x16,#32]
    stp x23,x24,[x16,#48]
    stp x25,x26,[x16,#64]
    stp x27,x28,[x16,#80]
    stp x29,x30,[x16,#96]
    stp d8,d9,[x16,#112]
    stp d10,d11,[x16,#128]
    stp d12,d13,[x16,#144]
    stp d14,d15,[x16,#160]
    save_sys sctlr_el2,23
    save_sys ttbr0_el2,24
    save_sys tcr_el2,25
    save_sys mair_el2,26
    save_sys vbar_el2,27
    save_sys hcr_el2,28
    save_sys cptr_el2,29
    save_sys tpidr_el2,30
    save_sys tpidr_el1,31
    save_sys tpidr_el0,32
    save_sys sp_el0,33
    save_sys sp_el1,34
    save_sys vbar_el1,35
    save_sys sctlr_el1,36
    save_sys cpacr_el1,37
    save_sys fpcr,38
    save_sys fpsr,39
    save_sys tpidrro_el0,55
    save_sys mdcr_el2,56
    str x0,[x16,#512]
    /* Disable allocations before cleaning every data/unified cache. Keep
     * translation active until all EFI dirty lines have reached memory. */
    bl cache_off
    bl install_status
    bl install_print
    ldr x16, =W202_LK_CONTEXT_ADDRESS
    bl load_environment
    ldr x18,[x16,#8]
    ldr x9, =W202_LK_FB_STACK
    mov sp,x9
    load_sys sctlr_el2,23
    isb
    /* Neither direct LCD output nor diagnostic printk may reach the panel.
     * The RAM-only printk/LCD stubs were installed with MMU off above. */
    ldr x16, =W202_LK_FB_CONTEXT
    ldr x0,[x16,#512]
    bl lk_fb_service
    ldr x16, =W202_LK_FB_CONTEXT
    str x0,[x16,#512]
    bl cache_off
    /* Restore the three original instruction pairs before returning to EFI
     * or later resuming Android/native Fastboot. No flash bytes are changed. */
    bl restore_status
    bl restore_print
    ldr x16, =W202_LK_FB_CONTEXT
    bl load_environment
    load_sys sctlr_el2,23
    isb
    load_sys fpcr,38
    load_sys fpsr,39
    ldp d8,d9,[x16,#112]
    ldp d10,d11,[x16,#128]
    ldp d12,d13,[x16,#144]
    ldp d14,d15,[x16,#160]
    ldp x19,x20,[x16,#16]
    ldp x21,x22,[x16,#32]
    ldp x23,x24,[x16,#48]
    ldp x25,x26,[x16,#64]
    ldp x27,x28,[x16,#80]
    ldp x29,x30,[x16,#96]
    ldp x9,x18,[x16]
    mov sp,x9
    ldr x0,[x16,#512]
    load_sys daif,22
    isb
    ret
unsupported:
    mov x0,#-1
    ret

/* Clobber only volatile registers, no stack or cacheable writes. The W202DS
 * CPUs use the pre-CCIDX CCSIDR layout (checked before publishing this ABI).
 * This is deliberately a complete clean across context/allocator boundaries. */
cache_off:
    mrs x9,sctlr_el2
    bic x9,x9,#4
    msr sctlr_el2,x9
    isb
    dsb sy
    mrs x10,clidr_el1
    ubfx x11,x10,#24,#3
    mov x12,#0
    mrs x17,csselr_el1
cache_level:
    cmp x12,x11
    b.hs caches_clean
    add x13,x12,x12,lsl #1
    lsr x13,x10,x13
    and x13,x13,#7
    cmp x13,#2
    b.lo next_level
    lsl x0,x12,#1
    msr csselr_el1,x0
    isb
    mrs x13,ccsidr_el1
    and x14,x13,#7
    add x14,x14,#4
    ubfx x15,x13,#3,#10
    clz w1,w15
    ubfx x13,x13,#13,#15
cache_set:
    mrs x2,ccsidr_el1
    ubfx x15,x2,#3,#10
cache_way:
    /* set index must be shifted by log2(cache line bytes). */
    lsl x3,x13,x14
    lsl x2,x15,x1
    orr x2,x2,x3
    orr x2,x2,x0
    dc cisw,x2
    subs x15,x15,#1
    b.ge cache_way
    subs x13,x13,#1
    b.ge cache_set
next_level:
    add x12,x12,#1
    b cache_level
caches_clean:
    msr csselr_el1,x17
    dsb sy
    mrs x9,sctlr_el2
    bic x9,x9,#1
    bic x9,x9,#0x1000
    msr sctlr_el2,x9
    isb
    ic iallu
    dsb sy
    isb
    ret

load_environment:
    /* x16 points to the same versioned CPU layout used by return.S. */
    load_sys hcr_el2,28
    load_sys cptr_el2,29
    load_sys cpacr_el1,37
    load_sys mdcr_el2,56
    load_sys ttbr0_el2,24
    load_sys tcr_el2,25
    load_sys mair_el2,26
    load_sys vbar_el2,27
    load_sys tpidr_el2,30
    load_sys tpidr_el1,31
    load_sys tpidr_el0,32
    load_sys tpidrro_el0,55
    load_sys sp_el0,33
    load_sys sp_el1,34
    load_sys vbar_el1,35
    load_sys sctlr_el1,36
    dsb sy
    tlbi alle2
    ic iallu
    dsb sy
    isb
    ret

/* Redirect native module INFO into bounded RAM storage. No nested USB calls.
 * Patched only with MMU/caches off and restored before returning to UEFI. */
install_status:
    ldr x16,=W202_LK_FB_CONTEXT
    ldr x9,=0xb500e11c
    ldr w10,[x9]
    ldr w11,[x9,#4]
    ldr w12,[x9,#8]
    ldr w13,[x9,#12]
    add x17,x16,#560
    stp w10,w11,[x17]
    stp w12,w13,[x17,#8]
    ldr x10,=0xd61f020018000050 // ldr w16, .+8; br x16 (32-bit native VA)
    ldr x11,=lk_fb_capture_status
    str w10,[x9]
    lsr x10,x10,#32
    str w10,[x9,#4]
    str w11,[x9,#8]
    lsr x11,x11,#32
    str w11,[x9,#12]
    dsb sy
    ic iallu
    dsb sy
    isb
    ret
restore_status:
    ldr x16,=W202_LK_FB_CONTEXT
    ldr x9,=0xb500e11c
    add x17,x16,#560
    ldp w10,w11,[x17]
    ldp w12,w13,[x17,#8]
    str w10,[x9]
    str w11,[x9,#4]
    str w12,[x9,#8]
    str w13,[x9,#12]
    dsb sy
    ic iallu
    dsb sy
    isb
    ret

/* Called with M/C/I off, against the pinned single-CPU LK text. */
install_print:
    ldr x16,=W202_LK_FB_CONTEXT
    adr x9,print_addresses
    add x10,x16,#528
    ldr x11,=0xd65f03c052800000 /* mov w0,#0; ret */
    mov x12,#3
1:  ldr x13,[x9],#8
    ldr w14,[x13]
    ldr w15,[x13,#4]
    stp w14,w15,[x10],#8
    str w11,[x13]
    lsr x15,x11,#32
    str w15,[x13,#4]
    subs x12,x12,#1
    b.ne 1b
    dsb sy
    ic iallu
    dsb sy
    isb
    ret
restore_print:
    ldr x16,=W202_LK_FB_CONTEXT
    adr x9,print_addresses
    add x10,x16,#528
    mov x12,#3
1:  ldr x13,[x9],#8
    ldp w14,w15,[x10],#8
    str w14,[x13]
    str w15,[x13,#4]
    subs x12,x12,#1
    b.ne 1b
    dsb sy
    ic iallu
    dsb sy
    isb
    ret
.balign 8
print_addresses: .quad 0xb5035e94,0xb5035c78,0xb5087d98
.ltorg
.section .note.GNU-stack,"",%progbits
