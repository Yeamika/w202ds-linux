/* SPDX-License-Identifier: MIT
 * Small extensions to the pinned native LK. No EDK2, GIC snapshot or heap.
 * The native Fastboot/USB stack and RAMBoot permission helper stay in use.
 */
.section .text,"ax"
.global lk_debug_oem, lk_debug_dispatch, lk_debug_jump

/* Android platform-tools: fastboot oem jump-rmk :probe / :init.
 * Preserve all other OEM commands by replaying the displaced prologue.
 */
lk_debug_oem:
    mov x9, x0
    ldrb w10, [x9]
    cmp w10, #32
    b.ne 1f
    add x9, x9, #1
1:
    adr x10, command
    mov x11, x9
2:
    ldrb w12, [x10], #1
    cbz w12, matched
    ldrb w13, [x11], #1
    cmp w12, w13
    b.ne native_oem
    b 2b
matched:
    ldrb w12, [x11]
    cbz w12, default_operation
    cmp w12, #32
    b.ne 3f
    add x11, x11, #1
    ldrb w12, [x11]
    cbz w12, default_operation
3:
    cmp w12, #58
    b.ne native_oem
    mov x0, x11
    b lk_jump
default_operation:
    adr x0, probe
    b lk_jump
native_oem:
    sub sp, sp, #0x3a0
    b lk_oem_continue
command: .asciz "jump-rmk"
probe: .asciz ":probe"
.balign 4

/* Only normal/external-reset dispatch changes. Charging, recovery,
 * bootloader requests and battery protection keep their native handlers.
 */
lk_debug_dispatch:
#if W202_DEBUG_USB_RECOVERY
    cmp w19, #2
    b.eq 1f
    cmp w19, #16
    b.ne 2f
1:
    ldr x9, =0xb500da88
    cmp x0, x9
    b.eq lk_fastboot_mode
2:
#endif
    br x0

#if W202_DEBUG_WATCHDOG || W202_DEBUG_SCREEN
/* Same PLK1 validation and temporary RO+X mapping as the original hook.
 * Only a module which returns under the native ABI can be resumed in place;
 * a hung payload resets into native Fastboot instead of restoring a full CPU
 * or interrupt-controller snapshot.
 */
lk_debug_jump:
    stp x29, x30, [sp, #-64]!
    mov x29, sp
    stp x19, x20, [sp, #16]
    stp x21, x22, [sp, #32]
    stp x23, x24, [sp, #48]
    mov x19, x0
    mov x20, #0x86000000
    cmp x1, x20
    b.ne reject
    cmp x2, #32
    b.lo reject
    tst x2, #0xffffffffff000000
    b.ne reject
    ldr w3, [x20, #4]
    cmp x2, x3
    b.ne reject
    ldp w3, w4, [x20, #8]
    mov w5, #0x4c50
    movk w5, #0x314b, lsl #16
    cmp w3, w5
    b.ne reject
    cmp w4, #1
    b.ne reject
    mrs x3, CurrentEL
    cmp x3, #8
    b.ne reject
    ldr x3, =0xb50eed8c
    ldr w3, [x3]
    cmp w3, #1
    b.ne reject
    adrp x21, ptable_page
    add x21, x21, #0x180
    ldr x22, [x21]
    tbnz x22, #1, reject
    and x5, x22, #0x0000ffffffe00000
    cmp x5, x20
    b.ne reject
#if W202_DEBUG_WATCHDOG
    bl arm_watchdog
    cbnz w1, reject
    mov w23, w0
#endif
#if W202_DEBUG_SCREEN
    adr x0, message_running
    bl lk_debug_screen
#endif
    /* Native calls may clobber the argument registers. Read length again. */
    mov x0, x20
    ldr w1, [x20, #4]
    bl lk_icache_sync
    bic x7, x22, #0x0060000000000000
    orr x7, x7, #0x80
    bl permissions
    mov x0, x19
    blr x20
    mov w19, w0
    mov x7, x22
    bl permissions
#if W202_DEBUG_WATCHDOG
    bl lk_stop_watchdog
    ldr w0, =0x644223ac
    mov w1, w23
    mov w2, #1
    bl lk_adi_write
#endif
    cbnz w19, reject
#if W202_DEBUG_SCREEN
    adr x0, message_okay
    bl lk_debug_screen
#endif
    adrp x0, okay_page
    add x0, x0, #0x7e0
    b reply
reject:
#if W202_DEBUG_SCREEN
    adr x0, message_rejected
    bl lk_debug_screen
#endif
    adrp x0, okay_page
    add x0, x0, #0x748
reply:
    mov x1, xzr
    bl lk_status
    ldp x23, x24, [sp, #48]
    ldp x21, x22, [sp, #32]
    ldp x19, x20, [sp, #16]
    ldp x29, x30, [sp], #64
    ret

#if W202_DEBUG_WATCHDOG
/* x0: saved reset monitor; w1: zero on success, -1 on failure.
 * Native Fastboot idles without this watchdog. Arm only for payload execution.
 */
arm_watchdog:
    stp x29, x30, [sp, #-32]!
    mov x29, sp
    stp x19, x20, [sp, #16]
    bl lk_stop_watchdog
    ldr w0, =0x644223ac
    bl lk_adi_read
    mov w19, w0
    bic w1, w19, #0xff
    orr w1, w1, #0x30 /* native bootloader/Fastboot reboot reason */
    ldr w0, =0x644223ac
    mov w2, #1
    bl lk_adi_write
    ldr w0, =0x644223ac
    bl lk_adi_read
    and w0, w0, #0xff
    cmp w0, #0x30
    b.ne arm_failed
    ldr w0, =W202_DEBUG_WATCHDOG_MS
    bl lk_start_watchdog
    mov w0, w19
    mov w1, #0
    b arm_return
arm_failed:
    ldr w0, =0x644223ac
    mov w1, w19
    mov w2, #1
    bl lk_adi_write
    mov w0, w19
    mov w1, #-1
arm_return:
    ldp x19, x20, [sp, #16]
    ldp x29, x30, [sp], #32
    ret
#endif
#else
lk_debug_jump:
    b lk_jump
#endif

#if W202_DEBUG_SCREEN
.global lk_debug_fastboot
/* Run after the native Fastboot splash, preserving the displaced prologue. */
lk_debug_fastboot:
    stp x29, x30, [sp, #-48]!
    mov x29, sp
    stp x0, x1, [sp, #16]
    str x2, [sp, #32]
    adr x0, message_ready
    bl lk_debug_screen
    ldp x0, x1, [sp, #16]
    ldr x2, [sp, #32]
    ldp x29, x30, [sp], #48
    stp x29, x30, [sp, #-272]!
    b lk_fastboot_continue
#endif

#if W202_DEBUG_SCREEN || W202_FASTBOOT_FLASH
.global lk_debug_download
/* The original download handler remains responsible for USB and its reply. */
lk_debug_download:
    stp x29, x30, [sp, #-48]!
    mov x29, sp
    stp x0, x1, [sp, #16]
    str x2, [sp, #32]
#if W202_DEBUG_SCREEN
    adr x0, message_download
    bl lk_debug_screen
#endif
    ldp x0, x1, [sp, #16]
    ldr x2, [sp, #32]
    bl native_download
    str x0, [sp, #16]
#if W202_DEBUG_SCREEN
    ldr x9, =0xb5122000 /* native Fastboot response and state */
    ldr w10, [x9, #64]
    cmp w10, #2
    b.ne download_failed
    ldr w10, [x9]
    mov w11, #0x4b4f
    movk w11, #0x5941, lsl #16 /* "OKAY" */
    cmp w10, w11
    b.ne download_failed
    adr x0, message_download_okay
    b download_done
download_failed:
    adr x0, message_download_failed
download_done:
    bl lk_debug_screen
#endif
    ldr x0, [sp, #16]
    ldp x29, x30, [sp], #48
    ret
native_download:
#if W202_FASTBOOT_FLASH
    b lk_debug_download_raw
#else
    stp x29, x30, [sp, #-256]!
    b lk_download_continue
#endif
#endif

#if W202_DEBUG_SCREEN
message_ready: .asciz "Fastboot ready; waiting for host"
message_download: .asciz "RAM download in progress"
message_download_okay: .asciz "RAM download OK; ready to execute"
message_download_failed: .asciz "RAM download failed; check host output"
#if W202_DEBUG_WATCHDOG
message_running: .asciz "RAMBoot executing; watchdog armed"
#else
message_running: .asciz "RAMBoot executing; watchdog disabled"
#endif
message_okay: .asciz "RAMBoot returned OK; Fastboot ready"
message_rejected: .asciz "RAMBoot rejected or returned error; check host output"
.balign 4
#endif
.ltorg
.section .note.GNU-stack,"",%progbits
