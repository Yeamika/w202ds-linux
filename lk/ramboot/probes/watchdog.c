/* SPDX-License-Identifier: MIT */
#include <stdint.h>

/* These addresses and instruction checks are specific to the pinned LK. */
#define POR_RST_MONITOR UINT32_C(0x644223ac)
#define NORMAL_REBOOT UINT32_C(0x40)
#define TIMEOUT_MS UINT64_C(15000)

static int (*const lk_printf)(const char *, ...) = (void *)0xb5035e94;
static uint32_t (*const adi_read)(uint32_t) = (void *)0xb506ce54;
static int (*const adi_write)(uint32_t, uint32_t, uint32_t) = (void *)0xb506cf70;
static void (*const start_watchdog)(uint64_t) = (void *)0xb5093220;
static const volatile uint32_t *const lk_cpu_count = (void *)0xb50eed8c;

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

int watchdog_probe(const char *operation)
{
    uint64_t el;
    uint32_t saved, verify;

    if (!operation_is(operation, ":probe") && !operation_is(operation, ":init"))
        return -1;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    if (el != 8 || *lk_cpu_count != 1 ||
        *(const volatile uint32_t *)0xb5093220 != UINT32_C(0xa9bd7bfd) ||
        *(const volatile uint32_t *)0xb5093224 != UINT32_C(0x52800081) ||
        *(const volatile uint32_t *)0xb506ce54 != UINT32_C(0xa9bc7bfd) ||
        *(const volatile uint32_t *)0xb506cf70 != UINT32_C(0xa9bc7bfd)) {
        lk_printf("RAMBOOT-WATCHDOG: incompatible LK state\n");
        return -1;
    }

    saved = adi_read(POR_RST_MONITOR);
    if (operation_is(operation, ":probe")) {
        lk_printf("RAMBOOT-WATCHDOG: ready; reboot monitor=%x\n", saved);
        return 0;
    }

    adi_write(POR_RST_MONITOR, (saved & ~UINT32_C(0xff)) | NORMAL_REBOOT, 1);
    verify = adi_read(POR_RST_MONITOR);
    if ((verify & UINT32_C(0xff)) != NORMAL_REBOOT) {
        adi_write(POR_RST_MONITOR, saved, 1);
        lk_printf("RAMBOOT-WATCHDOG: reboot monitor verification failed: %x\n", verify);
        return -1;
    }

    lk_printf("RAMBOOT-WATCHDOG: arming %lu ms; normal reboot monitor=%x\n",
              (unsigned long)TIMEOUT_MS, verify);
    start_watchdog(TIMEOUT_MS);
    return 0;
}
