// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "BootImage.h"
STATIC UINT32 Le32(CONST UINT8 *P) {
  return (UINT32)P[0]|(UINT32)P[1]<<8|(UINT32)P[2]<<16|(UINT32)P[3]<<24;
}
STATIC UINT64 Le64(CONST UINT8 *P) { return Le32(P)|((UINT64)Le32(P+4)<<32); }
STATIC BOOLEAN Magic(CONST UINT8 *P, CONST CHAR8 *S, UINTN N) {
  while (N--) if (*P++!=(UINT8)*S++) return FALSE;
  return TRUE;
}
STATIC UINT64 Page(UINT64 N) { return (N+4095)&~4095ULL; }
EFI_STATUS W202ParseBoot(CONST UINT8 *H, UINTN Bytes, UINT64 PartitionBytes, W202_BOOT_IMAGE *Image) {
  if (!H || !Image || Bytes<1584 || !Magic(H,"ANDROID!",8)) return EFI_VOLUME_CORRUPTED;
  // Deliberately limited to the captured, uncompressed boot v4 layout.
  if (Le32(H+40)!=4 || Le32(H+20)!=1584 || Le32(H+1580)!=0) return EFI_UNSUPPORTED;
  Image->KernelOffset=4096; Image->KernelBytes=Le32(H+8);
  Image->RamdiskOffset=4096+Page(Image->KernelBytes); Image->RamdiskBytes=Le32(H+12);
  if (Image->KernelBytes<64 || Image->KernelBytes>W202_OS_KERNEL_CAPACITY ||
      !Image->RamdiskBytes || Image->RamdiskBytes>W202_OS_INITRD_CAPACITY ||
      Image->RamdiskOffset+Page(Image->RamdiskBytes)>PartitionBytes) return EFI_BAD_BUFFER_SIZE;
  return EFI_SUCCESS;
}
EFI_STATUS W202ParseVendor(CONST UINT8 *H, UINTN Bytes, UINT64 PartitionBytes, W202_VENDOR_IMAGE *Image) {
  UINT64 DtbBytes;
  if (!H || !Image || Bytes<2128 || !Magic(H,"VNDRBOOT",8)) return EFI_VOLUME_CORRUPTED;
  if (Le32(H+8)!=4 || Le32(H+12)!=4096 || Le32(H+2096)!=2128 ||
      Le32(H+2112)!=108 || Le32(H+2116)!=1 || Le32(H+2120)!=108 || Le32(H+2124)!=0)
    return EFI_UNSUPPORTED;
  Image->RamdiskOffset=4096; Image->RamdiskBytes=Le32(H+24); DtbBytes=Le32(H+2100);
  Image->TableOffset=4096+Page(Image->RamdiskBytes)+Page(DtbBytes);
  if (!Image->RamdiskBytes || Image->RamdiskBytes>W202_OS_INITRD_CAPACITY ||
      !DtbBytes || DtbBytes>W202_OS_DTB_CAPACITY || Image->TableOffset+4096>PartitionBytes)
    return EFI_BAD_BUFFER_SIZE;
  return EFI_SUCCESS;
}
EFI_STATUS W202ParseKernel(CONST UINT8 *H, UINTN Bytes, UINT64 FileBytes, W202_KERNEL_IMAGE *Image) {
  UINT64 Offset, Size, Flags;
  if (!H || !Image || Bytes<64 || Le32(H+56)!=0x644d5241) return EFI_VOLUME_CORRUPTED;
  Offset=Le64(H+8); Size=Le64(H+16); Flags=Le64(H+24);
  // Relocation requires bit 3; this board supports little-endian 4 KiB kernels.
  if (Flags!=10 || !Size || Size<FileBytes || Offset>=0x200000 ||
      Size>W202_OS_KERNEL_CAPACITY-Offset) return EFI_UNSUPPORTED;
  Image->Entry=W202_OS_KERNEL_BASE+Offset; Image->MemoryBytes=Size;
  return EFI_SUCCESS;
}
