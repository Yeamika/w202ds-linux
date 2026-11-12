/* SPDX-License-Identifier: MIT */
#include "uefi_ramboot.h"
#include <string.h>

#define BOOT_PAGE 2048U
#define RAMBOOT_HEADER 32U
#define COMPACT_HEADER 40U
#define FV_HEADER_MIN 56U
#define FFS_HEADER 24U
#define TE_HEADER 40U

static uint16_t read16(const uint8_t *p)
{
	return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}

static uint32_t read24(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16;
}

static uint32_t read32(const uint8_t *p)
{
	return read24(p) | (uint32_t)p[3] << 24;
}

static uint64_t read64(const uint8_t *p)
{
	return (uint64_t)read32(p) | (uint64_t)read32(p + 4) << 32;
}

static int te_entry(const uint8_t *fd, uint32_t size, uint32_t pos,
		    uint32_t length, uint32_t *result)
{
	uint32_t stripped, entry, section_count, i, adjust;
	uint64_t actual_base, target;
	const uint8_t *te;

	if (length < TE_HEADER || pos > size || length > size - pos)
		return -1;
	te = fd + pos;
	if (read16(te) != 0x5a56 || read16(te + 2) != 0xaa64)
		return -1;
	section_count = te[4];
	stripped = read16(te + 6);
	entry = read32(te + 8);
	if (!section_count || section_count > 32 || stripped < TE_HEADER ||
	    TE_HEADER + section_count * 40U > length)
		return -1;
	adjust = stripped - TE_HEADER;
	if (entry < adjust || pos < adjust)
		return -1;
	actual_base = UEFI_RAMBOOT_FD_BASE + pos - adjust;
	if (read64(te + 16) != actual_base)
		return -1;
	target = (uint64_t)pos + entry - adjust;
	if (target >= size || target < pos || target - pos >= length || (target & 3))
		return -1;
	for (i = 0; i < section_count; ++i) {
		const uint8_t *section = te + TE_HEADER + i * 40U;
		uint32_t virtual_size = read32(section + 8);
		uint32_t address = read32(section + 12);
		uint32_t raw_size = read32(section + 16);
		uint32_t raw_offset = read32(section + 20);
		uint32_t flags = read32(section + 36);
		if (entry >= address && entry - address < virtual_size &&
		    entry - address < raw_size && (flags & UINT32_C(0x20000000)) &&
		    raw_offset == address) {
			*result = (uint32_t)target;
			return 0;
		}
	}
	return -1;
}

static int find_sec(const uint8_t *fd, uint32_t size, uint32_t fv_size,
		    uint32_t *entry)
{
	/* ArmPlatformPkg/PrePi/PeiUniCore.inf, encoded as an EFI_GUID. */
	static const uint8_t sec_guid[16] = {
		0x83,0x17,0x40,0x3e,0x94,0xcc,0xcd,0x4f,
		0x97,0xbc,0xbd,0x35,0xac,0x36,0x9d,0x2f
	};
	uint32_t header, offset, checksum = 0, i;

	if (size < FV_HEADER_MIN || memcmp(fd + 40, "_FVH", 4) ||
	    read64(fd + 32) != fv_size || size > fv_size)
		return -1;
	header = read16(fd + 48);
	if (header < FV_HEADER_MIN || header > size || (header & 1))
		return -1;
	for (i = 0; i < header; i += 2)
		checksum += read16(fd + i);
	if (checksum & 0xffff)
		return -1;
	offset = (header + 7U) & ~7U;
	while (offset <= size && size - offset >= FFS_HEADER) {
		const uint8_t *file = fd + offset;
		uint32_t file_size = read24(file + 20), section;
		if (file_size == 0xffffff || file_size < FFS_HEADER || file_size > size - offset)
			return -1;
		if (file[18] == 3 && !memcmp(file, sec_guid, sizeof(sec_guid))) {
			section = FFS_HEADER;
			while (section <= file_size && file_size - section >= 4) {
				uint32_t section_size = read24(file + section);
				if (section_size == 0xffffff || section_size < 4 || section_size > file_size - section)
					return -1;
				if (file[section + 3] == 0x12)
					return te_entry(fd, size, offset + section + 4, section_size - 4, entry);
				section = (section + section_size + 3U) & ~3U;
			}
			return -1;
		}
		offset = (offset + file_size + 7U) & ~7U;
	}
	return -1;
}

int uefi_ramboot_inspect(const void *data, uint64_t size,
			uefi_ramboot_crc_fn crc, struct uefi_ramboot_image *image)
{
	const uint8_t *boot = data, *descriptor, *fd;
	uint32_t kernel_size, fd_size, stored_size, header_size, version;
	uint32_t entry, fv_size, checksum, remaining, i;
	static const uint8_t erased[256] = {[0 ... 255] = 0xff};

	if (!boot || !crc || !image || size < BOOT_PAGE || memcmp(boot, "ANDROID!", 8) ||
	    read32(boot + 36) != BOOT_PAGE || read32(boot + 40) != 0 ||
	    read32(boot + 16) != 0 || read32(boot + 24) != 0)
		return -1;
	kernel_size = read32(boot + 8);
	if (kernel_size < RAMBOOT_HEADER || kernel_size > size - BOOT_PAGE)
		return -1;
	descriptor = boot + BOOT_PAGE;
	if (memcmp(descriptor, "W202UEFI", 8))
		return -1;
	version = read32(descriptor + 8);
	header_size = read32(descriptor + 12);
	fd_size = read32(descriptor + 16);
	if (fd_size != UEFI_RAMBOOT_FD_SIZE && fd_size != UEFI_RAMBOOT_LEGACY_FD_SIZE)
		return -1;
	if (version == 1 && header_size == RAMBOOT_HEADER) {
		if (read32(descriptor + 24) || read32(descriptor + 28))
			return -1;
		stored_size = fd_size;
	} else if (version == 2 && header_size == COMPACT_HEADER) {
		if (kernel_size < COMPACT_HEADER || read32(descriptor + 24) != 1 ||
		    read32(descriptor + 28))
			return -1;
		stored_size = read32(descriptor + 32);
	} else {
		return -1;
	}
	if (stored_size < FV_HEADER_MIN || stored_size > fd_size ||
	    kernel_size != header_size + stored_size)
		return -1;
	fd = descriptor + header_size;
	/* Legacy FDs may reserve two erased pages after the FV. New 2 MiB FDs
	 * keep the shared pages separately reserved in the RAM memory map. */
	if (read64(fd + 32) != fd_size && read64(fd + 32) != fd_size - 0x2000u)
		return -1;
	fv_size = (uint32_t)read64(fd + 32);
	for (i = fv_size; i < stored_size; ++i)
		if (fd[i] != 0xff)
			return -1;
	checksum = (uint32_t)crc(0, fd, stored_size);
	if (version == 2 && checksum != read32(descriptor + 36))
		return -1;
	/* Verify the complete logical image before touching the destination or
	 * hardware. The omitted bytes have exactly one permitted value. */
	remaining = fd_size - stored_size;
	while (remaining) {
		uint32_t count = remaining < sizeof(erased) ? remaining : sizeof(erased);
		checksum = (uint32_t)crc(checksum, erased, count);
		remaining -= count;
	}
	if (checksum != read32(descriptor + 20) ||
	    find_sec(fd, stored_size < fv_size ? stored_size : fv_size,
		     fv_size, &entry))
		return -1;
	image->firmware = fd;
	image->firmware_size = fd_size;
	image->entry_offset = entry;
	image->firmware_stored_size = stored_size;
	return 0;
}
