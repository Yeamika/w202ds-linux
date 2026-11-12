/* SPDX-License-Identifier: MIT */
#include <stddef.h>
#include <stdint.h>
#include "uefi_ramboot.h"
#include "LoaderHandoff.h"
#ifdef W202_LK_RETURN_ENABLED
#include "LkReturn.h"
int handoff_enter_sec_returnable(uint64_t entry);
__attribute__((noreturn)) void lk_return_android(uint64_t native_body);
#endif

#define DOWNLOAD_BASE UINT64_C(0x86000000)
#define IMAGE_OFFSET UINT32_C(0x10000)
#define DOWNLOAD_LIMIT UINT32_C(0x00ffffff)
#define TRACE_BASE UINT64_C(0x82fff000)
#define TRACE_MAGIC UINT64_C(0x57323032484e4431)
#define POR_RST_MONITOR UINT32_C(0x644223ac)
#define FASTBOOT_REBOOT UINT32_C(0x30)
#ifndef W202_RECOVERY_WATCHDOG_MS
#define W202_RECOVERY_WATCHDOG_MS 30000
#endif
#if W202_RECOVERY_WATCHDOG_MS < 30000 || W202_RECOVERY_WATCHDOG_MS > 180000
#error Recovery window must be between 30 and 180 seconds
#endif

__attribute__((noreturn)) void handoff_enter_sec(uint64_t entry);
static int (*const lk_printf)(const char *, ...) = (void *)0xb5035e94;
static uint32_t (*const adi_read)(uint32_t) = (void *)0xb506ce54;
static int (*const adi_write)(uint32_t, uint32_t, uint32_t) = (void *)0xb506cf70;
static void (*const start_watchdog)(uint64_t) = (void *)0xb5093220;
static const volatile uint32_t *const lk_cpu_count = (void *)0xb50eed8c;
static const volatile uint64_t *const lk_l2 = (void *)0xb5f01000;

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *left = a, *right = b;
    for (size_t i = 0; i < n; ++i) {
        if (left[i] != right[i])
            return (int)left[i] - (int)right[i];
    }
    return 0;
}

__attribute__((noinline)) static unsigned long crc32(unsigned long seed,
                                                    const unsigned char *data,
                                                    unsigned int size)
{
    static const uint32_t table[16] = {
        0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac,
        0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
        0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
        0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c
    };
    uint32_t crc = ~(uint32_t)seed;
    while (size--) {
        crc ^= *data++;
        crc = (crc >> 4) ^ table[crc & 15];
        crc = (crc >> 4) ^ table[crc & 15];
    }
    return ~crc;
}

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

static void clean_lines(uintptr_t address, size_t size, uintptr_t line)
{
    uintptr_t end = address + size;
    for (address &= ~(line - 1); address < end; address += line)
        __asm__ volatile("dc cvac, %0" :: "r"(address) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}


int ramboot_enter(const char *operation)
{
    const uint8_t *download = (const void *)(uintptr_t)DOWNLOAD_BASE;
    const uint8_t *boot = download + IMAGE_OFFSET;
    volatile uint8_t *dest = (void *)(uintptr_t)UEFI_RAMBOOT_FD_BASE;
    volatile uint64_t *trace = (void *)(uintptr_t)TRACE_BASE;
    struct uefi_ramboot_image image;
    uint64_t el, ctr, sctlr, daif, sp, entry, start_counter, frequency;
    uintptr_t line;
    uint32_t total, saved, verify, expected_crc;
#ifdef W202_LK_RETURN_ENABLED
    uint32_t key_mask;
#endif

    if (!operation_is(operation, ":probe") && !operation_is(operation, ":init") &&
        !operation_is(operation, ":resident"))
        return -1;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(start_counter));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    line = (uintptr_t)4 << ((ctr >> 16) & 15);
    if (el != 8 || *lk_cpu_count != 1 || line < 16 || line > 2048 ||
        *(const volatile uint32_t *)0xb500b904 != UINT32_C(0xa9bf7bfd) ||
        *(const volatile uint32_t *)0xb5093220 != UINT32_C(0xa9bd7bfd) ||
        *(const volatile uint32_t *)0xb5093224 != UINT32_C(0x52800081) ||
        *(const volatile uint32_t *)0xb506ce54 != UINT32_C(0xa9bc7bfd) ||
        *(const volatile uint32_t *)0xb506cf70 != UINT32_C(0xa9bc7bfd)) {
        lk_printf("RAMBOOT-SEC: incompatible LK state EL=%lu cpu=%u line=%lu\n",
                  (unsigned long)el, *lk_cpu_count, (unsigned long)line);
        return -1;
    }
    for (unsigned i = 0; i < 4; ++i)
        if (lk_l2[0x14 + i] != (UINT64_C(0x0060000082800711) + ((uint64_t)i << 21))) {
            lk_printf("RAMBOOT-SEC: unexpected FD page table at %u\n", i);
            return -1;
        }
    total = *(const volatile uint32_t *)(uintptr_t)(DOWNLOAD_BASE + 4);
    if (total <= IMAGE_OFFSET || total > DOWNLOAD_LIMIT ||
        uefi_ramboot_inspect(boot, total - IMAGE_OFFSET, crc32, &image)) {
        lk_printf("RAMBOOT-SEC: invalid UEFI image\n");
        return -1;
    }
    entry = UEFI_RAMBOOT_FD_BASE + image.entry_offset;
    /* Legacy images include the shared pages in their erased FD tail.
     * New 2 MiB FDs end below them and reserve them independently in PEI. */
    for (uint32_t i = 0x7fe000; i < image.firmware_stored_size; ++i)
        if (image.firmware[i] != 0xff) {
            lk_printf("RAMBOOT-SEC: FD lacks reserved hardware handoff pages\n");
            return -1;
        }
    lk_printf("RAMBOOT-SEC: validated FD size=%u stored=%u SEC=%lx\n", image.firmware_size,
              image.firmware_stored_size,
              (unsigned long)entry);
    if (operation_is(operation, ":probe"))
        return 0;

#ifdef W202_LK_RETURN_ENABLED
    /* Some native Fastboot entries leave EIC164 masked (0x0b). Match the
     * resident bootstrap's input preparation so KeysInit can install all
     * three keys. Preserve the inherited mask when returning to native LK. */
    key_mask=adi_read(UINT32_C(0x64420284));
    adi_write(UINT32_C(0x64420284),key_mask|0x10,1);
    if (!(adi_read(UINT32_C(0x64420284))&0x10)) {
        adi_write(UINT32_C(0x64420284),key_mask,1);
        return -1;
    }
#endif
    saved = adi_read(POR_RST_MONITOR);
    adi_write(POR_RST_MONITOR, (saved & ~UINT32_C(0xff)) | FASTBOOT_REBOOT, 1);
    verify = adi_read(POR_RST_MONITOR);
    if ((verify & UINT32_C(0xff)) != FASTBOOT_REBOOT) {
        adi_write(POR_RST_MONITOR, saved, 1);
#ifdef W202_LK_RETURN_ENABLED
        adi_write(UINT32_C(0x64420284),key_mask,1);
#endif
        return -1;
    }
    lk_printf("RAMBOOT-SEC: arming %u ms recovery watchdog\n", W202_RECOVERY_WATCHDOG_MS);
    start_watchdog(W202_RECOVERY_WATCHDOG_MS);

    for (uint32_t i = 0; i < image.firmware_size; ++i)
        dest[i] = i < image.firmware_stored_size ? image.firmware[i] : 0xff;
    expected_crc = (uint32_t)boot[2068] | (uint32_t)boot[2069] << 8 |
                   (uint32_t)boot[2070] << 16 | (uint32_t)boot[2071] << 24;
    if (crc32(0, (const uint8_t *)(uintptr_t)UEFI_RAMBOOT_FD_BASE,
              image.firmware_size) != expected_crc) {
        lk_printf("RAMBOOT-SEC: copied FD checksum failed; awaiting watchdog\n");
        for (;;)
            __asm__ volatile("wfe");
    }
    clean_lines(UEFI_RAMBOOT_FD_BASE, image.firmware_size, line);
    for (uint32_t i = 0; i < 0x2000; ++i)
        *(volatile uint8_t *)(uintptr_t)(W202_HANDOFF_ADDRESS + i) = 0xff;
    clean_lines(W202_HANDOFF_ADDRESS, 0x2000, line);
    W202_LOADER_HANDOFF record = {
        .magic=W202_HANDOFF_MAGIC, .version=2, .size=sizeof(record),
        .flags=W202_HANDOFF_ADI
    };
    if (operation_is(operation, ":resident"))
        record.flags |= W202_HANDOFF_RESIDENT_WATCHDOG;
    /* Verified stock read_vbat disassembly: (raw + offset) * 1000 / gain. */
    if (*(const volatile uint32_t *)0xb506fde4 == UINT32_C(0xa9be7bfd) &&
        *(const volatile uint32_t *)0xb506fe4c == UINT32_C(0x1ac10c00)) {
        int32_t gain=*(const volatile int32_t *)0xb50fccc0;
        int32_t offset=*(const volatile int32_t *)0xb50fccc4;
        if (gain>=500 && gain<=1500 && offset>=-1000 && offset<=1000) {
            record.voltage_gain=gain; record.voltage_offset=offset;
            record.flags|=W202_HANDOFF_CALIBRATION;
        }
    }
    /* Stock sprd_chan_batdet_adc_to_vol, not the uncalibrated image defaults.
     * Copy data while LK owns its address space; no LK calls from EDK2. */
    if (*(const volatile uint32_t *)0xb506cc6c == UINT32_C(0x90000485) &&
        *(const volatile uint32_t *)0xb506cc74 == UINT32_C(0x912de0a5) &&
        *(const volatile uint32_t *)0xb506cc90 == UINT32_C(0x910080a2)) {
        const volatile uint16_t *cal=(const volatile uint16_t *)0xb50fcb98;
        if (cal[0]==1400 && cal[2]==200 && cal[1]>=3000 && cal[1]<=4000 &&
            cal[3]>=200 && cal[3]<=800 && cal[4]==4) {
            record.ntc_high_mv=cal[0]; record.ntc_high_raw=cal[1];
            record.ntc_low_mv=cal[2]; record.ntc_low_raw=cal[3];
            record.flags|=W202_HANDOFF_BATDET_CALIBRATION;
        }
    }
    lk_printf("RAMBOOT-POWER: flags=%x NTC=(%u,%u),(%u,%u)\n",record.flags,
              record.ntc_high_mv,record.ntc_high_raw,record.ntc_low_mv,record.ntc_low_raw);
    record.checksum=0-W202HandoffSum(&record);
    *(volatile W202_LOADER_HANDOFF *)(uintptr_t)W202_HANDOFF_ADDRESS=record;
    clean_lines(W202_HANDOFF_ADDRESS,sizeof(record),line);
    /* Private timing record in the validated, reserved handoff page. */
    volatile uint64_t *metrics=(void *)(uintptr_t)(W202_HANDOFF_ADDRESS+0x100);
    metrics[0]=UINT64_C(0x57323032424f4f54); metrics[1]=start_counter;
    metrics[2]=frequency; metrics[3]=0;
    clean_lines(W202_HANDOFF_ADDRESS+0x100,32,line);
    __asm__ volatile("mrs %0, sctlr_el2" : "=r"(sctlr));
    __asm__ volatile("mrs %0, daif" : "=r"(daif));
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    trace[0] = TRACE_MAGIC;
    trace[1] = 0x10;
    trace[2] = el;
    trace[3] = sctlr;
    trace[4] = daif;
    trace[5] = sp;
    trace[6] = entry;
    trace[7] = 0;
    clean_lines(TRACE_BASE, 64, line);
    lk_printf("RAMBOOT-SEC: FD copied; entering native cleanup\n");
#ifdef W202_LK_RETURN_ENABLED
    int action=handoff_enter_sec_returnable(entry);
    lk_printf("LK-RETURN: restored action=%d\n",action);
    adi_write(POR_RST_MONITOR,saved,1);
    adi_write(UINT32_C(0x64420284),key_mask,1);
    if (action==W202_LK_RETURN_ANDROID) {
        volatile uint8_t *suffix=(void *)0xb50eed90;
        if (suffix[0]=='_' && suffix[1]=='b' && suffix[2]==0) {
            suffix[1]='a';
            *(volatile uintptr_t *)0xb50eef98=0;
            lk_printf("LK-RETURN: native Android A; RAM selection only\n");
#ifdef W202_LK_RETURN_DEFER_ANDROID
            lk_printf("LK-RETURN: diagnostic pause before native continue\n");
            ((void (*)(void))0xb50932a0)();
            return 0;
#else
            /* A cold resident has not initialized native Fastboot USB. */
            lk_return_android(operation_is(operation, ":resident")?
                              UINT64_C(0xb500da8c):UINT64_C(0xb500e2c8));
#endif
        }
        lk_printf("LK-RETURN: unexpected native suffix\n");
        action=-1;
    }
    ((void (*)(void))0xb50932a0)();
    return action==W202_LK_RETURN_PROBE?0:-1;
#else
    handoff_enter_sec(entry);
#endif
}
