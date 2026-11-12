/* SPDX-License-Identifier: MIT */
#include <stddef.h>
#include <stdint.h>
#include "uefi_ramboot.h"

#define DOWNLOAD_BASE UINT64_C(0x86000000)
#define IMAGE_OFFSET UINT32_C(0x10000)
#define DOWNLOAD_LIMIT UINT32_C(0x00ffffff)

static int (*const lk_printf)(const char *, ...) = (void *)0xb5035e94;

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *left = a, *right = b;
    for (size_t i = 0; i < n; ++i) {
        if (left[i] != right[i])
            return (int)left[i] - (int)right[i];
    }
    return 0;
}

static unsigned long crc32(unsigned long seed, const unsigned char *data,
                           unsigned int size)
{
    uint32_t crc = ~(uint32_t)seed;
    while (size--) {
        crc ^= *data++;
        for (unsigned i = 0; i < 8; ++i)
            crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & -(crc & 1));
    }
    return ~crc;
}

int ramboot_check(const char *operation)
{
    static const char expected[] = ":probe";
    const uint8_t *download = (const void *)(uintptr_t)DOWNLOAD_BASE;
    struct uefi_ramboot_image image;
    uint32_t total = *(const volatile uint32_t *)(uintptr_t)(DOWNLOAD_BASE + 4);

    if (!operation)
        return -1;
    for (unsigned i = 0; i < sizeof(expected); ++i)
        if (operation[i] != expected[i])
            return -1;
    if (total <= IMAGE_OFFSET || total > DOWNLOAD_LIMIT ||
        uefi_ramboot_inspect(download + IMAGE_OFFSET, total - IMAGE_OFFSET,
                             crc32, &image)) {
        lk_printf("RAMBOOT-CHECK: invalid UEFI image\n");
        return -1;
    }
    lk_printf("RAMBOOT-CHECK: accepted FD size=%u SEC=%lx\n",
              image.firmware_size,
              (unsigned long)(UEFI_RAMBOOT_FD_BASE + image.entry_offset));
    lk_printf("RAMBOOT-CHECK: validation only; SEC entry was not called\n");
    return 0;
}
