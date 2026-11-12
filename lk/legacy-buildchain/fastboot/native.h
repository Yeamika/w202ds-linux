/* SPDX-License-Identifier: MIT
 * ABI of the pinned uboot-mod.bin. Image validation checks the base SHA256.
 * These are raw operations: no dl_backup, slot selection or LCD calls.
 */
#ifndef LK_FASTBOOT_NATIVE_H
#define LK_FASTBOOT_NATIVE_H
#include <stdint.h>
static int (*const lk_raw_read)(const char *, uint64_t, uint64_t, void *) = (void *)0xb50405ec;
static int (*const lk_raw_write)(const char *, uint64_t, uint64_t, uint64_t, const void *) = (void *)0xb503f5d0;
static int (*const lk_partition_size)(const char *, uint64_t *) = (void *)0xb50403b8;
#endif
