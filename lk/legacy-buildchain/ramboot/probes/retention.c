/* SPDX-License-Identifier: MIT */
#include <stdint.h>

/* Last page of the provisional FD; this is a diagnostic reservation only. */
#define RETAIN_BASE UINT64_C(0x82fff000)
#define RETAIN_MAGIC UINT64_C(0x5752544e52414d31)
#define RETAIN_VALUE UINT64_C(0x8a6d35c27914efb0)
#define POR_RST_MONITOR UINT32_C(0x644223ac)
#define FASTBOOT_REBOOT UINT32_C(0x30)

static int (*const lk_printf)(const char *, ...) = (void *)0xb5035e94;
static uint32_t (*const adi_read)(uint32_t) = (void *)0xb506ce54;
static int (*const adi_write)(uint32_t, uint32_t, uint32_t) = (void *)0xb506cf70;
static void (*const start_watchdog)(uint64_t) = (void *)0xb5093220;
static const volatile uint32_t *const lk_cpu_count = (void *)0xb50eed8c;
static const volatile uint64_t *const lk_l2 = (void *)0xb5f01000;
static volatile uint64_t *const marker = (void *)(uintptr_t)RETAIN_BASE;

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

int retention_probe(const char *operation)
{
    uint64_t el, ctr;
    uint32_t saved, verify;

    if (!operation_is(operation, ":probe") && !operation_is(operation, ":init"))
        return -1;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    if (el != 8 || *lk_cpu_count != 1 ||
        (UINT64_C(4) << ((ctr >> 16) & 15)) < 24 ||
        lk_l2[0x17] != UINT64_C(0x0060000082e00711) ||
        *(const volatile uint32_t *)0xb5093220 != UINT32_C(0xa9bd7bfd) ||
        *(const volatile uint32_t *)0xb5093224 != UINT32_C(0x52800081) ||
        *(const volatile uint32_t *)0xb506ce54 != UINT32_C(0xa9bc7bfd) ||
        *(const volatile uint32_t *)0xb506cf70 != UINT32_C(0xa9bc7bfd)) {
        lk_printf("RAMBOOT-RETAIN: incompatible LK state or page table\n");
        return -1;
    }

    if (operation_is(operation, ":probe")) {
        __asm__ volatile("dc ivac, %0; dsb sy" :: "r"(marker) : "memory");
        lk_printf("RAMBOOT-RETAIN: magic=%lx value=%lx inverse=%lx\n",
                  (unsigned long)marker[0], (unsigned long)marker[1],
                  (unsigned long)marker[2]);
        return marker[0] == RETAIN_MAGIC && marker[1] == RETAIN_VALUE &&
               marker[2] == ~RETAIN_VALUE ? 0 : -1;
    }

    saved = adi_read(POR_RST_MONITOR);
    adi_write(POR_RST_MONITOR, (saved & ~UINT32_C(0xff)) | FASTBOOT_REBOOT, 1);
    verify = adi_read(POR_RST_MONITOR);
    if ((verify & UINT32_C(0xff)) != FASTBOOT_REBOOT) {
        adi_write(POR_RST_MONITOR, saved, 1);
        lk_printf("RAMBOOT-RETAIN: reboot monitor verification failed: %x\n", verify);
        return -1;
    }

    marker[0] = RETAIN_MAGIC;
    marker[1] = RETAIN_VALUE;
    marker[2] = ~RETAIN_VALUE;
    __asm__ volatile("dc cvac, %0; dsb sy" :: "r"(marker) : "memory");
    lk_printf("RAMBOOT-RETAIN: wrote marker at %lx; arming 15000 ms\n",
              (unsigned long)RETAIN_BASE);
    start_watchdog(15000);
    return 0;
}
