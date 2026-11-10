// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "LinuxImage.h"
STATIC UINT32 Le32(CONST UINT8 *P) {
  return (UINT32)P[0]|(UINT32)P[1]<<8|(UINT32)P[2]<<16|(UINT32)P[3]<<24;
}
STATIC UINT64 Le64(CONST UINT8 *P) { return Le32(P)|((UINT64)Le32(P+4)<<32); }
EFI_STATUS LinuxParseImage(CONST UINT8 *H, UINTN HBytes, CONST UINT8 *K,
                          UINTN KBytes, UINT64 FileBytes, LINUX_IMAGE *Out) {
  UINTN I; UINT64 Offset, Size;
  CONST CHAR8 Magic[]="ANDROID!";
  if (!H || !K || !Out || HBytes<1584 || KBytes<64) return EFI_INVALID_PARAMETER;
  for (I=0;I<8;++I) if (H[I]!=(UINT8)Magic[I]) return EFI_VOLUME_CORRUPTED;
  if (Le32(H+40)!=4 || Le32(H+20)!=1584 || Le32(H+1580)!=0 ||
      Le32(K+56)!=0x644d5241 || Le64(K+24)!=10) return EFI_UNSUPPORTED;
  Out->KernelBytes=Le32(H+8); Out->RamdiskBytes=Le32(H+12);
  Offset=Le64(K+8); Size=Le64(K+16);
  if (Offset>=0x200000 || Out->KernelBytes<64 || !Size || Size<Out->KernelBytes ||
      Size>0x4000000-Offset || !Out->RamdiskBytes || Out->RamdiskBytes>0x4000000)
    return EFI_BAD_BUFFER_SIZE;
  Out->RamdiskOffset=4096+((Out->KernelBytes+4095)&~4095ULL);
  if (Out->RamdiskOffset>FileBytes || Out->RamdiskBytes>FileBytes-Out->RamdiskOffset)
    return EFI_BAD_BUFFER_SIZE;
  Out->Entry=LINUX_KERNEL_BASE+Offset; Out->MemoryBytes=Size;
  return EFI_SUCCESS;
}
