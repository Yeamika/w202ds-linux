/* SPDX-License-Identifier: MIT */
#include <stddef.h>
#include <stdint.h>

#define LANDING_BASE UINT64_C(0x82800000)
#define TRACE_BASE UINT64_C(0x82fff000)
#define TRACE_MAGIC UINT64_C(0x57323032484e4431)
#define POR_RST_MONITOR UINT32_C(0x644223ac)
#define FASTBOOT_REBOOT UINT32_C(0x30)

extern const unsigned char handoff_landing_start[], handoff_landing_end[];
__attribute__((noreturn)) void handoff_cleanup_and_jump(void);

static int (*const lk_printf)(const char *, ...) = (void *)0xb5035e94;
static uint32_t (*const adi_read)(uint32_t) = (void *)0xb506ce54;
static int (*const adi_write)(uint32_t, uint32_t, uint32_t) = (void *)0xb506cf70;
static void (*const start_watchdog)(uint64_t) = (void *)0xb5093220;
static const volatile uint32_t *const lk_cpu_count = (void *)0xb50eed8c;
static const volatile uint64_t *const lk_l2 = (void *)0xb5f01000;
static volatile uint64_t *const trace = (void *)(uintptr_t)TRACE_BASE;

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

static void cache_lines(uintptr_t address, size_t size, uintptr_t line, int clean)
{
    uintptr_t end = address + size;
    for (address &= ~(line - 1); address < end; address += line) {
        if (clean)
            __asm__ volatile("dc cvac, %0" :: "r"(address) : "memory");
        else
            __asm__ volatile("dc ivac, %0" :: "r"(address) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

int handoff_probe(const char *operation)
{
    uint64_t el, ctr, sctlr;
    uintptr_t line;
    uint32_t saved, verify;
    size_t size;

    if (!operation_is(operation, ":probe") && !operation_is(operation, ":init"))
        return -1;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    line = (uintptr_t)4 << ((ctr >> 16) & 15);
    if (el != 8 || *lk_cpu_count != 1 || line < 16 || line > 2048 ||
        lk_l2[0x14] != UINT64_C(0x0060000082800711) ||
        lk_l2[0x17] != UINT64_C(0x0060000082e00711) ||
        *(const volatile uint32_t *)0xb500b904 != UINT32_C(0xa9bf7bfd) ||
        *(const volatile uint32_t *)0xb5093220 != UINT32_C(0xa9bd7bfd) ||
        *(const volatile uint32_t *)0xb5093224 != UINT32_C(0x52800081) ||
        *(const volatile uint32_t *)0xb506ce54 != UINT32_C(0xa9bc7bfd) ||
        *(const volatile uint32_t *)0xb506cf70 != UINT32_C(0xa9bc7bfd)) {
        lk_printf("RAMBOOT-HANDOFF: incompatible LK state or page table\n");
        return -1;
    }

    if (operation_is(operation, ":probe")) {
        cache_lines(TRACE_BASE, 4096, line, 0);
        lk_printf("RAMBOOT-HANDOFF: magic=%lx stage=%lx EL=%lx SCTLR=%lx DAIF=%lx SP=%lx\n",
                  (unsigned long)trace[0], (unsigned long)trace[1],
                  (unsigned long)trace[2], (unsigned long)trace[3],
                  (unsigned long)trace[4], (unsigned long)trace[5]);
        if (trace[0] == TRACE_MAGIC && trace[7] && trace[7] < UINT64_C(0x100000000)) {
            const volatile unsigned char *log = (const void *)(uintptr_t)(TRACE_BASE + 64);
            uint64_t total = trace[7];
            size_t count = total < 4032 ? (size_t)total : 4032;
            size_t start = total < 4032 ? 0 : (size_t)(total % 4032);
            char text[129];
            while (count) {
                size_t n = count < sizeof(text) - 1 ? count : sizeof(text) - 1;
                for (size_t i = 0; i < n; ++i) {
                    unsigned char ch = log[(start + i) % 4032];
                    text[i] = (ch >= 32 && ch < 127) || ch == '\n' || ch == '\r' || ch == '\t' ? ch : '?';
                }
                text[n] = 0;
                lk_printf("EDK2-RAM: %s", text);
                start = (start + n) % 4032;
                count -= n;
            }
        }
        return trace[0] == TRACE_MAGIC ? 0 : -1;
    }

    size = (size_t)(handoff_landing_end - handoff_landing_start);
    if (!size || size > 4096)
        return -1;
    volatile unsigned char *dest = (void *)(uintptr_t)LANDING_BASE;
    for (size_t i = 0; i < size; ++i)
        dest[i] = handoff_landing_start[i];
    for (size_t i = 0; i < size; ++i)
        if (dest[i] != handoff_landing_start[i])
            return -1;
    cache_lines(LANDING_BASE, size, line, 1);

    saved = adi_read(POR_RST_MONITOR);
    adi_write(POR_RST_MONITOR, (saved & ~UINT32_C(0xff)) | FASTBOOT_REBOOT, 1);
    verify = adi_read(POR_RST_MONITOR);
    if ((verify & UINT32_C(0xff)) != FASTBOOT_REBOOT) {
        adi_write(POR_RST_MONITOR, saved, 1);
        return -1;
    }

    __asm__ volatile("mrs %0, sctlr_el2" : "=r"(sctlr));
    trace[0] = TRACE_MAGIC;
    trace[1] = 0x11;
    trace[2] = el;
    trace[3] = sctlr;
    trace[4] = 0;
    trace[5] = 0;
    cache_lines(TRACE_BASE, 48, line, 1);
    lk_printf("RAMBOOT-HANDOFF: arming watchdog and entering native cleanup\n");
    start_watchdog(15000);
    handoff_cleanup_and_jump();
}
