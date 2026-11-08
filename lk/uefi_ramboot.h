/* SPDX-License-Identifier: MIT */
#ifndef SPRD_UEFI_RAMBOOT_H
#define SPRD_UEFI_RAMBOOT_H

#include <stddef.h>
#include <stdint.h>

#define UEFI_RAMBOOT_FD_BASE UINT64_C(0x82800000)
#define UEFI_RAMBOOT_FD_SIZE UINT32_C(0x00200000)
#define UEFI_RAMBOOT_LEGACY_FD_SIZE UINT32_C(0x00800000)
#define UEFI_RAMBOOT_REGION_SIZE UINT64_C(0x03800000)

struct uefi_ramboot_image {
	const uint8_t *firmware;
	uint32_t firmware_size;
	uint32_t entry_offset;
	/* Version 2 omits only an erased (0xff) suffix on storage. */
	uint32_t firmware_stored_size;
};

typedef unsigned long (*uefi_ramboot_crc_fn)(unsigned long, const unsigned char *, unsigned int);

/* Parse a downloaded Android v0 boot image containing a rebased EDK II FD. */
int uefi_ramboot_inspect(const void *data, uint64_t size,
			uefi_ramboot_crc_fn crc, struct uefi_ramboot_image *image);

#endif
