// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef W202_LINUX_IMAGE_H
#define W202_LINUX_IMAGE_H
#include <Uefi.h>
#define LINUX_INITRD_BASE 0xd0000000ULL
#define LINUX_KERNEL_BASE 0xd4000000ULL
#define LINUX_DTB_BASE 0xd8000000ULL
#define LINUX_DTB_CAPACITY 0x200000ULL
typedef struct {
  UINT64 KernelBytes, RamdiskOffset, RamdiskBytes, Entry, MemoryBytes;
} LINUX_IMAGE;
EFI_STATUS LinuxParseImage(CONST UINT8 *Header, UINTN HeaderBytes,
                          CONST UINT8 *Kernel, UINTN KernelHeaderBytes,
                          UINT64 FileBytes, LINUX_IMAGE *Image);
#endif
