/* SPDX-License-Identifier: MIT */
#include <stdint.h>

int ramboot_enter(const char *operation);

#ifndef W202_RECOVERY_WATCHDOG_MS
#define W202_RECOVERY_WATCHDOG_MS 30000
#endif

#define DPU_REG(offset) (*(const volatile uint32_t *)(uintptr_t)(UINT32_C(0x31000000) + (offset)))

static int operation_is(const char *operation, const char *expected)
{
    if (!operation)
        return 0;
    while (*expected && *operation == *expected) {
        ++operation;
        ++expected;
    }
    return *operation == *expected;
}

int resident_ramboot(const char *operation, unsigned mode, uintptr_t handler)
{
    void (*const stop_watchdog)(void) = (void *)0xb50932a0;
    int (*const lcd_init)(void) = (void *)0xb5086e5c;
    void (*const lcd_enable)(void) = (void *)0xb508868c;
    uintptr_t (*const panel)(void) = (void *)0xb5089bfc;
    void (*const backlight)(unsigned) = (void *)0xb50035a4;
    int (*const print)(const char *, ...) = (void *)0xb5035e94;
    uint32_t (*const adi_read)(uint32_t) = (void *)0xb506ce54;
    int (*const adi_write)(uint32_t, uint32_t, uint32_t) = (void *)0xb506cf70;
    void (*const start_watchdog)(uint64_t) = (void *)0xb5093220;
    uintptr_t descriptor;
    uint64_t el;
    uint32_t saved, key_mask;

    if (!operation_is(operation, ":dispatch"))
        return ramboot_enter(operation);
    /* Recovery, fastboot, charging and battery protection remain native.
     * Only the pinned normal/ext-reset handler is an EDK2 startup route. */
    if ((mode != 2 && mode != 16) || handler != UINT64_C(0xb500da88))
        return -1;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    if (el != 8 || *(const volatile unsigned *)0xb50eed8c != 1 ||
        *(const volatile uint32_t *)0xb506ce54 != UINT32_C(0xa9bc7bfd) ||
        *(const volatile uint32_t *)0xb506cf70 != UINT32_C(0xa9bc7bfd) ||
        *(const volatile uint32_t *)0xb508868c != UINT32_C(0xa9bd7bfd) ||
        *(const volatile uint32_t *)0xb50932a0 != UINT32_C(0xa9bd7bfd) ||
        *(const volatile uint32_t *)0xb5093220 != UINT32_C(0xa9bd7bfd) ||
        *(const volatile uint32_t *)0xb5093224 != UINT32_C(0x52800081))
        return -1;
    /* This pinned native call is stop_watchdog, not board initialization.
     * Clear inherited watchdog state BEFORE arming recovery; calling it
     * afterwards would remove reset protection during panel setup. */
    stop_watchdog();
    saved = adi_read(UINT32_C(0x644223ac));
    adi_write(UINT32_C(0x644223ac), (saved & ~UINT32_C(0xff)) | 0x30, 1);
    if ((adi_read(UINT32_C(0x644223ac)) & 0xff) != 0x30) {
        adi_write(UINT32_C(0x644223ac), saved, 1);
        return -1;
    }
    start_watchdog(W202_RECOVERY_WATCHDOG_MS);
    print("EDK2-RESIDENT: mode=%u cpu=%u initializing inherited display\n",
          mode, *(const volatile unsigned *)0xb50eed8c);
    if (lcd_init()) {
        print("EDK2-RESIDENT: LCD initialization failed; recovering fastboot\n");
        return -1;
    }
    descriptor = panel();
    if (!descriptor || descriptor == UINT64_C(0xdeaddeaddeaddead)) {
        print("EDK2-RESIDENT: invalid panel; recovering fastboot\n");
        return -1;
    }
    /* lcd_init probes the panel and clears its framebuffer, but does not
     * configure a scanout layer. Fastboot used to supply that missing flip
     * before RAMBoot; normal cold boot has no such inherited display.
     * Execute the matching native layer setup before handing it to GOP. */
    lcd_enable();
    print("EDK2-RESIDENT: scanout enable=%x addr=%x layer=%x pitch=%u\n",
          DPU_REG(0x2c), DPU_REG(0x30), DPU_REG(0x40), DPU_REG(0x4c));
    if (DPU_REG(0) != 0x600 || DPU_REG(0x18) != 0x078004b0 ||
        DPU_REG(0x2c) != 1 || DPU_REG(0x30) != 0xb0000000 ||
        (DPU_REG(0x40) & 0x007000f0) != 0x00200040 ||
        DPU_REG(0x4c) != 1200) {
        print("EDK2-RESIDENT: incompatible scanout; recovering fastboot\n");
        return -1;
    }
    /* Volume-up is analogue EIC164. Native fastboot requests its input
     * mask, whereas normal boot may leave only mask 0x0b. Preserve the
     * other EIC lines and enable this data input without enabling IRQs. */
    key_mask = adi_read(UINT32_C(0x64420284));
    if (!(key_mask & 0x10))
        adi_write(UINT32_C(0x64420284), key_mask | 0x10, 1);
    if (!(adi_read(UINT32_C(0x64420284)) & 0x10)) {
        print("EDK2-RESIDENT: volume input unavailable; recovering fastboot\n");
        return -1;
    }
    print("EDK2-RESIDENT: display ready cpu=%u\n",
          *(const volatile unsigned *)0xb50eed8c);
    backlight(70);
#ifdef W202_RESIDENT_PREFLIGHT
    /* Same cold-boot path and FD; deliberately let the watchdog return to
     * fastboot so the retained SEC/DXE/application trace can be collected. */
    return ramboot_enter(":init");
#else
    return ramboot_enter(":resident");
#endif
}
